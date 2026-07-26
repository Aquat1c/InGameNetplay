#include "netplay/bridge/session_bridge.h"

#include "netplay/bridge/async_hosting.h"
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
HostSessionNetworkConfig g_activeHostNetworkConfig = {};
bool g_activeHostNetworkConfigValid = false;
uint32_t g_lastRejectedHostListenerSerial = 0;
bool g_synchronousStartRejected = false;
HANDLE g_peerProbeHandle = nullptr;
DWORD g_peerProbeProcessId = 0;

std::thread g_startWorker;
bool g_startWorkerRunning = false;
uint32_t g_startRequestSerial = 0;

bool IsKnownFamily(network::NetworkFamily family)
{
    return family == network::NetworkFamily::IPv4
        || family == network::NetworkFamily::IPv6;
}

void ClosePeerProbeHandleUnlocked()
{
    if (g_peerProbeHandle != nullptr)
    {
        CloseHandle(g_peerProbeHandle);
        g_peerProbeHandle = nullptr;
    }
    g_peerProbeProcessId = 0;
}

void MarkSynchronousStartRejectedUnlocked(
    NetbridgeRole role,
    const char* reason)
{
    g_synchronousStartRejected = true;
    mod::Log(
        "REVIVAL_NETPLAY_SYNC_START_REJECTED latched=1 role=%d "
        "reason=%s phase=%s",
        static_cast<int>(role),
        reason != nullptr ? reason : "",
        PhaseToString(
            static_cast<NetbridgePhase>(g_status.phase)));
}

bool ShouldCancelToIdle(const char* reason)
{
    return reason != nullptr
        && (std::strcmp(reason, "user_cancel") == 0
            || std::strcmp(reason, "leave_menu") == 0
            || std::strcmp(reason, "external_cancel") == 0
            || std::strcmp(reason, "dismissed_error") == 0
            || std::strcmp(reason, "dismissed_host_error") == 0
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
    takeover::LogRevival102jDeepStep("Phase.session_bridge_transition", &g_status);
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
    ClosePeerProbeHandleUnlocked();
    g_status = {};
    g_activeHostNetworkConfig = {};
    g_activeHostNetworkConfigValid = false;
    g_synchronousStartRejected = false;
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
        g_activeHostNetworkConfig = {};
        g_activeHostNetworkConfigValid = false;
        g_synchronousStartRejected = false;
        ClosePeerProbeHandleUnlocked();
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
    g_activeHostNetworkConfig = {};
    g_activeHostNetworkConfigValid = false;
    g_synchronousStartRejected = false;
    ClosePeerProbeHandleUnlocked();
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

    // Advance the async-hosting state machine (prompt-hold / peer-found / accept
    // release). Cheap no-op while inactive. Runs here so it sees the freshly
    // updated status snapshot in the title/netplay-menu context.
    async_host::Tick();

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
                    "(slowCount=%u) - full tick is slow",
                    elapsedMs, s_tickSlowCount);
            }
        }
    }
}

