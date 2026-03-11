// ===========================================================================
// EFZ Netplay State Export — Implementation
// ===========================================================================
//
// Creates a named shared memory block ("EFZNetplay_State") and populates it
// each tick from the bridge status and live game memory.
//
// Also exposes a DLL-export function EFZNetplay_GetState() so consumer mods
// can use GetProcAddress instead of shared memory if preferred.
//
// This module is intentionally self-contained.  It reads the fields it needs
// from the bridge status struct and raw EFZ game memory — no changes to
// existing game logic or hook flow are required.
// ===========================================================================
#include "netplay/bridge/netplay_state_export.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "efz_netplay_state.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/battle_log_menu.h"
#include "netplay/core/options_menu.h"
#include "netplay/core/player_rooms_menu.h"
#include "netplay/hooks/internal/shared.h"
#include "logger.h"

namespace netplay::bridge::state_export
{

// ---------------------------------------------------------------------------
// Module-local state
// ---------------------------------------------------------------------------
namespace
{
HANDLE g_shmHandle = nullptr;
EFZNetplayState* g_shmView = nullptr;
EFZNetplayState g_localCopy = {};   // returned via DLL export

// Previous-tick flag values for transition logging.
uint8_t g_prevInNetplayMenu = 0;
uint8_t g_prevInNetplayCharacterSelect = 0;
uint8_t g_prevInNetplayMatch = 0;

// Previous-tick v4 values for transition logging.
uint8_t g_prevActivityPhase = 0;
uint8_t g_prevEndReason = 0;
int32_t g_prevSessionMode = 0;
int32_t g_prevSessionPhase = 0;

// Monotonic counters (v4).
uint32_t g_stateSeq = 0;
uint32_t g_sessionId = 0;
uint32_t g_setId = 0;

// Latched end reason — persists until next session starts.
uint8_t g_latchedEndReason = EFZ_END_NONE;

// Last tick (GetTickCount) at which Update() completed a write.
// Used for stall detection: log a warning if >250 ms pass between updates
// while a session is active.
uint32_t g_lastExportTickMs = 0;
constexpr uint32_t kExportStallWarningMs = 250;

// Previous-tick phase for detecting transitions (session start / end).
int g_prevBridgePhase = static_cast<int>(NetbridgePhase::Idle);
// Previous-tick scores for detecting set transitions and win increments.
int32_t g_prevP1Wins = 0;
int32_t g_prevP2Wins = 0;

// High-water-mark latched wins.  These survive screen transitions where the
// live data sources (Revival session, EFZ game system) may temporarily read
// as 0 due to session-pointer validation gaps or game-system resets.
// Reset only on new session start.
int32_t g_latchedP1Wins = 0;
int32_t g_latchedP2Wins = 0;

// Previous-tick nicknames for change detection.
char g_prevLocalNickname[64] = {};
char g_prevP1Name[64] = {};
char g_prevP2Name[64] = {};

// Previous-tick network metrics for change detection.
int g_prevPingMs = -1;
int g_prevRollbackFrames = -1;

// Previous-tick charselect context for change detection.
uint8_t g_prevP1CharId = 0xFF;
uint8_t g_prevP2CharId = 0xFF;
uint8_t g_prevP1Locked = 0;
uint8_t g_prevP2Locked = 0;

// EFZ.exe game-system offsets (same values used by the hooks layer).
constexpr uintptr_t kScreenTableAddr = 0x00790110;
constexpr uintptr_t kScreenIndexAddr = 0x00790148;
constexpr uint32_t kOffsetGameSystemInScreen = 0x1C;
constexpr uint32_t kGameSystemOffsetMatchCtr = 4952;

// Character-select screen object offsets.
constexpr uint32_t kCharSelectP1CharId = 1340;
constexpr uint32_t kCharSelectP2CharId = 1341;
constexpr uint32_t kCharSelectP1Timer  = 1344;  // uint16_t — non-zero = locked
constexpr uint32_t kCharSelectP2Timer  = 1346;
constexpr uint32_t kCharSelectP1GridCol = 1336;
constexpr uint32_t kCharSelectP1GridRow = 1338;
constexpr uint32_t kCharSelectP2GridCol = 1337;
constexpr uint32_t kCharSelectP2GridRow = 1339;
constexpr uint32_t kCharSelectGridMap   = 1209;  // charId = gridMap[row*3 + col]
} // namespace

// ---------------------------------------------------------------------------
// Helpers — read game state from EFZ game memory (same process)
// ---------------------------------------------------------------------------
namespace
{
/// Returns the screen object pointer for the given screen index, or 0.
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

/// Returns the game-system pointer from the currently active screen context,
/// or 0 if unavailable.  SEH-guarded because the address space may not be
/// initialised yet at startup.
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

