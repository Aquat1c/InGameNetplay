#include "netplay/bridge/session_bridge.h"

#include "netplay/bridge/netplay_state_export.h"
#include "netplay/bridge/revival_takeover.h"
#include "netplay/bridge/takeover_internal.h"
#include "logger.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <windows.h>

namespace netplay::bridge
{
namespace
{
std::mutex g_mutex;
NetbridgeStatus g_status = {};
uint32_t g_connectStartTick = 0;
bool g_initialized = false;

std::thread g_startWorker;
bool g_startWorkerRunning = false;
uint32_t g_startRequestSerial = 0;

bool ShouldCancelToIdle(const char* reason)
{
    return reason != nullptr
        && (std::strcmp(reason, "user_cancel") == 0
            || std::strcmp(reason, "leave_menu") == 0
            || std::strcmp(reason, "external_cancel") == 0
            || std::strcmp(reason, "dismissed_error") == 0
            || std::strcmp(reason, "no_overlay_session_ended") == 0);
}

void SetPhase(NetbridgePhase phase, const char* error)
{
    g_status.phase = static_cast<int>(phase);
    g_status.phaseTick = GetTickCount();
    if (error != nullptr)
    {
#if defined(_MSC_VER)
        strncpy_s(g_status.errorMsg, sizeof(g_status.errorMsg), error, _TRUNCATE);
#else
        std::snprintf(g_status.errorMsg, sizeof(g_status.errorMsg), "%s", error);
#endif
    }
    else if (phase != NetbridgePhase::Failed)
    {
        g_status.errorMsg[0] = '\0';
    }
}

void JoinFinishedWorkerUnlocked()
{
    if (g_startWorker.joinable() && !g_startWorkerRunning)
    {
        g_startWorker.join();
    }
}

void InitializeHostUnlocked(const char* reason)
{
    g_status = {};
    SetPhase(NetbridgePhase::Idle, nullptr);
    takeover::InitializeHost();
    takeover::Tick(&g_status, &g_connectStartTick);
    g_initialized = true;
    mod::Log("SessionBridge: initialized (role=host, reason=%s)", reason != nullptr ? reason : "unknown");
}
} // namespace

bool IsCurrentProcessRevival()
{
    return takeover::IsCurrentProcessRevival();
}

bool IsRunningUnderWine()
{
    return takeover::IsRunningUnderWine();
}

int SelfPatchIat()
{
    return takeover::SelfPatchIat();
}

void Initialize()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized)
    {
        return;
    }

    InitializeHostUnlocked("startup");
    state_export::Initialize();
}

void Shutdown()
{
    std::thread workerToJoin;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_initialized)
        {
            return;
        }

        ++g_startRequestSerial;
        if (g_startWorkerRunning)
        {
            takeover::RequestAbortStart();
            if (g_startWorker.joinable())
            {
                workerToJoin = std::move(g_startWorker);
            }
            g_startWorkerRunning = false;
        }
        else
        {
            JoinFinishedWorkerUnlocked();
        }
    }

    if (workerToJoin.joinable())
    {
        workerToJoin.join();
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        takeover::CancelSession("shutdown", &g_status);
        takeover::ShutdownHost();
        state_export::Shutdown();
        g_status = {};
        SetPhase(NetbridgePhase::Idle, nullptr);
        g_connectStartTick = 0;
        g_initialized = false;
    }

    mod::Log("SessionBridge: shutdown (role=host)");
}

void EmergencyShutdown()
{
    // Best-effort teardown for DLL detach during process termination.
    // Avoid blocking joins under loader-lock constraints.
    if (!g_mutex.try_lock())
    {
        takeover::RequestAbortStart();
        takeover::EmergencyShutdownHost();
        return;
    }

    if (g_startWorker.joinable())
    {
        g_startWorker.detach();
    }
    g_startWorkerRunning = false;
    ++g_startRequestSerial;
    takeover::RequestAbortStart();
    takeover::EmergencyShutdownHost();
    g_status = {};
    SetPhase(NetbridgePhase::Idle, nullptr);
    g_connectStartTick = 0;
    g_initialized = false;
    g_mutex.unlock();
}

void InitializeInjectedProcess()
{
    takeover::InitializeInjected();
}

void ShutdownInjectedProcess()
{
    takeover::ShutdownInjected();
}

