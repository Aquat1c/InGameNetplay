#include "netplay/bridge/netplay_state_export.h"
#include "netplay/bridge/netplay_state_snapshot.h"

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

#include "efz_netplay_state.h"
#include "netplay/bridge/async_hosting.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/battle_log_menu.h"
#include "netplay/core/options_menu.h"
#include "netplay/core/player_rooms_menu.h"
#include "netplay/hooks/internal/shared.h"
#include "logger.h"

namespace netplay::bridge::state_export
{

void UpdateNow(const NetbridgeStatus& status);

namespace
{
bool FormatConnectionEndpoint(
    const NetbridgeStatus& status,
    char* output,
    size_t outputSize)
{
    if (output == nullptr || outputSize == 0)
    {
        return false;
    }
    output[0] = '\0';

    // Keep allocation-free: this also runs on the gameplay export pulse.
    const size_t addressLength = std::strlen(status.address);
    const bool ipv6 = std::strchr(status.address, ':') != nullptr;
    char portText[6] = {};
    const int portLength = std::snprintf(
        portText,
        sizeof(portText),
        "%u",
        static_cast<unsigned>(status.port));
    if (portLength <= 0)
    {
        return false;
    }

    const size_t required =
        addressLength
        + static_cast<size_t>(portLength)
        + 1
        + (ipv6 ? 2 : 0)
        + 1;
    if (required > outputSize)
    {
        return false;
    }

    char* cursor = output;
    if (ipv6)
    {
        *cursor++ = '[';
    }
    std::memcpy(cursor, status.address, addressLength);
    cursor += addressLength;
    if (ipv6)
    {
        *cursor++ = ']';
    }
    *cursor++ = ':';
    std::memcpy(
        cursor,
        portText,
        static_cast<size_t>(portLength));
    cursor += portLength;
    *cursor = '\0';
    return true;
}
}

namespace
{
HANDLE g_shmHandle = nullptr;
EFZNetplayState* g_shmView = nullptr;
StateSnapshot g_snapshot;
std::thread g_updateWorker;
HANDLE g_updateEvent = nullptr;
CRITICAL_SECTION g_updateRequestLock = {};
bool g_updateRequestLockInitialized = false;
NetbridgeStatus g_pendingStatus = {};
volatile LONG g_updateRequestPending = 0;
volatile LONG g_updateWorkerStop = 0;
volatile LONG g_updateWorkerReady = 0;
volatile LONG g_updatePublishDropped = 0;
volatile LONG g_updateInFlight = 0;
volatile LONG g_updatesSuspended = 0;

uint8_t g_prevInNetplayMenu = 0;
uint8_t g_prevInNetplayCharacterSelect = 0;
uint8_t g_prevInNetplayMatch = 0;

uint8_t g_prevActivityPhase = 0;
uint8_t g_prevEndReason = 0;
int32_t g_prevSessionMode = 0;
int32_t g_prevSessionPhase = 0;

volatile LONG g_stateSeq = 0;
uint32_t g_sessionId = 0;
uint32_t g_setId = 0;

uint8_t g_latchedEndReason = EFZ_END_NONE;

uint32_t g_lastExportTickMs = 0;
constexpr uint32_t kExportStallWarningMs = 250;

int g_prevBridgePhase = static_cast<int>(NetbridgePhase::Idle);
int32_t g_prevP1Wins = 0;
int32_t g_prevP2Wins = 0;

// Live wins may transiently read 0 at screen transitions.
int32_t g_latchedP1Wins = 0;
int32_t g_latchedP2Wins = 0;

char g_prevLocalNickname[64] = {};
char g_prevP1Name[64] = {};
char g_prevP2Name[64] = {};

// Boundary reads may miss the session names; a set keeps its last good pair.
char g_latchedP1Name[64] = {};
char g_latchedP2Name[64] = {};

constexpr DWORD kSuspendedHeartbeatMs = 250;
constexpr uintptr_t kScreenIndexAddr = 0x00790148;

uint8_t ReadScreenIndex()
{
    __try
    {
        return *reinterpret_cast<const volatile uint8_t*>(kScreenIndexAddr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0xFF;
}

void ApplyOnlineFlowScreen(EFZNetplayState& s, uint8_t screenIdx)
{
    s.inNetplayMenu = 0;
    s.netplayMenuScreen = 0;
    s.netplayMenuDetail = EFZ_MENU_DETAIL_NONE;
    s.inNetplayCharacterSelect = (screenIdx == 1 || screenIdx == 2) ? 1 : 0;
    s.inNetplayMatch = screenIdx == 3 ? 1 : 0;
    s.activityPhase = static_cast<uint8_t>(
        screenIdx == 1
            ? EFZ_ACTIVITY_CHAR_SELECT
            : (screenIdx == 2 ? EFZ_ACTIVITY_LOADING : EFZ_ACTIVITY_MATCH));
    s.capabilityFlags &= ~static_cast<uint32_t>(EFZ_CAP_MENU);
    s.capabilityFlags |= EFZ_CAP_GAME_FLOW | EFZ_CAP_ACTIVITY;
}

// Runs beside the live simulation: never walk a game or session pointer.
void PublishSuspendedHeartbeat()
{
    static uint8_t s_lastActivity = 0xFF;
    const uint8_t screenIdx = ReadScreenIndex();
    const bool inOnlineFlow =
        screenIdx == 1 || screenIdx == 2 || screenIdx == 3;

    EnterCriticalSection(&g_updateRequestLock);
    EFZNetplayState s = {};
    (void)g_snapshot.Copy(&s, sizeof(s), EFZ_NETPLAY_STATE_VERSION);
    if (inOnlineFlow)
    {
        ApplyOnlineFlowScreen(s, screenIdx);
    }
    s.stateSeq = static_cast<uint32_t>(InterlockedIncrement(&g_stateSeq));
    s.lastUpdateTick = GetTickCount();
    if (g_shmView != nullptr)
    {
        std::memcpy(g_shmView, &s, sizeof(EFZNetplayState));
    }
    g_snapshot.Publish(s);
    g_lastExportTickMs = s.lastUpdateTick;
    LeaveCriticalSection(&g_updateRequestLock);

    if (s.activityPhase != s_lastActivity)
    {
        s_lastActivity = s.activityPhase;
        mod::Log(
            "StateExport: heartbeat activity=%u screen=%u seq=%u",
            static_cast<unsigned>(s.activityPhase),
            static_cast<unsigned>(screenIdx),
            s.stateSeq);
    }
}

int g_prevPingMs = -1;
int g_prevRollbackFrames = -1;

static constexpr int kPingWindowSize = 64;
static int g_pingWindow[kPingWindowSize] = {};
static int g_pingWindowPos = 0;
static int g_pingWindowCount = 0;
static int g_pingBaseline = -1;
static bool g_pingBaselineLocked = false;
static uint32_t g_pingDriftWarnings = 0;
static uint32_t g_lastPingDriftLogSeq = 0;

void ExportWorkerMain()
{
    mod::Log("StateExport: async worker started");

    while (g_updateEvent != nullptr)
    {
        const bool suspended =
            InterlockedCompareExchange(&g_updatesSuspended, 0, 0) != 0;
        const DWORD waitResult = WaitForSingleObject(
            g_updateEvent,
            suspended ? kSuspendedHeartbeatMs : INFINITE);
        if (waitResult != WAIT_OBJECT_0)
        {
            if (InterlockedCompareExchange(&g_updateWorkerStop, 0, 0) != 0)
            {
                break;
            }
            if (waitResult == WAIT_TIMEOUT
                && InterlockedCompareExchange(&g_updatesSuspended, 0, 0) != 0)
            {
                PublishSuspendedHeartbeat();
            }
            continue;
        }

        if (InterlockedCompareExchange(&g_updateWorkerStop, 0, 0) != 0)
        {
            break;
        }

        for (;;)
        {
            InterlockedIncrement(&g_updateInFlight);
            if (InterlockedExchange(&g_updateRequestPending, 0) == 0)
            {
                InterlockedDecrement(&g_updateInFlight);
                break;
            }

            if (!g_updateRequestLockInitialized)
            {
                InterlockedDecrement(&g_updateInFlight);
                break;
            }

            NetbridgeStatus snapshot = {};
            EnterCriticalSection(&g_updateRequestLock);
            snapshot = g_pendingStatus;
            LeaveCriticalSection(&g_updateRequestLock);

            UpdateNow(snapshot);
            InterlockedDecrement(&g_updateInFlight);

            if (InterlockedCompareExchange(&g_updateWorkerStop, 0, 0) != 0)
            {
                break;
            }
        }
    }

    mod::Log("StateExport: async worker stopped");
}

void QueueUpdate(const NetbridgeStatus& status)
{
    if (InterlockedCompareExchange(&g_updatesSuspended, 0, 0) != 0)
    {
        return;
    }
    InterlockedIncrement(&g_updateInFlight);
    // Second check closes the race with suspension: a loser publishes nothing.
    if (InterlockedCompareExchange(&g_updatesSuspended, 0, 0) != 0)
    {
        InterlockedDecrement(&g_updateInFlight);
        return;
    }
    if (InterlockedCompareExchange(&g_updateWorkerReady, 0, 0) == 0
        || g_updateEvent == nullptr
        || !g_updateRequestLockInitialized)
    {
        UpdateNow(status);
        InterlockedDecrement(&g_updateInFlight);
        return;
    }

    if (!TryEnterCriticalSection(&g_updateRequestLock))
    {
        const LONG dropCount = InterlockedIncrement(&g_updatePublishDropped);
        if (dropCount <= 10 || (dropCount % 300) == 0)
        {
            MOD_LIFECYCLE_TRACE(
                "StateExport: async queue skipped due contention dropCount=%ld",
                static_cast<long>(dropCount));
        }
        InterlockedDecrement(&g_updateInFlight);
        return;
    }

    g_pendingStatus = status;
    // Publish under the lock: the suspension barrier crosses this same lock.
    InterlockedExchange(&g_updateRequestPending, 1);
    LeaveCriticalSection(&g_updateRequestLock);
    InterlockedDecrement(&g_updateInFlight);

    (void)SetEvent(g_updateEvent);
}

uint8_t g_prevP1CharId = 0xFF;
uint8_t g_prevP2CharId = 0xFF;
uint8_t g_prevP1Locked = 0;
uint8_t g_prevP2Locked = 0;

// EFZ.exe game-system offsets; must stay in sync with the hooks layer.
constexpr uintptr_t kScreenTableAddr = 0x00790110;
constexpr uint32_t kOffsetGameSystemInScreen = 0x1C;
constexpr uint32_t kGameSystemOffsetMatchCtr = 4952;

constexpr uint32_t kCharSelectP1CharId = 1340;
constexpr uint32_t kCharSelectP2CharId = 1341;
constexpr uint32_t kCharSelectP1Timer  = 1344;  // uint16_t - Random spinner, later the stage timer
constexpr uint32_t kCharSelectP2Timer  = 1346;
constexpr uint32_t kCharSelectP1State  = 1182;  // +side. 0 navigating, 5 intro, 1-3 picked, 6+ and 4 locked
constexpr uint32_t kCharSelectP1GridCol = 1336;
constexpr uint32_t kCharSelectP1GridRow = 1338;
constexpr uint32_t kCharSelectP2GridCol = 1337;
constexpr uint32_t kCharSelectP2GridRow = 1339;
constexpr uint32_t kCharSelectGridMap   = 1209;  // charId = gridMap[row*3 + col]
}

namespace
{
static uint32_t ReadScreenObject(uint8_t screenIdx)
{
    __try
    {
        if (screenIdx >= 16)
            return 0;
        const uint32_t screenObj =
            reinterpret_cast<const uint32_t*>(kScreenTableAddr)[screenIdx];
        return screenObj;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

static uint32_t ReadGameSystemPtr()
{
    __try
    {
        const uint8_t screenIdx = *reinterpret_cast<const uint8_t*>(kScreenIndexAddr);
        if (screenIdx >= 16)
            return 0;

        const uint32_t screenObj =
            reinterpret_cast<const uint32_t*>(kScreenTableAddr)[screenIdx];
        if (screenObj == 0)
            return 0;

        return *reinterpret_cast<const uint32_t*>(screenObj + kOffsetGameSystemInScreen);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

static void ReadScores(int32_t& p1Wins, int32_t& p2Wins, int32_t& matchCtr)
{
    p1Wins = 0;
    p2Wins = 0;
    matchCtr = 0;

    const uint32_t gameSys = ReadGameSystemPtr();
    if (gameSys == 0)
        return;

    __try
    {
        matchCtr = static_cast<int32_t>(
            *reinterpret_cast<const uint8_t*>(gameSys + kGameSystemOffsetMatchCtr));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        matchCtr = 0;
    }
}

// Printed as text: the numeric value clashes with Revival's role global.
static const char* NetplayRoleName(int netplayRole)
{
    using namespace netplay::bridge::takeover;
    switch (netplayRole)
    {
    case kNetplayRoleNone:      return "none";
    case kNetplayRoleHost:      return "host";
    case kNetplayRoleClient:    return "client";
    case kNetplayRoleSpectator: return "spectator";
    default:                    return "unknown";
    }
}

static int32_t ResolveSessionMode(int netplayRole, int localRoleFlag, int bridgeRole)
{
    using namespace netplay::bridge::takeover;
    switch (netplayRole)
    {
    case kNetplayRoleHost:      return EFZ_SESSION_HOSTING;
    case kNetplayRoleClient:    return EFZ_SESSION_JOINING;
    case kNetplayRoleSpectator: return EFZ_SESSION_SPECTATING;
    default:
        break;
    }

    // g_netplayRole may still be None during connecting: use the intended role.
    if (netplayRole == kNetplayRoleNone)
    {
        switch (static_cast<NetbridgeRole>(bridgeRole))
        {
        case NetbridgeRole::Host:          return EFZ_SESSION_HOSTING;
        case NetbridgeRole::Join:          return EFZ_SESSION_JOINING;
        case NetbridgeRole::Spectate:      return EFZ_SESSION_SPECTATING;
        case NetbridgeRole::JoinSpectate:  return EFZ_SESSION_JOINING;
        default: break;
        }
    }

    if (localRoleFlag == kLocalRoleTournament)
        return EFZ_SESSION_TOURNAMENT;

    return EFZ_SESSION_NONE;
}

static int32_t ResolveLocalSide(int netplayRole, int activePlayer)
{
    if (activePlayer == 0 || activePlayer == 1)
        return activePlayer;

    using namespace netplay::bridge::takeover;
    switch (netplayRole)
    {
    case kNetplayRoleHost:   return 0;
    case kNetplayRoleClient: return 1;
    default:                 return -1;
    }
}

static bool ReadCharSelectContext(
    uint8_t& p1CharId, uint8_t& p2CharId,
    uint8_t& p1Locked, uint8_t& p2Locked,
    uint8_t& localCursorCharId, int32_t localSide)
{
    p1CharId = 0xFF;
    p2CharId = 0xFF;
    p1Locked = 0;
    p2Locked = 0;
    localCursorCharId = 0xFF;

    const uint32_t csObj = ReadScreenObject(1);
    if (csObj == 0)
        return false;

    __try
    {
        const auto gridChar = [csObj](uint32_t colOffset, uint32_t rowOffset) -> uint8_t {
            const uint8_t col = *reinterpret_cast<const uint8_t*>(csObj + colOffset);
            const uint8_t row = *reinterpret_cast<const uint8_t*>(csObj + rowOffset);
            if (col >= 3 || row >= 8)
                return 0xFF;
            // The Random slot holds -1, which reads as the unknown sentinel.
            return *reinterpret_cast<const uint8_t*>(csObj + kCharSelectGridMap + row * 3 + col);
        };
        const auto sideChar = [csObj, &gridChar](
                                  uint32_t stateOffset,
                                  uint32_t timerOffset,
                                  uint32_t charOffset,
                                  uint32_t colOffset,
                                  uint32_t rowOffset) -> uint8_t {
            const uint8_t state = *reinterpret_cast<const uint8_t*>(csObj + stateOffset);
            if (state != 0 && state != 5)
                return *reinterpret_cast<const uint8_t*>(csObj + charOffset);
            // +1340 is stale until the cursor moves, and a running spinner drives the cursor.
            if (*reinterpret_cast<const int16_t*>(csObj + timerOffset) > 0)
                return 0xFF;
            return gridChar(colOffset, rowOffset);
        };

        p1CharId = sideChar(kCharSelectP1State, kCharSelectP1Timer,
            kCharSelectP1CharId, kCharSelectP1GridCol, kCharSelectP1GridRow);
        p2CharId = sideChar(kCharSelectP1State + 1, kCharSelectP2Timer,
            kCharSelectP2CharId, kCharSelectP2GridCol, kCharSelectP2GridRow);

        const uint8_t p1State = *reinterpret_cast<const uint8_t*>(csObj + kCharSelectP1State);
        const uint8_t p2State = *reinterpret_cast<const uint8_t*>(csObj + kCharSelectP1State + 1);
        p1Locked = (p1State == 4 || p1State >= 6) ? 1 : 0;
        p2Locked = (p2State == 4 || p2State >= 6) ? 1 : 0;

        if (localSide == 0)
            localCursorCharId = gridChar(kCharSelectP1GridCol, kCharSelectP1GridRow);
        else if (localSide == 1)
            localCursorCharId = gridChar(kCharSelectP2GridCol, kCharSelectP2GridRow);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static uint8_t DeriveEndReason(const NetbridgeStatus& status, int prevPhase)
{
    const auto phase = static_cast<NetbridgePhase>(status.phase);
    const auto prev  = static_cast<NetbridgePhase>(prevPhase);

    if (phase != NetbridgePhase::Failed && phase != NetbridgePhase::SessionEnded)
        return EFZ_END_NONE;
    if (prev == NetbridgePhase::Failed || prev == NetbridgePhase::SessionEnded)
        return EFZ_END_NONE;

    const char* err = status.errorMsg;

    if (phase == NetbridgePhase::Failed)
    {
        if (std::strstr(err, "process ended") != nullptr)
            return EFZ_END_PEER_PROCESS_DIED;
        if (std::strstr(err, "start canceled") != nullptr)
            return EFZ_END_CANCELLED;
        return EFZ_END_CONNECT_FAILED;
    }

    if (std::strstr(err, "peer disconnected") != nullptr)
        return EFZ_END_DISCONNECT;
    if (std::strstr(err, "process ended") != nullptr ||
        std::strstr(err, "process exited") != nullptr)
        return EFZ_END_PEER_PROCESS_DIED;

    return EFZ_END_GRACEFUL;
}

static uint8_t ResolveExportedMenuScreen(netplay::menu::NetplayMenuId menuId)
{
    switch (menuId)
    {
    case netplay::menu::NetplayMenuId::Main:
        return EFZ_MENU_MAIN;
    case netplay::menu::NetplayMenuId::Host:
        return EFZ_MENU_HOST;
    case netplay::menu::NetplayMenuId::Join:
        return EFZ_MENU_JOIN;
    case netplay::menu::NetplayMenuId::PlayerRooms:
        return EFZ_MENU_PLAYER_ROOMS;
    case netplay::menu::NetplayMenuId::Options:
        return EFZ_MENU_OPTIONS;
    case netplay::menu::NetplayMenuId::Lobby:
        return EFZ_MENU_LOBBY;
    case netplay::menu::NetplayMenuId::BattleLog:
        return EFZ_MENU_BATTLE_LOG;
    default:
        return EFZ_MENU_MAIN;
    }
}

static uint8_t ResolveExportedMenuDetail(
    const netplay::hooks::internal::NetplayMenuState& menu)
{
    if (!menu.active)
        return EFZ_MENU_DETAIL_NONE;

    switch (menu.menuId)
    {
    case netplay::menu::NetplayMenuId::PlayerRooms:
        return netplay::player_rooms::GetMenuDetailForStateExport();
    case netplay::menu::NetplayMenuId::Options:
        return netplay::options::GetMenuDetailForStateExport();
    case netplay::menu::NetplayMenuId::BattleLog:
        return netplay::battle_log::GetMenuDetailForStateExport();
    default:
        return EFZ_MENU_DETAIL_NONE;
    }
}
}

void Initialize()
{
    g_shmHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(EFZNetplayState),
        EFZ_NETPLAY_STATE_SHM_NAME);

    if (g_shmHandle != nullptr)
    {
        g_shmView = static_cast<EFZNetplayState*>(
            MapViewOfFile(g_shmHandle, FILE_MAP_WRITE, 0, 0, sizeof(EFZNetplayState)));

        if (g_shmView == nullptr)
        {
            mod::Log("StateExport: MapViewOfFile failed (err=%lu), closing orphaned handle",
                     static_cast<unsigned long>(GetLastError()));
            CloseHandle(g_shmHandle);
            g_shmHandle = nullptr;
        }
    }

    EFZNetplayState init = {};
    init.magic = EFZ_NETPLAY_STATE_MAGIC;
    init.version = EFZ_NETPLAY_STATE_VERSION;
    init.structSize = sizeof(EFZNetplayState);
    init.lastUpdateTick = GetTickCount();
    init.sessionMode = EFZ_SESSION_NONE;
    init.sessionPhase = EFZ_PHASE_IDLE;
    init.localSide = -1;
    init.pingMs = -1;
    init.rollbackFrames = -1;
    init.avgPingMs = -1;
    init.minPingMs = -1;
    init.maxPingMs = -1;
    init.recommendedDelay = -1;
    init.minDelay = -1;
    init.maxDelay = -1;

    if (g_shmView != nullptr)
    {
        std::memcpy(g_shmView, &init, sizeof(EFZNetplayState));
    }
    g_snapshot.Publish(init);

    InitializeCriticalSection(&g_updateRequestLock);
    g_updateRequestLockInitialized = true;
    g_pendingStatus = {};
    InterlockedExchange(&g_updateRequestPending, 0);
    InterlockedExchange(&g_updateWorkerStop, 0);
    InterlockedExchange(&g_updateWorkerReady, 0);
    InterlockedExchange(&g_updatePublishDropped, 0);
    InterlockedExchange(&g_updateInFlight, 0);
    InterlockedExchange(&g_updatesSuspended, 0);

    g_updateEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (g_updateEvent != nullptr)
    {
        try
        {
            g_updateWorker = std::thread(ExportWorkerMain);
            InterlockedExchange(&g_updateWorkerReady, 1);
        }
        catch (...)
        {
            CloseHandle(g_updateEvent);
            g_updateEvent = nullptr;
        }
    }

    mod::Log("StateExport: initialized (shm=%s, view=%p, async=%d)",
             g_shmHandle != nullptr ? "ok" : "FAIL",
             static_cast<void*>(g_shmView),
             InterlockedCompareExchange(&g_updateWorkerReady, 0, 0) != 0 ? 1 : 0);
}

void Shutdown()
{
    InterlockedExchange(&g_updateWorkerReady, 0);
    InterlockedExchange(&g_updateWorkerStop, 1);
    if (g_updateEvent != nullptr)
    {
        (void)SetEvent(g_updateEvent);
    }
    if (g_updateWorker.joinable())
    {
        g_updateWorker.join();
    }
    if (g_updateEvent != nullptr)
    {
        CloseHandle(g_updateEvent);
        g_updateEvent = nullptr;
    }
    InterlockedExchange(&g_updateRequestPending, 0);
    InterlockedExchange(&g_updatePublishDropped, 0);
    InterlockedExchange(&g_updatesSuspended, 1);
    g_pendingStatus = {};
    if (g_updateRequestLockInitialized)
    {
        DeleteCriticalSection(&g_updateRequestLock);
        g_updateRequestLockInitialized = false;
    }

    if (g_shmView != nullptr)
    {
        g_shmView->magic = 0;
        UnmapViewOfFile(g_shmView);
        g_shmView = nullptr;
    }
    if (g_shmHandle != nullptr)
    {
        CloseHandle(g_shmHandle);
        g_shmHandle = nullptr;
    }

    g_snapshot.Publish({});
    mod::Log("StateExport: shutdown");
}

void EmergencyShutdown()
{
    InterlockedExchange(&g_updateWorkerReady, 0);
    InterlockedExchange(&g_updateWorkerStop, 1);
    if (g_updateEvent != nullptr)
    {
        (void)SetEvent(g_updateEvent);
    }
    if (g_updateWorker.joinable())
    {
        // Loader lock held: detach rather than join.
        g_updateWorker.detach();
    }
}

void UpdateNow(const NetbridgeStatus& status)
{
    LARGE_INTEGER updateQpcStart = {};
    QueryPerformanceCounter(&updateQpcStart);

    EFZNetplayState s = {};

    s.magic = EFZ_NETPLAY_STATE_MAGIC;
    s.version = EFZ_NETPLAY_STATE_VERSION;
    s.structSize = sizeof(EFZNetplayState);
    s.lastUpdateTick = GetTickCount();

    {
        const uint32_t now = GetTickCount();
        const bool sessionActive =
            static_cast<NetbridgePhase>(status.phase) != NetbridgePhase::Idle &&
            static_cast<NetbridgePhase>(status.phase) != NetbridgePhase::Failed &&
            static_cast<NetbridgePhase>(status.phase) != NetbridgePhase::SessionEnded;
        if (sessionActive
            && g_lastExportTickMs != 0
            && InterlockedCompareExchange(&g_updatesSuspended, 0, 0) == 0)
        {
            const uint32_t elapsed = now - g_lastExportTickMs;
            if (elapsed > kExportStallWarningMs)
            {
                mod::Log(
                    "StateExport: STALL detected - %u ms since last update "
                    "(seq=%u phase=%d)",
                    static_cast<unsigned>(elapsed),
                    g_stateSeq,
                    status.phase);
            }
        }
    }

    s.stateSeq = static_cast<uint32_t>(InterlockedIncrement(&g_stateSeq));

    s.sessionMode = ResolveSessionMode(
        takeover::g_netplayRole, takeover::g_localRoleFlag, status.role);
    s.sessionPhase = status.phase;
    s.localSide = ResolveLocalSide(takeover::g_netplayRole, status.activePlayer);

    {
        const auto curPhase = static_cast<NetbridgePhase>(status.phase);
        const auto prevPhase = static_cast<NetbridgePhase>(g_prevBridgePhase);

        if (curPhase == NetbridgePhase::Connecting
            && (prevPhase == NetbridgePhase::Idle
                || prevPhase == NetbridgePhase::Failed
                || prevPhase == NetbridgePhase::SessionEnded))
        {
            ++g_sessionId;
            g_latchedEndReason = EFZ_END_NONE;
            g_latchedP1Wins = 0;
            g_latchedP2Wins = 0;
            g_prevP1Wins = 0;
            g_prevP2Wins = 0;
            g_latchedP1Name[0] = '\0';
            g_latchedP2Name[0] = '\0';
            g_pingWindowPos = 0;
            g_pingWindowCount = 0;
            g_pingBaseline = -1;
            g_pingBaselineLocked = false;
            g_pingDriftWarnings = 0;
            g_lastPingDriftLogSeq = 0;
            mod::Log("StateExport: new session sessionId=%u", g_sessionId);
        }

        const uint8_t newEnd = DeriveEndReason(status, g_prevBridgePhase);
        if (newEnd != EFZ_END_NONE)
        {
            g_latchedEndReason = newEnd;
            mod::Log("StateExport: session ended endReason=%u err='%s'",
                     static_cast<unsigned>(newEnd), status.errorMsg);
        }

        g_prevBridgePhase = status.phase;
    }

    s.sessionId = g_sessionId;
    s.endReason = g_latchedEndReason;

    uint32_t caps = 0;

    caps |= EFZ_CAP_SESSION;

    {
        int32_t liveP1 = status.sessionP1Wins;
        int32_t liveP2 = status.sessionP2Wins;

        int32_t gsP1Unused = 0, gsP2Unused = 0, gsMatch = 0;
        ReadScores(gsP1Unused, gsP2Unused, gsMatch);
        s.matchCounter = gsMatch;

        const int32_t liveTotal = liveP1 + liveP2;
        const int32_t latchTotal = g_latchedP1Wins + g_latchedP2Wins;
        if (liveTotal > latchTotal)
        {
            g_latchedP1Wins = liveP1;
            g_latchedP2Wins = liveP2;
        }

        if (g_latchedP1Wins + g_latchedP2Wins > liveP1 + liveP2)
        {
            s.p1Wins = g_latchedP1Wins;
            s.p2Wins = g_latchedP2Wins;
        }
        else
        {
            s.p1Wins = liveP1;
            s.p2Wins = liveP2;
        }
    }
    if (status.sessionScoresValid != 0)
        caps |= EFZ_CAP_SCORES;

    if ((g_prevP1Wins != 0 || g_prevP2Wins != 0) &&
        s.p1Wins == 0 && s.p2Wins == 0)
    {
        ++g_setId;
        mod::Log("StateExport: new set setId=%u", g_setId);
    }
    else if (s.p1Wins != g_prevP1Wins || s.p2Wins != g_prevP2Wins)
    {
        mod::Log(
            "StateExport: wins %d-%d -> %d-%d seq=%u",
            g_prevP1Wins, g_prevP2Wins,
            s.p1Wins,     s.p2Wins,
            s.stateSeq);
    }
    g_prevP1Wins = s.p1Wins;
    g_prevP2Wins = s.p2Wins;
    s.setId = g_setId;

    std::memcpy(s.localNickname, status.nickname, sizeof(s.localNickname));
    std::memcpy(s.p1Name, status.p1Name, sizeof(s.p1Name));
    std::memcpy(s.p2Name, status.p2Name, sizeof(s.p2Name));
    if (status.sessionNamesValid != 0)
    {
        std::memcpy(g_latchedP1Name, s.p1Name, sizeof(g_latchedP1Name));
        std::memcpy(g_latchedP2Name, s.p2Name, sizeof(g_latchedP2Name));
    }
    else if (InterlockedCompareExchange(&g_updatesSuspended, 0, 0) != 0)
    {
        std::memcpy(s.p1Name, g_latchedP1Name, sizeof(s.p1Name));
        std::memcpy(s.p2Name, g_latchedP2Name, sizeof(s.p2Name));
    }
    if (status.sessionNamesValid != 0 || s.localNickname[0] != '\0')
        caps |= EFZ_CAP_NICKNAMES;

    if (std::strncmp(s.localNickname, g_prevLocalNickname, sizeof(s.localNickname)) != 0
        || std::strncmp(s.p1Name, g_prevP1Name, sizeof(s.p1Name)) != 0
        || std::strncmp(s.p2Name, g_prevP2Name, sizeof(s.p2Name)) != 0)
    {
        mod::Log(
            "StateExport: nicknames local='%s'->'%s' p1='%s'->'%s' p2='%s'->'%s' seq=%u",
            g_prevLocalNickname, s.localNickname,
            g_prevP1Name,        s.p1Name,
            g_prevP2Name,        s.p2Name,
            s.stateSeq);
        std::memcpy(g_prevLocalNickname, s.localNickname, sizeof(g_prevLocalNickname));
        std::memcpy(g_prevP1Name,        s.p1Name,        sizeof(g_prevP1Name));
        std::memcpy(g_prevP2Name,        s.p2Name,        sizeof(g_prevP2Name));
    }

    s.pingMs = status.pingMs;
    s.rollbackFrames = status.rollbackFrames;
    if (s.pingMs >= 0 || s.rollbackFrames >= 0)
        caps |= EFZ_CAP_NETWORK;

    if (s.pingMs != g_prevPingMs || s.rollbackFrames != g_prevRollbackFrames)
    {
        mod::Log(
            "StateExport: network ping=%d->%d delay=%d->%d seq=%u",
            g_prevPingMs,         s.pingMs,
            g_prevRollbackFrames, s.rollbackFrames,
            s.stateSeq);
        g_prevPingMs         = s.pingMs;
        g_prevRollbackFrames = s.rollbackFrames;
    }

    if (s.pingMs > 0)
    {
        g_pingWindow[g_pingWindowPos] = s.pingMs;
        g_pingWindowPos = (g_pingWindowPos + 1) % kPingWindowSize;
        if (g_pingWindowCount < kPingWindowSize)
            ++g_pingWindowCount;

        if (g_pingWindowCount == kPingWindowSize)
        {
            int sum = 0;
            for (int pi = 0; pi < kPingWindowSize; ++pi)
                sum += g_pingWindow[pi];
            const int rollingAvg = sum / kPingWindowSize;

            if (!g_pingBaselineLocked)
            {
                g_pingBaseline = rollingAvg;
                g_pingBaselineLocked = true;
                mod::Log(
                    "PING_TRACK: baseline established avg=%dms seq=%u",
                    g_pingBaseline, s.stateSeq);
            }
            else if (g_pingBaseline > 0)
            {
                const int drift = rollingAvg - g_pingBaseline;
                const int driftPct = (drift * 100) / g_pingBaseline;
                if ((driftPct > 25 || driftPct < -25)
                    && (g_pingDriftWarnings < 10
                        || (s.stateSeq - g_lastPingDriftLogSeq > 5000)))
                {
                    ++g_pingDriftWarnings;
                    g_lastPingDriftLogSeq = s.stateSeq;
                    mod::Log(
                        "PING_TRACK: *** DRIFT *** baseline=%dms current=%dms "
                        "drift=%+d%%  warnings=%u seq=%u",
                        g_pingBaseline, rollingAvg, driftPct,
                        g_pingDriftWarnings, s.stateSeq);
                }
            }
        }
    }

    const uint8_t rawScreenIdx = ReadScreenIndex();
    const uint8_t screenIdx = rawScreenIdx == 0xFF ? 0 : rawScreenIdx;

    {
        const auto& menu = netplay::hooks::internal::g_netplayMenuState;
        s.inNetplayMenu = menu.active ? 1 : 0;
        s.netplayMenuScreen = menu.active
            ? ResolveExportedMenuScreen(menu.menuId)
            : 0;
        s.netplayMenuDetail = menu.active
            ? ResolveExportedMenuDetail(menu)
            : EFZ_MENU_DETAIL_NONE;
        if (s.inNetplayMenu)
            caps |= EFZ_CAP_MENU;
    }

    if (s.inNetplayMenu && screenIdx != 0)
    {
        mod::Log(
            "StateExport: reconcile - inNetplayMenu=1 but screenIdx=%u, clearing",
            static_cast<unsigned>(screenIdx));
        s.inNetplayMenu = 0;
        s.netplayMenuScreen = 0;
        s.netplayMenuDetail = EFZ_MENU_DETAIL_NONE;
        caps &= ~EFZ_CAP_MENU;
    }

    s.revivalVersion[0] = '\0';
    if (takeover::g_activeRevival != nullptr
        && takeover::g_activeRevival->versionTag != nullptr)
    {
#if defined(_MSC_VER)
        strncpy_s(s.revivalVersion, sizeof(s.revivalVersion),
                  takeover::g_activeRevival->versionTag, _TRUNCATE);
#else
        std::snprintf(s.revivalVersion, sizeof(s.revivalVersion),
                      "%s", takeover::g_activeRevival->versionTag);
#endif
        if (s.revivalVersion[0] != '\0')
            caps |= EFZ_CAP_REVIVAL;
    }

    // 1=charselect, 2=loading, 3=battle.
    {
        // Launcher-adopted sessions suspend without setting the return flag.
        const bool inOnlineFlow =
            netplay::hooks::internal::g_returnToNetplayAfterMatch
            || InterlockedCompareExchange(&g_updatesSuspended, 0, 0) != 0;

        s.inNetplayCharacterSelect = 0;
        s.inNetplayMatch = 0;

        if (inOnlineFlow && !s.inNetplayMenu)
        {
            if (screenIdx == 1 || screenIdx == 2)
                s.inNetplayCharacterSelect = 1;
            else if (screenIdx == 3)
                s.inNetplayMatch = 1;
        }

        if (s.inNetplayCharacterSelect || s.inNetplayMatch)
            caps |= EFZ_CAP_GAME_FLOW;
    }

    {
        const auto phase = static_cast<NetbridgePhase>(status.phase);

        if (s.inNetplayMenu)
        {
            s.activityPhase = EFZ_ACTIVITY_MENU;
        }
        else if (phase == NetbridgePhase::Connecting)
        {
            s.activityPhase = EFZ_ACTIVITY_CONNECTING;
        }
        else if (phase == NetbridgePhase::DelaySetup)
        {
            s.activityPhase = EFZ_ACTIVITY_DELAY_SETUP;
        }
        else if (s.inNetplayCharacterSelect)
        {
            if (screenIdx == 2)
                s.activityPhase = EFZ_ACTIVITY_LOADING;
            else
                s.activityPhase = EFZ_ACTIVITY_CHAR_SELECT;
        }
        else if (s.inNetplayMatch)
        {
            s.activityPhase = EFZ_ACTIVITY_MATCH;
        }
        else
        {
            s.activityPhase = EFZ_ACTIVITY_IDLE;
        }

        if (s.activityPhase != EFZ_ACTIVITY_IDLE)
            caps |= EFZ_CAP_ACTIVITY;
    }

    s.p1CharId = 0xFF;
    s.p2CharId = 0xFF;
    s.p1Locked = 0;
    s.p2Locked = 0;
    s.localCursorCharId = 0xFF;

    if (s.activityPhase == EFZ_ACTIVITY_CHAR_SELECT ||
        s.activityPhase == EFZ_ACTIVITY_LOADING)
    {
        if (ReadCharSelectContext(
                s.p1CharId, s.p2CharId,
                s.p1Locked, s.p2Locked,
                s.localCursorCharId, s.localSide))
        {
            caps |= EFZ_CAP_CHAR_SELECT;
        }
    }

    if (s.p1CharId != g_prevP1CharId || s.p2CharId != g_prevP2CharId
        || s.p1Locked != g_prevP1Locked || s.p2Locked != g_prevP2Locked)
    {
        mod::Log(
            "StateExport: charsel p1CharId=%u->%u locked=%u->%u "
            "p2CharId=%u->%u locked=%u->%u cursor=%u seq=%u",
            static_cast<unsigned>(g_prevP1CharId),   static_cast<unsigned>(s.p1CharId),
            static_cast<unsigned>(g_prevP1Locked),   static_cast<unsigned>(s.p1Locked),
            static_cast<unsigned>(g_prevP2CharId),   static_cast<unsigned>(s.p2CharId),
            static_cast<unsigned>(g_prevP2Locked),   static_cast<unsigned>(s.p2Locked),
            static_cast<unsigned>(s.localCursorCharId),
            s.stateSeq);
        g_prevP1CharId  = s.p1CharId;
        g_prevP2CharId  = s.p2CharId;
        g_prevP1Locked  = s.p1Locked;
        g_prevP2Locked  = s.p2Locked;
    }

    s.stageId = 0xFF;
    s.roundIndex = 0xFF;
    s.isRoundActive = 0;
    s.roundTimerFrames = 0xFFFF;

    if (s.activityPhase == EFZ_ACTIVITY_MATCH)
    {
        const uint32_t gameSys = ReadGameSystemPtr();
        if (gameSys != 0)
        {
            __try
            {
                s.roundIndex = static_cast<uint8_t>(
                    *reinterpret_cast<const uint8_t*>(
                        gameSys + kGameSystemOffsetMatchCtr));
                s.isRoundActive = 1;
                caps |= EFZ_CAP_MATCH_CONTEXT;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    {
        namespace ah = netplay::bridge::async_host;
        const bool active = ah::IsActive();
        s.asyncHostActive = active ? 1 : 0;
        s.asyncHostMinimized = ah::IsMinimized() ? 1 : 0;
        s.asyncHostPeerFound = ah::IsPeerFoundHeld() ? 1 : 0;
        s.asyncHostTimedOut = ah::IsTimedOut() ? 1 : 0;
        s.hostPort = active ? ah::HostPort() : 0;
        if (active)
        {
            caps |= EFZ_CAP_ASYNC_HOST;
            if (ah::IsMinimized()
                && !s.inNetplayMatch
                && !s.inNetplayCharacterSelect
                && !s.inNetplayMenu)
            {
                s.activityPhase = EFZ_ACTIVITY_HOST_IDLE;
                s.sessionMode = EFZ_SESSION_HOSTING;
                caps |= EFZ_CAP_ACTIVITY | EFZ_CAP_SESSION;
            }
        }
    }

    {
        const DelayPromptMetrics m = netplay::bridge::GetDelayPromptMetrics();
        s.avgPingMs = m.averagePingMs;
        s.minPingMs = m.minPingMs;
        s.maxPingMs = m.maxPingMs;
        s.recommendedDelay = m.recommendedDelay;
        s.minDelay = m.minDelay;
        s.maxDelay = m.maxDelay;
        if (m.serial != 0 && (m.averagePingMs >= 0 || m.recommendedDelay >= 0))
        {
            caps |= EFZ_CAP_NET_DETAIL;
        }
    }

    s.connectionAddress[0] = '\0';
    if (status.address[0] != '\0')
    {
        bool connectionAddressComplete = false;
        if (status.port != 0)
        {
            connectionAddressComplete = FormatConnectionEndpoint(
                status,
                s.connectionAddress,
                sizeof(s.connectionAddress));
        }
        else
        {
            const size_t rawLength = std::strlen(status.address);
            if (rawLength < sizeof(s.connectionAddress))
            {
                std::memcpy(
                    s.connectionAddress,
                    status.address,
                    rawLength + 1);
                connectionAddressComplete = true;
            }
        }
        if (connectionAddressComplete)
        {
            caps |= EFZ_CAP_CONNECTION;
        }
    }

    s.capabilityFlags = caps;

    {
        if (s.inNetplayMenu != g_prevInNetplayMenu
            || s.inNetplayCharacterSelect != g_prevInNetplayCharacterSelect
            || s.inNetplayMatch != g_prevInNetplayMatch)
        {
            mod::Log(
                "StateExport: flow menu=%u->%u charsel=%u->%u match=%u->%u "
                "screen=%u mode=%d phase=%d netRole=%s p1Wins=%d p2Wins=%d",
                static_cast<unsigned>(g_prevInNetplayMenu),
                static_cast<unsigned>(s.inNetplayMenu),
                static_cast<unsigned>(g_prevInNetplayCharacterSelect),
                static_cast<unsigned>(s.inNetplayCharacterSelect),
                static_cast<unsigned>(g_prevInNetplayMatch),
                static_cast<unsigned>(s.inNetplayMatch),
                static_cast<unsigned>(screenIdx),
                s.sessionMode,
                s.sessionPhase,
                NetplayRoleName(takeover::g_netplayRole),
                s.p1Wins,
                s.p2Wins);
            g_prevInNetplayMenu = s.inNetplayMenu;
            g_prevInNetplayCharacterSelect = s.inNetplayCharacterSelect;
            g_prevInNetplayMatch = s.inNetplayMatch;
        }

        if (s.activityPhase != g_prevActivityPhase)
        {
            mod::Log(
                "StateExport: activity %u->%u screen=%u mode=%d phase=%d netRole=%s side=%d "
                "p1='%s' p2='%s' caps=0x%X sessionId=%u setId=%u seq=%u",
                static_cast<unsigned>(g_prevActivityPhase),
                static_cast<unsigned>(s.activityPhase),
                static_cast<unsigned>(screenIdx),
                s.sessionMode,
                s.sessionPhase,
                NetplayRoleName(takeover::g_netplayRole),
                s.localSide,
                s.p1Name,
                s.p2Name,
                s.capabilityFlags,
                s.sessionId,
                s.setId,
                s.stateSeq);
            if (s.activityPhase == EFZ_ACTIVITY_CHAR_SELECT ||
                s.activityPhase == EFZ_ACTIVITY_LOADING)
            {
                mod::Log(
                    "StateExport:   charsel p1CharId=%u p2CharId=%u "
                    "p1Locked=%u p2Locked=%u cursorChar=%u",
                    static_cast<unsigned>(s.p1CharId),
                    static_cast<unsigned>(s.p2CharId),
                    static_cast<unsigned>(s.p1Locked),
                    static_cast<unsigned>(s.p2Locked),
                    static_cast<unsigned>(s.localCursorCharId));
            }
            if (s.activityPhase == EFZ_ACTIVITY_MATCH)
            {
                mod::Log(
                    "StateExport:   match round=%u isRoundActive=%u "
                    "p1Wins=%d p2Wins=%d",
                    static_cast<unsigned>(s.roundIndex),
                    static_cast<unsigned>(s.isRoundActive),
                    s.p1Wins,
                    s.p2Wins);
            }
            g_prevActivityPhase = s.activityPhase;
        }
    }

    if (s.stateSeq == 1 || (s.stateSeq % 600) == 0)
    {
        MOD_LIFECYCLE_TRACE(
            "StateExport: heartbeat seq=%u activity=%u menu=%u screen=%u "
            "mode=%d phase=%d netRole=%s side=%d "
            "p1Wins=%d p2Wins=%d ping=%d delay=%d "
            "local='%s' p1='%s' p2='%s' revival='%s' "
            "caps=0x%X sessionId=%u setId=%u endReason=%u",
            s.stateSeq,
            static_cast<unsigned>(s.activityPhase),
            static_cast<unsigned>(s.inNetplayMenu),
            static_cast<unsigned>(screenIdx),
            s.sessionMode,
            s.sessionPhase,
            NetplayRoleName(takeover::g_netplayRole),
            s.localSide,
            s.p1Wins,
            s.p2Wins,
            s.pingMs,
            s.rollbackFrames,
            s.localNickname,
            s.p1Name,
            s.p2Name,
            s.revivalVersion,
            s.capabilityFlags,
            s.sessionId,
            s.setId,
            static_cast<unsigned>(s.endReason));
    }

    if (g_updateRequestLockInitialized)
    {
        EnterCriticalSection(&g_updateRequestLock);
    }
    s.stateSeq = static_cast<uint32_t>(InterlockedIncrement(&g_stateSeq));
    if (g_shmView != nullptr)
    {
        std::memcpy(g_shmView, &s, sizeof(EFZNetplayState));
    }
    g_snapshot.Publish(s);
    g_lastExportTickMs = s.lastUpdateTick;
    if (g_updateRequestLockInitialized)
    {
        LeaveCriticalSection(&g_updateRequestLock);
    }

    {
        static uint32_t s_updateSlowCount = 0;
        LARGE_INTEGER updateQpcEnd = {}, freq = {};
        QueryPerformanceCounter(&updateQpcEnd);
        QueryPerformanceFrequency(&freq);
        const double elapsedMs =
            static_cast<double>(updateQpcEnd.QuadPart - updateQpcStart.QuadPart)
            * 1000.0 / static_cast<double>(freq.QuadPart);
        if (elapsedMs > 2.0)
        {
            ++s_updateSlowCount;
            if (s_updateSlowCount <= 10 || (s_updateSlowCount % 200 == 0))
            {
                mod::Log(
                    "PERF_WARN: StateExport::Update took %.2fms "
                    "(slowCount=%u seq=%u) - export path is slow",
                    elapsedMs, s_updateSlowCount, s.stateSeq);
            }
        }
    }
}

void Update(const NetbridgeStatus& status)
{
    QueueUpdate(status);
}

bool SuspendForOnlineSimulation()
{
    InterlockedExchange(&g_updatesSuspended, 1);
    if (g_updateRequestLockInitialized)
    {
        EnterCriticalSection(&g_updateRequestLock);
        LeaveCriticalSection(&g_updateRequestLock);
    }
    if (g_updateEvent != nullptr)
    {
        (void)SetEvent(g_updateEvent);
    }

    const DWORD startTick = GetTickCount();
    for (;;)
    {
        // Read twice: a single split observation could falsely observe quiescence.
        const LONG inFlightBefore =
            InterlockedCompareExchange(&g_updateInFlight, 0, 0);
        const LONG pendingBefore =
            InterlockedCompareExchange(&g_updateRequestPending, 0, 0);
        const LONG inFlightAfter =
            InterlockedCompareExchange(&g_updateInFlight, 0, 0);
        const LONG pendingAfter =
            InterlockedCompareExchange(&g_updateRequestPending, 0, 0);
        if (inFlightBefore == 0
            && pendingBefore == 0
            && inFlightAfter == 0
            && pendingAfter == 0)
        {
            break;
        }

        if (GetTickCount() - startTick >= 2000u)
        {
            mod::Log(
                "StateExport: online suspension timed out with worker in flight");
            return false;
        }
        Sleep(0);
    }

    return true;
}

void ResumeControlPlaneUpdates()
{
    InterlockedExchange(&g_updatesSuspended, 0);
}

bool IsSuspendedForOnlineSimulation()
{
    return g_updateRequestLockInitialized
        && InterlockedCompareExchange(&g_updatesSuspended, 0, 0) != 0;
}

bool OnlineCharSelectChanged()
{
    if (!IsSuspendedForOnlineSimulation() || ReadScreenIndex() != 1)
    {
        return false;
    }
    uint8_t p1CharId = 0xFF, p2CharId = 0xFF, p1Locked = 0, p2Locked = 0, cursor = 0xFF;
    if (!ReadCharSelectContext(p1CharId, p2CharId, p1Locked, p2Locked, cursor, -1))
    {
        return false;
    }
    static uint32_t s_lastSelection = 0xFFFFFFFFu;
    const uint32_t selection = static_cast<uint32_t>(p1CharId)
        | (static_cast<uint32_t>(p2CharId) << 8)
        | (static_cast<uint32_t>(p1Locked) << 16)
        | (static_cast<uint32_t>(p2Locked) << 24);
    if (selection == s_lastSelection)
    {
        return false;
    }
    s_lastSelection = selection;
    return true;
}

void PublishOnlineBoundary(const NetbridgeStatus& status)
{
    UpdateNow(status);
}

void PublishTrainingHandoffBoundary(const NetbridgeStatus& status)
{
    if (g_updateRequestLockInitialized) EnterCriticalSection(&g_updateRequestLock);
    const uint32_t tick = GetTickCount();
    g_snapshot.PublishHandoff(status, tick,
        static_cast<uint32_t>(InterlockedIncrement(&g_stateSeq)));
    g_lastExportTickMs = tick;
    if (g_shmView != nullptr)
    {
        EFZNetplayState snapshot = {};
        if (g_snapshot.Copy(&snapshot, sizeof(snapshot), EFZ_NETPLAY_STATE_VERSION))
            std::memcpy(g_shmView, &snapshot, sizeof(snapshot));
    }
    if (g_updateRequestLockInitialized) LeaveCriticalSection(&g_updateRequestLock);
}

const EFZNetplayState* GetExportedState()
{
    return g_snapshot.LegacyPointer();
}

bool CopyExportedState(EFZNetplayState* output, uint32_t outputSize, uint32_t minimumVersion)
{
    return g_snapshot.Copy(output, outputSize, minimumVersion);
}

}