void TickExportOnly(bool force)
{
    // Lightweight export pulse. Called every game frame from
    // OurPerFrameTickHook (revival_memory.cpp), but rate-limited here so
    // shared-state consumers remain current without performing session-memory
    // reads and queue publication on every rollback tick.
    //
    // 125 ms is fast enough for UI/status consumers while moving this work
    // from ~60 Hz to at most 8 Hz. Transition sites pass force=true.
    static volatile LONG s_lastPublishTick = 0;
    const DWORD nowTick = GetTickCount();
    if (!force)
    {
        const LONG previous = InterlockedCompareExchange(
            &s_lastPublishTick, 0, 0);
        if (previous != 0
            && static_cast<DWORD>(nowTick - static_cast<DWORD>(previous)) < 125u)
        {
            return;
        }
        if (InterlockedCompareExchange(
                &s_lastPublishTick,
                static_cast<LONG>(nowTick),
                previous) != previous)
        {
            return;
        }
    }
    else
    {
        InterlockedExchange(&s_lastPublishTick, static_cast<LONG>(nowTick));
    }

    // Keep activityPhase,
    // inNetplayMenu, stateSeq, scores, ping, delay, and all other exported
    // fields remain current during loading screen and battle - screens that
    // have no title/charselect hook calling the full Tick().
    //
    // Also called immediately after g_netplayMenuState.active is cleared in
    // HandoffConnectedSessionToVsHumanState / HandoffSpectateSession so that
    // the handoff is reflected in the export before the next frame hook fires.
    //
    // We call RefreshRuntimeStatus() here to re-read volatile session fields
    // (wins, ping, delay, activePlayer, etc.) from Revival memory.  Without
    // this, those fields stay stale at whatever value they had when the last
    // full Tick() ran - typically during connection, before any match was
    // played - so wins would read 0-0 even after a match ends.

    NetbridgeStatus statusSnapshot = {};
    bool shouldHandleHostProtocolAck = false;
    {
        std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
        if (!lock.owns_lock())
        {
            static uint32_t s_teoSkippedCount = 0;
            ++s_teoSkippedCount;
            if (s_teoSkippedCount <= 10 || (s_teoSkippedCount % 300) == 0)
            {
                MOD_LIFECYCLE_TRACE(
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
        shouldHandleHostProtocolAck = !g_startWorkerRunning;
    }

    // Gameplay/loading screens call this lightweight path instead of the full
    // takeover::Tick(). Pump the temporary Protocol restore here as well, but
    // only after the worker has committed the matching PID/status snapshot and
    // after releasing the bridge mutex (the Protocol mutex is a leaf lock).
    if (shouldHandleHostProtocolAck)
    {
        takeover::HandleTemporaryHostProtocolListenerAck(
            static_cast<DWORD>(statusSnapshot.processId),
            statusSnapshot.port);
    }

    state_export::Update(statusSnapshot);
}

static bool StartSessionInternal(
    NetbridgeRole role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni,
    const HostSessionNetworkConfig* explicitHostNetworkConfig,
    HostStartFailure* outHostFailure)
{
    if (outHostFailure != nullptr)
    {
        *outHostFailure = HostStartFailure::None;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        InitializeHostUnlocked("on_demand");
    }

    JoinFinishedWorkerUnlocked();
    if (g_startWorkerRunning)
    {
        if (outHostFailure != nullptr)
        {
            *outHostFailure =
                HostStartFailure::StartAlreadyInProgress;
        }
        SetPhase(NetbridgePhase::Failed, "session start already in progress");
        mod::Log("SessionBridge: StartSession rejected (worker already running)");
        return false;
    }

    if (explicitHostNetworkConfig != nullptr)
    {
        // A synchronous rejection must not leave the prior attempt's family
        // snapshot available to listener validation.
        g_activeHostNetworkConfig = {};
        g_activeHostNetworkConfigValid = false;
        takeover::ClearHostListenerObservation();
    }

    std::string addressCopy = (address != nullptr) ? address : "";
    std::string iniAddressCopy = addressCopy;
    const std::string nicknameCopy = (nickname != nullptr) ? nickname : "";
    network::NetworkFamily sessionFamily = network::NetworkFamily::IPv4;
    bool writeHostProtocol = false;
    HostSessionNetworkConfig normalizedHostConfig = {};

    const bool remoteRole =
        role == NetbridgeRole::Join
        || role == NetbridgeRole::Spectate
        || role == NetbridgeRole::JoinSpectate;
    if (remoteRole)
    {
        network::RemoteHostInput remoteInput = {};
        if (port == 0
            || !network::ParseRemoteHostInput(
                addressCopy,
                &remoteInput))
        {
            g_status.role = static_cast<int>(role);
            g_status.port = port;
            SetPhase(
                NetbridgePhase::Failed,
                "Invalid Revival netplay session address or port.");
            MarkSynchronousStartRejectedUnlocked(
                role,
                "invalid_remote_endpoint");
            mod::Log(
                "REVIVAL_NETPLAY_SESSION_START_REJECTED reason=invalid_remote_endpoint "
                "role=%d port=%u address='%s'",
                static_cast<int>(role),
                static_cast<unsigned>(port),
                addressCopy.c_str());
            return false;
        }

        iniAddressCopy = remoteInput.host;
        network::NetworkEndpoint remoteEndpoint = {};
        network::RemoteEndpointResolution resolution = {};
        if (!network::ResolveRemoteEndpoint(
                iniAddressCopy,
                port,
                &remoteEndpoint,
                &resolution))
        {
            g_status.role = static_cast<int>(role);
            g_status.port = port;

            const char* errorText =
                resolution.failure
                        == network::RemoteEndpointResolveFailure::NameLookup
                    ? "Could not find that host. Check the address and try again."
                    : resolution.failure
                            == network::RemoteEndpointResolveFailure::NoUsableFamily
                        ? "This PC cannot use any address available for that host."
                        : "Network connections are not available on this PC.";
            SetPhase(NetbridgePhase::Failed, errorText);
            MarkSynchronousStartRejectedUnlocked(
                role,
                resolution.failure
                        == network::RemoteEndpointResolveFailure::NameLookup
                    ? "remote_name_lookup_failed"
                    : "remote_resolution_unavailable");
            mod::Log(
                "REVIVAL_NETPLAY_SESSION_START_REJECTED "
                "reason=remote_resolution_failed role=%d input='%s' "
                "failure=%u wsaError=%d candidates(v4=%d v6=%d) "
                "unavailable(v4=%d v6=%d)",
                static_cast<int>(role),
                iniAddressCopy.c_str(),
                static_cast<unsigned>(resolution.failure),
                resolution.nativeError,
                resolution.ipv4CandidateSeen ? 1 : 0,
                resolution.ipv6CandidateSeen ? 1 : 0,
                resolution.ipv4Unavailable ? 1 : 0,
                resolution.ipv6Unavailable ? 1 : 0);
            return false;
        }

        const network::NetworkFamilyProbeResult probe =
            resolution.selectedProbe;
        if (probe.unavailable)
        {
            char errorText[128] = {};
            std::snprintf(
                errorText,
                sizeof(errorText),
                "%s connections are not available on this PC or network.",
                network::FamilyName(remoteEndpoint.family));
            g_status.role = static_cast<int>(role);
            g_status.port = port;
            SetPhase(NetbridgePhase::Failed, errorText);
            MarkSynchronousStartRejectedUnlocked(
                role,
                "remote_family_unavailable");
            mod::Log(
                "REVIVAL_NETPLAY_FAMILY_UNAVAILABLE role=%d family=%s "
                "stage=%s wsaError=%d address='%s' port=%u",
                static_cast<int>(role),
                network::FamilyName(remoteEndpoint.family),
                network::ProbeStageName(probe.stage),
                probe.nativeError,
                remoteEndpoint.host.c_str(),
                static_cast<unsigned>(port));
            return false;
        }
        if (probe.nativeError != 0)
        {
            mod::Log(
                "REVIVAL_NETPLAY_FAMILY_PREFLIGHT_AMBIGUOUS role=%d family=%s "
                "stage=%s wsaError=%d action=allow_native_revival",
                static_cast<int>(role),
                network::FamilyName(remoteEndpoint.family),
                network::ProbeStageName(probe.stage),
                probe.nativeError);
        }

        addressCopy = remoteEndpoint.host;
        sessionFamily = remoteEndpoint.family;
        if (resolution.inputWasHostname)
        {
            mod::Log(
                "REVIVAL_NETPLAY_REMOTE_HOST_RESOLVED input='%s' "
                "selectedFamily=%s address='%s' port=%u "
                "candidates(v4=%d v6=%d) unavailable(v4=%d v6=%d)",
                iniAddressCopy.c_str(),
                network::FamilyName(sessionFamily),
                addressCopy.c_str(),
                static_cast<unsigned>(port),
                resolution.ipv4CandidateSeen ? 1 : 0,
                resolution.ipv6CandidateSeen ? 1 : 0,
                resolution.ipv4Unavailable ? 1 : 0,
                resolution.ipv6Unavailable ? 1 : 0);
        }
    }
    else if (role == NetbridgeRole::Host
             && explicitHostNetworkConfig != nullptr)
    {
        normalizedHostConfig = *explicitHostNetworkConfig;
        if (!IsKnownFamily(normalizedHostConfig.preferredFamily)
            || !IsKnownFamily(normalizedHostConfig.effectiveFamily))
        {
            if (outHostFailure != nullptr)
            {
                *outHostFailure = HostStartFailure::InvalidRequest;
            }
            g_status.role = static_cast<int>(role);
            g_status.port = port;
            SetPhase(
                NetbridgePhase::Failed,
                "The hosting settings are invalid.");
            MarkSynchronousStartRejectedUnlocked(
                role,
                "invalid_host_family");
            mod::Log(
                "REVIVAL_NETPLAY_SESSION_START_REJECTED "
                "reason=invalid_host_family preferred=%u effective=%u",
                static_cast<unsigned>(normalizedHostConfig.preferredFamily),
                static_cast<unsigned>(normalizedHostConfig.effectiveFamily));
            return false;
        }

        if (!normalizedHostConfig.publicAddress.empty())
        {
            network::NetworkHost publicHost = {};
            if (!network::ParseBareHost(
                    normalizedHostConfig.publicAddress,
                    &publicHost)
                || publicHost.family != normalizedHostConfig.effectiveFamily
                || !network::IsGloballyRoutableHost(publicHost))
            {
                if (outHostFailure != nullptr)
                {
                    *outHostFailure =
                        HostStartFailure::InvalidRequest;
                }
                g_status.role = static_cast<int>(role);
                g_status.port = port;
                SetPhase(
                    NetbridgePhase::Failed,
                    "The detected address is not a global public address.");
                MarkSynchronousStartRejectedUnlocked(
                    role,
                    "public_address_not_global_or_family_mismatch");
                mod::Log(
                    "REVIVAL_NETPLAY_SESSION_START_REJECTED "
                    "reason=public_address_not_global_or_family_mismatch "
                    "effective=%s address='%s'",
                    network::FamilyName(normalizedHostConfig.effectiveFamily),
                    normalizedHostConfig.publicAddress.c_str());
                return false;
            }
            normalizedHostConfig.publicAddress = publicHost.host;
        }

        // This function runs on the game's/menu thread.  Local IPv4/IPv6
        // capability detection is intentionally performed by the background
        // capability worker before Host is selected; never open or bind a
        // probe socket here.  A missing/stale snapshot is advisory only, so
        // the real Revival listener acknowledgement remains the authority.
        mod::Log(
            "REVIVAL_NETPLAY_HOST_SELECTION_ACCEPTED family=%s "
            "localProbe=background_or_native hostPort=%u",
            network::FamilyName(normalizedHostConfig.effectiveFamily),
            static_cast<unsigned>(port));

        sessionFamily = normalizedHostConfig.effectiveFamily;
        writeHostProtocol = true;
    }

    g_status.role = static_cast<int>(role);
    g_status.port = port;
    mod::Log(
        "SessionBridge: StartSession role=%d port=%u address='%s' nickname='%s' "
        "writeNicknameToIni=%d family=%s explicitHostFamily=%d",
        static_cast<int>(role),
        static_cast<unsigned>(port),
        addressCopy.c_str(),
        nicknameCopy.c_str(),
        writeNicknameToIni ? 1 : 0,
        network::FamilyName(sessionFamily),
        writeHostProtocol ? 1 : 0);
    if (writeHostProtocol)
    {
        g_activeHostNetworkConfig = normalizedHostConfig;
        g_activeHostNetworkConfigValid = true;
        mod::Log(
            "REVIVAL_NETPLAY_HOST_SELECTION preferred=%s effective=%s "
            "fallback=%d automaticRetry=%d publicAddress='%s'",
            network::FamilyName(normalizedHostConfig.preferredFamily),
            network::FamilyName(normalizedHostConfig.effectiveFamily),
            normalizedHostConfig.preferredFamily
                    != normalizedHostConfig.effectiveFamily
                ? 1
                : 0,
            normalizedHostConfig.automaticFamilyRetryAttempted
                ? 1
                : 0,
            normalizedHostConfig.publicAddress.c_str());
    }
    else
    {
        g_activeHostNetworkConfig = {};
        g_activeHostNetworkConfigValid = false;
    }
    takeover::ClearHostListenerObservation();
    takeover::ClearDelayPromptState("session_bridge_start_session");
    if (g_synchronousStartRejected)
    {
        mod::Log(
            "REVIVAL_NETPLAY_SYNC_START_REJECTED latched=0 "
            "action=clear_before_queued_attempt role=%d",
            static_cast<int>(role));
        g_synchronousStartRejected = false;
    }
#if defined(_MSC_VER)
    strncpy_s(g_status.address, sizeof(g_status.address), addressCopy.c_str(), _TRUNCATE);
    strncpy_s(g_status.nickname, sizeof(g_status.nickname), nicknameCopy.c_str(), _TRUNCATE);
#else
    std::snprintf(g_status.address, sizeof(g_status.address), "%s", addressCopy.c_str());
    std::snprintf(g_status.nickname, sizeof(g_status.nickname), "%s", nicknameCopy.c_str());
#endif

    SetPhase(NetbridgePhase::Connecting, nullptr);
    g_connectStartTick = GetTickCount();
    takeover::LogRevival102jDeepStep("SessionBridge.StartSession.01.queued", &g_status);

    const uint32_t requestSerial = ++g_startRequestSerial;
    g_startWorkerRunning = true;

    g_startWorker = std::thread([
        requestSerial,
        role,
        port,
        addressCopy,
        iniAddressCopy,
        nicknameCopy,
        writeNicknameToIni,
        sessionFamily,
        writeHostProtocol]() {
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

        takeover::LogRevival102jDeepStep(
            "SessionBridge.StartSession.02.worker_begin",
            &workerStatus);

        uint32_t workerConnectStartTick = GetTickCount();
        const bool started = takeover::StartSession(
            role,
            port,
            addressCopy.c_str(),
            iniAddressCopy.c_str(),
            nicknameCopy.c_str(),
            writeNicknameToIni,
            sessionFamily,
            writeHostProtocol,
            &workerStatus,
            &workerConnectStartTick);
        takeover::LogRevival102jDeepStep(
            started
                ? "SessionBridge.StartSession.03.takeover_returned_success"
                : "SessionBridge.StartSession.03.takeover_returned_failure",
            &workerStatus);

        bool cancelStaleSession = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (requestSerial == g_startRequestSerial)
            {
                g_status = workerStatus;
                g_connectStartTick = workerConnectStartTick;
                takeover::LogRevival102jDeepStep(
                    "SessionBridge.StartSession.04.worker_result_committed",
                    &g_status);
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
                takeover::LogRevival102jDeepStep(
                    "SessionBridge.StartSession.04.worker_result_stale",
                    &workerStatus);
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

bool StartSession(
    NetbridgeRole role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni)
{
    return StartSessionInternal(
        role,
        port,
        address,
        nickname,
        writeNicknameToIni,
        nullptr,
        nullptr);
}

bool StartHostSession(
    uint16_t port,
    const char* nickname,
    const HostSessionNetworkConfig& networkConfig,
    bool writeNicknameToIni,
    HostStartFailure* outFailure)
{
    return StartSessionInternal(
        NetbridgeRole::Host,
        port,
        "",
        nickname,
        writeNicknameToIni,
        &networkConfig,
        outFailure);
}

bool GetActiveHostSessionNetworkConfig(HostSessionNetworkConfig* outConfig)
{
    if (outConfig == nullptr)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_activeHostNetworkConfigValid)
    {
        return false;
    }
    *outConfig = g_activeHostNetworkConfig;
    return true;
}

bool GetHostProtocolOverrideState(HostProtocolOverrideState* outState)
{
    return takeover::GetHostProtocolOverrideState(outState);
}

bool BeginOptionsIniAccess(
    bool writeAccess,
    HostProtocolOverrideState* outState)
{
    return takeover::BeginOptionsIniAccess(writeAccess, outState);
}

void EndOptionsIniAccess()
{
    takeover::EndOptionsIniAccess();
}

bool GetHostListenerObservation(HostListenerObservation* outObservation)
{
    if (outObservation == nullptr)
    {
        return false;
    }

    HostListenerObservation observation = {};
    std::lock_guard<std::mutex> lock(g_mutex);

    LONG serial = 0;
    DWORD processId = 0;
    if (!takeover::ReadHostListenerObservation(
            &serial,
            &observation.family,
            &observation.port,
            &processId))
    {
        *outObservation = observation;
        return false;
    }

    observation.available = true;
    observation.serial = static_cast<uint32_t>(serial);
    observation.processId = static_cast<uint32_t>(processId);
    const DWORD expectedProcessId =
        g_startWorkerRunning
        ? 0
        : static_cast<DWORD>(g_status.processId);
    observation.expectedProcessKnown =
        expectedProcessId != 0;
    observation.processMatches =
        observation.expectedProcessKnown
        && processId == expectedProcessId;
    if (!observation.processMatches)
    {
        if (observation.serial
            != g_lastRejectedHostListenerSerial)
        {
            g_lastRejectedHostListenerSerial =
                observation.serial;
            mod::Log(
                "REVIVAL_HOST_ACK_REJECTED reason=stale_helper "
                "serial=%u observedPid=%lu expectedPid=%lu family=%s "
                "port=%u",
                observation.serial,
                static_cast<unsigned long>(processId),
                static_cast<unsigned long>(
                    expectedProcessId),
                network::FamilyName(observation.family),
                static_cast<unsigned>(observation.port));
        }
        *outObservation = observation;
        return false;
    }
    observation.expectedFamilyKnown = g_activeHostNetworkConfigValid;
    if (observation.expectedFamilyKnown)
    {
        observation.familyMatches =
            observation.family == g_activeHostNetworkConfig.effectiveFamily;
    }
    observation.portMatches =
        g_status.port == 0 || observation.port == g_status.port;
    *outObservation = observation;
    return true;
}

bool IsSessionStartInProgress()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    JoinFinishedWorkerUnlocked();
    return g_startWorkerRunning;
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

bool RequiresNativeVsHumanSyncForHandoff()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return false;
    }

    return takeover::RequiresNativeVsHumanSyncForHandoff(&g_status);
}

bool RequestPeerQuitBeforeLocalExit(const char* reason)
{
    // Older Revival builds already complete their native quit path correctly
    // with the mod installed.  Keep those versions as the control and add the
    // explicit pre-teardown broadcast only for the MinGW 1.02j wire layout.
    takeover::EnsureActiveRevivalProfile();
    const char* const quitWireName = takeover::RevivalWireName("Quit");
    if (quitWireName == nullptr || std::strcmp(quitWireName, "Quit_Spec") != 0)
    {
        mod::Log(
            "SessionBridge: pre-exit peer-quit left to legacy native path "
            "reason='%s' wire='%s'",
            reason != nullptr ? reason : "",
            quitWireName != nullptr ? quitWireName : "");
        return false;
    }

    NetbridgeStatus status = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_initialized)
        {
            return false;
        }
        status = g_status;
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(status.phase);
    const bool activePhase =
        phase == NetbridgePhase::Connecting
        || phase == NetbridgePhase::DelaySetup
        || phase == NetbridgePhase::Connected;
    const bool activeRole =
        status.roleFlag == takeover::kLocalRoleOnline
        || status.roleFlag == takeover::kLocalRoleSpectate;
    if (!activePhase || !activeRole || status.processId == 0)
    {
        mod::Log(
            "SessionBridge: pre-exit peer-quit skipped reason='%s' phase=%s "
            "role=%d roleFlag=%d helperPid=%lu",
            reason != nullptr ? reason : "",
            PhaseToString(phase),
            status.role,
            status.roleFlag,
            static_cast<unsigned long>(status.processId));
        return false;
    }

    const bool sent = takeover::RequestInjectedPeerQuitBroadcast(
        reason != nullptr ? reason : "local_exit",
        300u);
    mod::Log(
        "SessionBridge: pre-exit peer-quit reason='%s' result=%d phase=%s "
        "role=%d roleFlag=%d helperPid=%lu",
        reason != nullptr ? reason : "",
        sent ? 1 : 0,
        PhaseToString(phase),
        status.role,
        status.roleFlag,
        static_cast<unsigned long>(status.processId));
    return sent;
}

void CancelSession(const char* reason)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return;
    }

    ++g_startRequestSerial;
    g_activeHostNetworkConfig = {};
    g_activeHostNetworkConfigValid = false;
    takeover::ClearHostListenerObservation();
    mod::Log("SessionBridge: CancelSession reason='%s'", (reason != nullptr) ? reason : "");

    if (g_synchronousStartRejected && !g_startWorkerRunning)
    {
        const std::string rejectedError = g_status.errorMsg;
        g_synchronousStartRejected = false;
        SetPhase(NetbridgePhase::Idle, nullptr);
        mod::Log(
            "REVIVAL_NETPLAY_SYNC_START_REJECTED latched=0 "
            "action=acknowledge_without_takeover reason='%s' error='%s'",
            reason != nullptr ? reason : "",
            rejectedError.c_str());
        return;
    }

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

bool CompletePendingTournamentReturnCleanup()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return false;
    }

    JoinFinishedWorkerUnlocked();
    const bool completed =
        takeover::CompletePendingTournamentReturnCleanup(&g_status);
    if (completed)
    {
        state_export::Update(g_status);
    }
    return completed;
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
    {
        std::string joinEndpoint = status.address;
        network::NetworkHost host = {};
        if (status.port != 0
            && network::ParseBareHost(status.address, &host))
        {
            network::NetworkEndpoint endpoint = {};
            endpoint.family = host.family;
            endpoint.host = host.host;
            endpoint.port = status.port;
            (void)network::FormatEndpoint(endpoint, &joinEndpoint);
        }
        std::snprintf(
            buffer,
            bufferSize,
            "Nick:%s Host:%u Join:%s Role:%d",
            status.nickname[0] != '\0' ? status.nickname : "Player",
            static_cast<unsigned>(status.port),
            joinEndpoint.c_str(),
            status.roleFlag);
        break;
    }
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
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized || g_status.processId == 0)
    {
        ClosePeerProbeHandleUnlocked();
        return false;
    }

    const DWORD processId =
        static_cast<DWORD>(g_status.processId);
    if (g_peerProbeHandle == nullptr
        || g_peerProbeProcessId != processId)
    {
        ClosePeerProbeHandleUnlocked();
        // Own a stable, minimal-rights handle instead of borrowing takeover's
        // mutable HANDLE, which teardown can close and Windows can reuse.
        g_peerProbeHandle =
            OpenProcess(SYNCHRONIZE, FALSE, processId);
        if (g_peerProbeHandle == nullptr)
        {
            return false;
        }
        g_peerProbeProcessId = processId;
    }

    const DWORD waitResult =
        WaitForSingleObject(g_peerProbeHandle, 0);
    return waitResult == WAIT_TIMEOUT;
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
bool RestoreRevivalTitleDispatchForRecovery(const char* caller)
{
    return takeover::RestoreExeDispatchHookForTitle(caller);
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