void Tick()
{
    LARGE_INTEGER tickQpcPre = {};
    QueryPerformanceCounter(&tickQpcPre);

    NetbridgeStatus statusSnapshot = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_initialized)
        {
            return;
        }

        // Keep the main thread non-blocking while start worker owns takeover startup.
        if (g_startWorkerRunning)
        {
            return;
        }

        JoinFinishedWorkerUnlocked();
        takeover::Tick(&g_status, &g_connectStartTick);
        statusSnapshot = g_status;
    }

    state_export::Update(statusSnapshot);

    // --- Timing guard on full Tick (includes takeover::Tick + export enqueue) --
    {
        static uint32_t s_tickSlowCount = 0;
        LARGE_INTEGER tickQpcPost = {}, freq = {};
        QueryPerformanceCounter(&tickQpcPost);
        QueryPerformanceFrequency(&freq);
        const double elapsedMs =
            static_cast<double>(tickQpcPost.QuadPart - tickQpcPre.QuadPart)
            * 1000.0 / static_cast<double>(freq.QuadPart);
        if (elapsedMs > 5.0)
        {
            ++s_tickSlowCount;
            if (s_tickSlowCount <= 10 || (s_tickSlowCount % 200 == 0))
            {
                mod::Log(
                    "PERF_WARN: session_bridge::Tick took %.2fms "
                    "(slowCount=%u) — full tick is slow",
                    elapsedMs, s_tickSlowCount);
            }
        }
    }
}

void TickExportOnly()
{
    // Lightweight per-frame export pulse.  Called every game frame from
    // OurPerFrameTickHook (revival_memory.cpp) so that activityPhase,
    // inNetplayMenu, stateSeq, scores, ping, delay, and all other exported
    // fields remain current during loading screen and battle — screens that
    // have no title/charselect hook calling the full Tick().
    //
    // Also called immediately after g_netplayMenuState.active is cleared in
    // HandoffConnectedSessionToVsHumanState / HandoffSpectateSession so that
    // the handoff is reflected in the export before the next frame hook fires.
    //
    // We call RefreshRuntimeStatus() here to re-read volatile session fields
    // (wins, ping, delay, activePlayer, etc.) from Revival memory.  Without
    // this, those fields stay stale at whatever value they had when the last
    // full Tick() ran — typically during connection, before any match was
    // played — so wins would read 0-0 even after a match ends.

    NetbridgeStatus statusSnapshot = {};
    {
        std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
        if (!lock.owns_lock())
        {
            static uint32_t s_teoSkippedCount = 0;
            ++s_teoSkippedCount;
            if (s_teoSkippedCount <= 10 || (s_teoSkippedCount % 300) == 0)
            {
                mod::Log(
                    "PERF_WARN: TickExportOnly skipped due to bridge lock contention "
                    "(skipCount=%u)",
                    s_teoSkippedCount);
            }
            return;
        }

        if (!g_initialized)
        {
            return;
        }

        takeover::RefreshRuntimeStatus(&g_status);
        statusSnapshot = g_status;
    }

    state_export::Update(statusSnapshot);
}

bool StartSession(
    NetbridgeRole role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        InitializeHostUnlocked("on_demand");
    }

    JoinFinishedWorkerUnlocked();
    if (g_startWorkerRunning)
    {
        SetPhase(NetbridgePhase::Failed, "session start already in progress");
        mod::Log("SessionBridge: StartSession rejected (worker already running)");
        return false;
    }

    const std::string addressCopy = (address != nullptr) ? address : "";
    const std::string nicknameCopy = (nickname != nullptr) ? nickname : "";

    g_status.role = static_cast<int>(role);
    g_status.port = port;
    mod::Log(
        "SessionBridge: StartSession role=%d port=%u address='%s' nickname='%s' writeNicknameToIni=%d",
        static_cast<int>(role),
        static_cast<unsigned>(port),
        addressCopy.c_str(),
        nicknameCopy.c_str(),
        writeNicknameToIni ? 1 : 0);
    takeover::ClearDelayPromptState("session_bridge_start_session");
#if defined(_MSC_VER)
    strncpy_s(g_status.address, sizeof(g_status.address), addressCopy.c_str(), _TRUNCATE);
    strncpy_s(g_status.nickname, sizeof(g_status.nickname), nicknameCopy.c_str(), _TRUNCATE);