    // EFZ.exe does NOT have win counters — only Revival does.
    // We only read the match counter (round counter) from the game system.
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

// Map internal g_netplayRole → exported EFZNetplaySessionMode.
// Falls back to status.role (NetbridgeRole intent) during the connecting
// phase, before g_netplayRole is resolved by the init handshake.
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

    // g_netplayRole may still be kNetplayRoleNone during connecting phase.
    // Check the intended role from StartSession (NetbridgeRole).
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

    // Fall back to localRoleFlag to detect tournament.
    if (localRoleFlag == kLocalRoleTournament)
        return EFZ_SESSION_TOURNAMENT;

    return EFZ_SESSION_NONE;
}

// Map local side.  Prefer the session object's activePlayer field (0=P1,
// 1=P2) when available — it reflects the actual assignment after init.
// Fall back to role-based inference (host=P1, client=P2) during connecting.
static int32_t ResolveLocalSide(int netplayRole, int activePlayer)
{
    // activePlayer from the Revival session object is authoritative.
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

/// Read charselect context from the charselect screen object.
/// Returns true if the read succeeded (screen object was valid).
static bool ReadCharSelectContext(
    uint8_t& p1CharId, uint8_t& p2CharId,
    uint8_t& p1Locked, uint8_t& p2Locked,
    uint8_t& localCursorCharId, int32_t localSide)
{
    // Defaults: unavailable
    p1CharId = 0xFF;
    p2CharId = 0xFF;
    p1Locked = 0;
    p2Locked = 0;
    localCursorCharId = 0xFF;

    // Charselect is screen index 1.
    const uint32_t csObj = ReadScreenObject(1);
    if (csObj == 0)
        return false;

    __try
    {
        p1CharId = *reinterpret_cast<const uint8_t*>(csObj + kCharSelectP1CharId);
        p2CharId = *reinterpret_cast<const uint8_t*>(csObj + kCharSelectP2CharId);

        // A non-zero timer means the player has confirmed their pick.
        const uint16_t p1Timer =
            *reinterpret_cast<const uint16_t*>(csObj + kCharSelectP1Timer);
        const uint16_t p2Timer =
            *reinterpret_cast<const uint16_t*>(csObj + kCharSelectP2Timer);
        p1Locked = (p1Timer != 0) ? 1 : 0;
        p2Locked = (p2Timer != 0) ? 1 : 0;

        // Derive cursor char from grid position for the local player.
        // gridMap[row*3 + col] = charId
        const uint8_t* gridMap =
            reinterpret_cast<const uint8_t*>(csObj + kCharSelectGridMap);
        if (localSide == 0)
        {
            const uint8_t col =
                *reinterpret_cast<const uint8_t*>(csObj + kCharSelectP1GridCol);
            const uint8_t row =
                *reinterpret_cast<const uint8_t*>(csObj + kCharSelectP1GridRow);
            localCursorCharId = gridMap[row * 3 + col];
        }
        else if (localSide == 1)
        {
            const uint8_t col =
                *reinterpret_cast<const uint8_t*>(csObj + kCharSelectP2GridCol);
            const uint8_t row =
                *reinterpret_cast<const uint8_t*>(csObj + kCharSelectP2GridRow);
            localCursorCharId = gridMap[row * 3 + col];
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

/// Derive end reason from bridge phase transitions and error messages.
static uint8_t DeriveEndReason(const NetbridgeStatus& status, int prevPhase)
{
    const auto phase = static_cast<NetbridgePhase>(status.phase);
    const auto prev  = static_cast<NetbridgePhase>(prevPhase);

    // Only derive on actual transition into an end state.
    if (phase != NetbridgePhase::Failed && phase != NetbridgePhase::SessionEnded)
        return EFZ_END_NONE;
    // Don't re-derive if we're already in an end state from last tick.
    if (prev == NetbridgePhase::Failed || prev == NetbridgePhase::SessionEnded)
        return EFZ_END_NONE;

    // Check the error message for clues.
    const char* err = status.errorMsg;

    if (phase == NetbridgePhase::Failed)
    {
        // Check for specific failure reasons.
        if (std::strstr(err, "process ended") != nullptr)
            return EFZ_END_PEER_PROCESS_DIED;
        if (std::strstr(err, "start canceled") != nullptr)
            return EFZ_END_CANCELLED;
        return EFZ_END_CONNECT_FAILED;
    }

    // SessionEnded
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
} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Initialize()
{
    // Create the named shared memory block.
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
    }

    // Initialise both the shared mapping and the local copy.
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

    if (g_shmView != nullptr)
    {
        std::memcpy(g_shmView, &init, sizeof(EFZNetplayState));
    }
    g_localCopy = init;

    mod::Log("StateExport: initialized (shm=%s, view=%p)",
             g_shmHandle != nullptr ? "ok" : "FAIL",
             static_cast<void*>(g_shmView));
}

void Shutdown()
{
    if (g_shmView != nullptr)
    {
        // Clear the magic so consumers know the data is stale.
        g_shmView->magic = 0;
        UnmapViewOfFile(g_shmView);
        g_shmView = nullptr;
    }
    if (g_shmHandle != nullptr)
    {
        CloseHandle(g_shmHandle);
        g_shmHandle = nullptr;
    }

    g_localCopy = {};
    mod::Log("StateExport: shutdown");
}

void Update(const NetbridgeStatus& status)
{
    EFZNetplayState s = {};

    // Header
    s.magic = EFZ_NETPLAY_STATE_MAGIC;
    s.version = EFZ_NETPLAY_STATE_VERSION;
    s.structSize = sizeof(EFZNetplayState);
    s.lastUpdateTick = GetTickCount();

    // Stall detection: warn if Update() was not called for too long during
    // an active session.  Stale exports cause consumer mods (e.g.
    // EFZRichPresence) to show incorrect state indefinitely.
    {
        const uint32_t now = GetTickCount();
        const bool sessionActive =
            static_cast<NetbridgePhase>(status.phase) != NetbridgePhase::Idle &&
            static_cast<NetbridgePhase>(status.phase) != NetbridgePhase::Failed &&
            static_cast<NetbridgePhase>(status.phase) != NetbridgePhase::SessionEnded;
        if (sessionActive && g_lastExportTickMs != 0)
        {
            const uint32_t elapsed = now - g_lastExportTickMs;
            if (elapsed > kExportStallWarningMs)
            {
                mod::Log(
                    "StateExport: STALL detected — %u ms since last update "
                    "(seq=%u phase=%d)",
                    static_cast<unsigned>(elapsed),
                    g_stateSeq,
                    status.phase);
            }
        }
    }

    // Increment sequence counter every tick.
    s.stateSeq = ++g_stateSeq;

    // Session identity
    s.sessionMode = ResolveSessionMode(
        takeover::g_netplayRole, takeover::g_localRoleFlag, status.role);
    s.sessionPhase = status.phase;
    s.localSide = ResolveLocalSide(takeover::g_netplayRole, status.activePlayer);

    // Detect session start: phase transitions from Idle to Connecting.
    {
        const auto curPhase = static_cast<NetbridgePhase>(status.phase);
        const auto prevPhase = static_cast<NetbridgePhase>(g_prevBridgePhase);

        // New session?
        if (curPhase == NetbridgePhase::Connecting &&
            prevPhase == NetbridgePhase::Idle)
        {
            ++g_sessionId;
            g_latchedEndReason = EFZ_END_NONE;
            g_latchedP1Wins = 0;
            g_latchedP2Wins = 0;
            g_prevP1Wins = 0;
            g_prevP2Wins = 0;
            mod::Log("StateExport: new session sessionId=%u", g_sessionId);
        }

        // Determine end reason on transition into terminal state.
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

    // Capability bits — filled in as each group is populated.
    uint32_t caps = 0;

    // Session identity is always available.
    caps |= EFZ_CAP_SESSION;

    // Scores — Revival session is the sole authority for win counts.
    // EFZ.exe does NOT track wins at all; only the Revival DLL does,
    // via the session object at the version-specific offsets.
    //
    // A high-water-mark latch prevents the exported wins from dropping
    // back to 0 during transient session-pointer validation gaps or
    // game-system resets that occur at screen transitions.
    {
        int32_t liveP1 = status.sessionP1Wins;
        int32_t liveP2 = status.sessionP2Wins;

        // Read match counter from the EXE game system (it does track rounds).
        int32_t gsP1Unused = 0, gsP2Unused = 0, gsMatch = 0;
        ReadScores(gsP1Unused, gsP2Unused, gsMatch);
        s.matchCounter = gsMatch;

        // Update the high-water-mark latch when the live total exceeds it.
        const int32_t liveTotal = liveP1 + liveP2;
        const int32_t latchTotal = g_latchedP1Wins + g_latchedP2Wins;
        if (liveTotal > latchTotal)
        {
            g_latchedP1Wins = liveP1;
            g_latchedP2Wins = liveP2;
        }

        // Use the latch if it has a higher total than the live read.
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
    if (s.p1Wins != 0 || s.p2Wins != 0 || s.matchCounter != 0)
        caps |= EFZ_CAP_SCORES;

    // Detect set transition: scores reset to 0-0 from non-zero.
    if ((g_prevP1Wins != 0 || g_prevP2Wins != 0) &&
        s.p1Wins == 0 && s.p2Wins == 0)
    {
        ++g_setId;
        mod::Log("StateExport: new set setId=%u", g_setId);
    }
    // Log individual win increments.
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

    // Nicknames — copy from bridge status
    std::memcpy(s.localNickname, status.nickname, sizeof(s.localNickname));
    std::memcpy(s.p1Name, status.p1Name, sizeof(s.p1Name));
    std::memcpy(s.p2Name, status.p2Name, sizeof(s.p2Name));
    if (s.localNickname[0] != '\0' || s.p1Name[0] != '\0' || s.p2Name[0] != '\0')
        caps |= EFZ_CAP_NICKNAMES;

    // Log when any nickname changes.
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

    // Network
    s.pingMs = status.pingMs;
    s.rollbackFrames = status.rollbackFrames;
    if (s.pingMs >= 0 || s.rollbackFrames >= 0)
        caps |= EFZ_CAP_NETWORK;

    // Log when ping or delay changes by more than 1 ms / 1 frame.
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

    // Read the EFZ screen index once here; used by menu reconciliation,
    // game-flow flags, activity phase, and match context below.
    uint8_t screenIdx = 0;
    __try
    {
        screenIdx = *reinterpret_cast<const uint8_t*>(kScreenIndexAddr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Netplay menu state (v2/v6)
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

    // Screen-index consistency: the netplay menu only exists on the title
    // screen (index 0).  If EFZ has moved to charselect (1), loading (2), or
    // battle (3), the menu active-flag is stale and must be suppressed so
    // consumers never see inNetplayMenu=1 while a match is in progress.
    if (s.inNetplayMenu && screenIdx != 0)
    {
        mod::Log(
            "StateExport: reconcile — inNetplayMenu=1 but screenIdx=%u, clearing",
            static_cast<unsigned>(screenIdx));
        s.inNetplayMenu = 0;
        s.netplayMenuScreen = 0;
        s.netplayMenuDetail = EFZ_MENU_DETAIL_NONE;
        caps &= ~EFZ_CAP_MENU;
    }

    // EfzRevival version tag (v2)
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

    // Game-flow flags (v3) — mutually exclusive with inNetplayMenu.
    // When g_returnToNetplayAfterMatch is true we are inside an online
    // session flow (charselect → loading → match).  The EFZ screen index
    // tells us exactly which phase we are in:
    //   index 1 = charselect, index 2 = loading, index 3 = battle.
    // For charselect we also count the loading screen (index 2) because
    // it's still part of the pre-match transition.
    {
        const bool inOnlineFlow =
            netplay::hooks::internal::g_returnToNetplayAfterMatch;
        // screenIdx already read above.

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

    // --- Activity phase (v4) -----------------------------------------------
    // Derive from the combination of session phase, menu state, and screen
    // index.  This is a more granular version of the v3 flow flags.
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
            // Distinguish charselect vs loading screen.
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

    // --- Character-select context (v4) -------------------------------------
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

    // Log charselect context changes (character picks, lock-ins).
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

    // --- Match context (v4) ------------------------------------------------
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
                // Match counter serves as round index (0-based).
                s.roundIndex = static_cast<uint8_t>(
                    *reinterpret_cast<const uint8_t*>(
                        gameSys + kGameSystemOffsetMatchCtr));
                s.isRoundActive = 1;
                caps |= EFZ_CAP_MATCH_CONTEXT;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    s.capabilityFlags = caps;

    // Transition logging (v3 + v4) — log when flags or activity phase change.
    {
        if (s.inNetplayMenu != g_prevInNetplayMenu
            || s.inNetplayCharacterSelect != g_prevInNetplayCharacterSelect
            || s.inNetplayMatch != g_prevInNetplayMatch)
        {
            mod::Log(
                "StateExport: flow menu=%u->%u charsel=%u->%u match=%u->%u "
                "screen=%u mode=%d phase=%d role=%d p1Wins=%d p2Wins=%d",
                static_cast<unsigned>(g_prevInNetplayMenu),
                static_cast<unsigned>(s.inNetplayMenu),
                static_cast<unsigned>(g_prevInNetplayCharacterSelect),
                static_cast<unsigned>(s.inNetplayCharacterSelect),
                static_cast<unsigned>(g_prevInNetplayMatch),
                static_cast<unsigned>(s.inNetplayMatch),
                static_cast<unsigned>(screenIdx),
                s.sessionMode,
                s.sessionPhase,
                takeover::g_netplayRole,
                s.p1Wins,
                s.p2Wins);
            g_prevInNetplayMenu = s.inNetplayMenu;
            g_prevInNetplayCharacterSelect = s.inNetplayCharacterSelect;
            g_prevInNetplayMatch = s.inNetplayMatch;
        }

        if (s.activityPhase != g_prevActivityPhase)
        {
            mod::Log(
                "StateExport: activity %u->%u screen=%u mode=%d phase=%d role=%d side=%d "
                "p1='%s' p2='%s' caps=0x%X sessionId=%u setId=%u seq=%u",
                static_cast<unsigned>(g_prevActivityPhase),
                static_cast<unsigned>(s.activityPhase),
                static_cast<unsigned>(screenIdx),
                s.sessionMode,
                s.sessionPhase,
                takeover::g_netplayRole,
                s.localSide,
                s.p1Name,
                s.p2Name,
                s.capabilityFlags,
                s.sessionId,
                s.setId,
                s.stateSeq);
            // Log context fields for the new phase.
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

    // Periodic heartbeat — every 600 ticks (~10s at 60fps) and on the very
    // first tick — dump all key fields so we can verify the export without
    // needing a phase transition to trigger the transition logs.
    if (g_stateSeq == 1 || (g_stateSeq % 600) == 0)
    {
        mod::Log(
            "StateExport: heartbeat seq=%u activity=%u menu=%u screen=%u "
            "mode=%d phase=%d role=%d side=%d "
            "p1Wins=%d p2Wins=%d ping=%d delay=%d "
            "local='%s' p1='%s' p2='%s' revival='%s' "
            "caps=0x%X sessionId=%u setId=%u endReason=%u",
            s.stateSeq,
            static_cast<unsigned>(s.activityPhase),
            static_cast<unsigned>(s.inNetplayMenu),
            static_cast<unsigned>(screenIdx),
            s.sessionMode,
            s.sessionPhase,
            takeover::g_netplayRole,
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

    // Commit to shared memory and local copy
    if (g_shmView != nullptr)
    {
        std::memcpy(g_shmView, &s, sizeof(EFZNetplayState));
    }
    g_localCopy = s;
    g_lastExportTickMs = s.lastUpdateTick;
}

const EFZNetplayState* GetExportedState()
{
    return &g_localCopy;
}

} // namespace netplay::bridge::state_export