#else
    std::snprintf(g_status.address, sizeof(g_status.address), "%s", addressCopy.c_str());
    std::snprintf(g_status.nickname, sizeof(g_status.nickname), "%s", nicknameCopy.c_str());
#endif

    SetPhase(NetbridgePhase::Connecting, nullptr);
    g_connectStartTick = GetTickCount();

    const uint32_t requestSerial = ++g_startRequestSerial;
    g_startWorkerRunning = true;

    g_startWorker = std::thread([requestSerial, role, port, addressCopy, nicknameCopy, writeNicknameToIni]() {
        NetbridgeStatus workerStatus = {};
        workerStatus.role = static_cast<int>(role);
        workerStatus.port = port;
#if defined(_MSC_VER)
        strncpy_s(workerStatus.address, sizeof(workerStatus.address), addressCopy.c_str(), _TRUNCATE);
        strncpy_s(workerStatus.nickname, sizeof(workerStatus.nickname), nicknameCopy.c_str(), _TRUNCATE);
#else
        std::snprintf(workerStatus.address, sizeof(workerStatus.address), "%s", addressCopy.c_str());
        std::snprintf(workerStatus.nickname, sizeof(workerStatus.nickname), "%s", nicknameCopy.c_str());
#endif

        uint32_t workerConnectStartTick = GetTickCount();
        const bool started = takeover::StartSession(
            role,
            port,
            addressCopy.c_str(),
            nicknameCopy.c_str(),
            writeNicknameToIni,
            &workerStatus,
            &workerConnectStartTick);

        bool cancelStaleSession = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (requestSerial == g_startRequestSerial)
            {
                g_status = workerStatus;
                g_connectStartTick = workerConnectStartTick;
                if (!started)
                {
                    mod::Log(
                        "SessionBridge: StartSession failed phase=%s error='%s'",
                        PhaseToString(static_cast<NetbridgePhase>(g_status.phase)),
                        g_status.errorMsg);
                }
            }
            else
            {
                mod::Log(
                    "SessionBridge: dropped stale start result serial=%u current=%u",
                    static_cast<unsigned>(requestSerial),
                    static_cast<unsigned>(g_startRequestSerial));
                cancelStaleSession = started;
            }

            g_startWorkerRunning = false;
        }

        if (cancelStaleSession)
        {
            takeover::CancelSession("stale_worker_result", nullptr);
        }
    });

    return true;
}

bool ApplyInputDelay(int delayFrames)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return false;
    }

    JoinFinishedWorkerUnlocked();
    if (g_startWorkerRunning)
    {
        mod::Log("SessionBridge: ApplyInputDelay rejected (start worker running)");
        return false;
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(g_status.phase);
    if (phase != NetbridgePhase::Connecting
        && phase != NetbridgePhase::DelaySetup
        && phase != NetbridgePhase::Connected)
    {
        mod::Log(
            "SessionBridge: ApplyInputDelay ignored (phase=%s value=%d)",
            PhaseToString(phase),
            delayFrames);
        return false;
    }

    const bool applied = takeover::ApplyInputDelay(delayFrames, &g_status);
    mod::Log(
        "SessionBridge: ApplyInputDelay value=%d result=%d phase=%s",
        delayFrames,
        applied ? 1 : 0,
        PhaseToString(static_cast<NetbridgePhase>(g_status.phase)));
    return applied;
}

bool AnswerSpectatePromptChoice(int choice)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return false;
    }

    JoinFinishedWorkerUnlocked();
    if (g_startWorkerRunning)
    {
        mod::Log("SessionBridge: AnswerSpectatePromptChoice rejected (start worker running)");
        return false;
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(g_status.phase);
    if (phase != NetbridgePhase::Connecting
        && phase != NetbridgePhase::DelaySetup
        && phase != NetbridgePhase::Connected)
    {
        mod::Log(
            "SessionBridge: AnswerSpectatePromptChoice ignored (phase=%s choice=%d)",
            PhaseToString(phase),
            choice);
        return false;
    }

    const bool answered = takeover::AnswerSpectatePromptChoice(choice, &g_status);
    mod::Log(
        "SessionBridge: AnswerSpectatePromptChoice choice=%d result=%d phase=%s",
        choice,
        answered ? 1 : 0,
        PhaseToString(static_cast<NetbridgePhase>(g_status.phase)));
    return answered;
}

bool PrepareVsHumanHandoff()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return false;
    }

    JoinFinishedWorkerUnlocked();
    if (g_startWorkerRunning)
    {
        mod::Log("SessionBridge: PrepareVsHumanHandoff rejected (start worker running)");
        return false;
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(g_status.phase);
    const bool allowDuringConnectingDelayStage =
        (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
        && g_status.delaySetupReady != 0;
    if (phase != NetbridgePhase::Connected && !allowDuringConnectingDelayStage)
    {
        mod::Log(
            "SessionBridge: PrepareVsHumanHandoff ignored (phase=%s)",
            PhaseToString(phase));
        return false;
    }

    const bool prepared = takeover::PrepareVsHumanHandoff(&g_status);
    mod::Log(
        "SessionBridge: PrepareVsHumanHandoff result=%d sync(mode=%d flag1084=%d session=%d flags=%d/%d)",
        prepared ? 1 : 0,
        g_status.syncGameMode,
        g_status.syncMode0Flag1084,
        g_status.syncSessionByte,
        g_status.syncGlobalFlag4964,
        g_status.syncGlobalFlag4965);
    return prepared;
}

void CancelSession(const char* reason)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return;
    }

    ++g_startRequestSerial;
    mod::Log("SessionBridge: CancelSession reason='%s'", (reason != nullptr) ? reason : "");

    if (g_startWorkerRunning)
    {
        takeover::RequestAbortStart();
        if (ShouldCancelToIdle(reason))
        {
            SetPhase(NetbridgePhase::Idle, nullptr);
            mod::Log(
                "SessionBridge: cancel acknowledged -> Idle reason='%s'",
                reason != nullptr ? reason : "");
        }
        else
        {
            SetPhase(NetbridgePhase::Failed, "start canceled");
        }
        return;
    }

    JoinFinishedWorkerUnlocked();
    takeover::CancelSession(reason, &g_status);
}

bool ConsumeRevivalExitInterception(int* outMode)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return false;
    }

    // Invalidate any in-flight start worker result so it won't overwrite the
    // cleaned-up status when it eventually completes.
    ++g_startRequestSerial;
    g_connectStartTick = 0;

    JoinFinishedWorkerUnlocked();
    return takeover::ConsumeRevivalExitInterception(outMode, &g_status);
}

void CompleteGameplayExitRecovery(int mode, const char* origin)
{
    NetbridgeStatus statusSnapshot = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_initialized)
        {
            return;
        }

        ++g_startRequestSerial;
        g_connectStartTick = 0;
        JoinFinishedWorkerUnlocked();

        takeover::RefreshRuntimeStatus(&g_status);
        if (mode == takeover::kLocalRoleOnline
            || mode == takeover::kLocalRoleSpectate)
        {
            SetPhase(NetbridgePhase::SessionEnded, nullptr);
        }
        else
        {
            SetPhase(NetbridgePhase::Idle, nullptr);
        }

        statusSnapshot = g_status;
    }

    state_export::Update(statusSnapshot);
    mod::Log(
        "SessionBridge: gameplay exit recovery completed origin=%s mode=%d phase=%d",
        origin != nullptr ? origin : "",
        mode,
        statusSnapshot.phase);
}

bool NotifyTitleScreenActive()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return false;
    }

    JoinFinishedWorkerUnlocked();
    return takeover::NotifyTitleScreenActive(&g_status);
}

void OnTitleSelectionConfirmed(int selection)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        InitializeHostUnlocked("title_confirm");
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(g_status.phase);
    if (phase == NetbridgePhase::Connecting
        || phase == NetbridgePhase::DelaySetup
        || phase == NetbridgePhase::Connected)
    {
        return;
    }

    takeover::OnTitleSelectionConfirmed(selection, &g_status);
}

NetbridgeStatus GetStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

DelayPromptMetrics GetDelayPromptMetrics()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return takeover::GetDelayPromptMetrics();
}

const char* PhaseToString(NetbridgePhase phase)
{
    switch (phase)
    {
    case NetbridgePhase::Idle:
        return "Idle";
    case NetbridgePhase::Connecting:
        return "Connecting";
    case NetbridgePhase::DelaySetup:
        return "DelaySetup";
    case NetbridgePhase::Connected:
        return "Connected";
    case NetbridgePhase::Failed:
        return "Failed";
    case NetbridgePhase::SessionEnded:
        return "SessionEnded";
    default:
        return "Unknown";
    }
}

void BuildStatusLine(const NetbridgeStatus& status, char* buffer, size_t bufferSize)
{
    if (buffer == nullptr || bufferSize == 0)
    {
        return;
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(status.phase);
    switch (phase)
    {
    case NetbridgePhase::Idle:
        std::snprintf(
            buffer,
            bufferSize,
            "Nick:%s Host:%u Join:%s:%u Role:%d",
            status.nickname[0] != '\0' ? status.nickname : "Player",
            static_cast<unsigned>(status.port),
            status.address,
            static_cast<unsigned>(status.port),
            status.roleFlag);
        break;
    case NetbridgePhase::Connecting:
        std::snprintf(
            buffer,
            bufferSize,
            "Connecting... pid=%lu Sync:%d/%d/%d DelayReady:%d Prompt:%d Init:%d ESC/BACK=Cancel",
            static_cast<unsigned long>(status.processId),
            status.syncGameMode,
            status.syncMode0Flag1084,
            status.syncSessionByte,
            status.delaySetupReady,
            status.delayPromptSerial > 0 ? 1 : 0,
            status.localInitApplied);
        break;
    case NetbridgePhase::DelaySetup:
        std::snprintf(
            buffer,
            bufferSize,
            "Delay setup pid=%lu Ping:%dms Current:%df Prompt:%d Init:%d CONFIRM=Apply ESC/BACK=Cancel",
            static_cast<unsigned long>(status.processId),
            status.pingMs,
            status.rollbackFrames,
            status.delayPromptSerial > 0 ? 1 : 0,
            status.localInitApplied);
        break;
    case NetbridgePhase::Connected:
    {
        char pingText[32] = "?";
        char delayText[32] = "?";
        char namesText[140] = {};
        if (status.pingMs >= 0)
        {
            std::snprintf(pingText, sizeof(pingText), "%dms", status.pingMs);
        }
        if (status.rollbackFrames >= 0)
        {
            std::snprintf(delayText, sizeof(delayText), "%df", status.rollbackFrames);
        }
        if (status.p1Name[0] != '\0' && status.p2Name[0] != '\0')
        {
            std::snprintf(namesText, sizeof(namesText), " %s vs %s", status.p1Name, status.p2Name);
        }
        std::snprintf(
            buffer,
            bufferSize,
            "Connected pid=%lu Ping:%s Delay:%s Role:%d Sync:%d/%d/%d%s",
            static_cast<unsigned long>(status.processId),
            pingText,
            delayText,
            status.roleFlag,
            status.syncGameMode,
            status.syncMode0Flag1084,
            status.syncSessionByte,
            namesText);
        break;
    }
    case NetbridgePhase::Failed:
        std::snprintf(buffer, bufferSize, "Connect failed: %s", status.errorMsg);
        break;
    case NetbridgePhase::SessionEnded:
        std::snprintf(buffer, bufferSize, "Disconnected");
        break;
    default:
        std::snprintf(buffer, bufferSize, "Status unknown");
        break;
    }
}
bool IsPeerProcessAlive()
{
    return takeover::IsPeerProcessAlive();
}
bool IsNetplayExitInterceptionPending()
{
    return takeover::IsNetplayExitInterceptionPending();
}
bool ForceLocalPlayInit()
{
    return takeover::ForceLocalPlayInit();
}
bool ForceGameModeToTitle()
{
    return takeover::ForceGameModeToTitle();
}
uintptr_t GetRevivalRenderContextOffset()
{
    const auto* profile = takeover::g_activeRevival;
    return profile != nullptr ? profile->renderContextGlobalOffset : 0;
}
uintptr_t GetRevivalSessionPtrOffset()
{
    const auto* profile = takeover::g_activeRevival;
    return (profile != nullptr && profile->sessionPtrOffsetCount > 0)
        ? profile->sessionPtrOffsets[0] : 0;
}
} // namespace netplay::bridge
