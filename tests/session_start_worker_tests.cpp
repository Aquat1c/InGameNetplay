#include "../src/netplay/bridge/session_bridge.cpp"

#include <cstdlib>
#include <new>

namespace
{
thread_local int g_failAllocationAfter = -1;
int g_armQueuedAllocationFailure = -1;
int g_failures = 0;
int g_workerCalls = 0;
int g_abortCalls = 0;
int g_cleanupCalls = 0;
bool g_workerSucceeds = true;
bool g_blockCleanup = false;
HANDLE g_abortRequested = CreateEventA(nullptr, FALSE, FALSE, nullptr);
HANDLE g_workerEntered = CreateEventA(nullptr, FALSE, FALSE, nullptr);
HANDLE g_workerRelease = CreateEventA(nullptr, FALSE, FALSE, nullptr);
HANDLE g_cleanupEntered = CreateEventA(nullptr, FALSE, FALSE, nullptr);
HANDLE g_cleanupRelease = CreateEventA(nullptr, FALSE, FALSE, nullptr);

void Expect(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++g_failures; }
}

void Wait(HANDLE event)
{
    if (WaitForSingleObject(event, 5000) != WAIT_OBJECT_0)
    {
        std::fprintf(stderr, "FAIL: worker synchronization timed out\n");
        std::abort();
    }
}

void JoinWorker()
{
    using namespace netplay::bridge;
    SetEvent(g_workerRelease);
    if (g_startWorker.joinable()) g_startWorker.join();
}
}

void* operator new(std::size_t size)
{
    if (g_failAllocationAfter >= 0 && g_failAllocationAfter-- == 0)
    {
        g_failAllocationAfter = -1;
        throw std::bad_alloc();
    }
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void operator delete(void* ptr) noexcept { std::free(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { std::free(ptr); }

namespace mod { void Log(const char*, ...) {} }
namespace netplay::connection_check { void Cancel() {} }

namespace netplay::network
{
const char* FamilyName(NetworkFamily) { return "IPv4"; }
const char* ProbeStageName(NetworkProbeStage) { return "test"; }
bool ParseRemoteHostInput(const std::string&, RemoteHostInput*) { return false; }
bool ResolveRemoteEndpoint(const std::string&, uint16_t, NetworkEndpoint*, RemoteEndpointResolution*) { return false; }
bool ParseBareHost(const std::string&, NetworkHost*) { return false; }
bool IsGloballyRoutableHost(const NetworkHost&) { return false; }
}

namespace netplay::bridge::state_export
{
bool IsSuspendedForOnlineSimulation() { return false; }
bool SuspendForOnlineSimulation() { return true; }
void ResumeControlPlaneUpdates() {}
void PublishTrainingHandoffBoundary(const NetbridgeStatus&) {}
void Shutdown() {}
void EmergencyShutdown() {}
}

namespace netplay::bridge::takeover
{
revival_launch::LaunchDisposition g_launchDisposition = {};
int g_localRoleFlag = 0;
int g_netplayRole = 0;
DWORD g_revivalProcessId = 0;

bool InitializeHost() { return true; }
void RefreshRuntimeStatus(NetbridgeStatus*) {}
void Tick(NetbridgeStatus*, uint32_t*, bool*, bool) {}
void ClearHostListenerObservation() {}
void ClearDelayPromptState(const char*) {}
void RequestAbortStart() { ++g_abortCalls; SetEvent(g_abortRequested); }
void StopSpectatorEscWatcher() {}
void ShutdownHost() {}
void EmergencyShutdownHost() {}
void LogRevival102jDeepStep(const char* context, const NetbridgeStatus*)
{
    if (std::strcmp(context, "SessionBridge.StartSession.01.queued") == 0)
    {
        g_failAllocationAfter = g_armQueuedAllocationFailure;
        g_armQueuedAllocationFailure = -1;
    }
}
bool StartSession(NetbridgeRole, uint16_t, const char*, const char*, const char*,
                  bool, network::NetworkFamily, bool, NetbridgeStatus* status,
                  uint32_t* tick)
{
    ++g_workerCalls;
    const HANDLE release = g_workerRelease;
    const bool succeeds = g_workerSucceeds;
    SetEvent(g_workerEntered);
    Wait(release);
    status->phase = static_cast<int>(succeeds
        ? NetbridgePhase::Connected : NetbridgePhase::Failed);
    status->processId = 101;
    if (!succeeds) strcpy_s(status->errorMsg, "takeover failed");
    *tick = 1234;
    return succeeds;
}
void CancelSession(const char* reason, NetbridgeStatus* status)
{
    ++g_cleanupCalls;
    if (std::strcmp(reason, "stale_worker_result") == 0 && g_blockCleanup)
    {
        SetEvent(g_cleanupEntered);
        Wait(g_cleanupRelease);
    }
    if (status) status->phase = static_cast<int>(NetbridgePhase::Idle);
}
}

namespace netplay { bool PrepareExternalLauncherSimulationHandoff() { std::abort(); } }
namespace netplay::bridge::async_host { void Tick() { std::abort(); } }
namespace netplay::bridge::state_export
{
void Initialize() { std::abort(); }
void Update(const NetbridgeStatus&) { std::abort(); }
bool OnlineCharSelectChanged() { std::abort(); }
}
namespace netplay::bridge::takeover
{
const RevivalAddressProfile* g_activeRevival = nullptr;
bool IsCurrentProcessRevival() { std::abort(); }
bool IsRunningUnderWine() { std::abort(); }
int SelfPatchIat() { std::abort(); }
void InitializeInjected() { std::abort(); }
void ShutdownInjected() { std::abort(); }
bool ApplyInputDelay(int, int, NetbridgeStatus*) { std::abort(); }
bool AnswerSpectatePromptChoice(int, NetbridgeStatus*) { std::abort(); }
bool PrepareVsHumanHandoff(NetbridgeStatus*) { std::abort(); }
void OnTitleSelectionConfirmed(int, NetbridgeStatus*) { std::abort(); }
bool ConsumeRevivalExitInterception(int*, NetbridgeStatus*) { std::abort(); }
bool NotifyTitleScreenActive(NetbridgeStatus*) { std::abort(); }
bool CompletePendingTournamentReturnCleanup(NetbridgeStatus*) { std::abort(); }
DelayPromptMetrics GetDelayPromptMetrics() { std::abort(); }
bool IsNetplayExitInterceptionPending() { std::abort(); }
bool ForceLocalPlayInit() { std::abort(); }
bool ForceGameModeToTitle() { std::abort(); }
void EnsureActiveRevivalProfile() { std::abort(); }
const char* RevivalWireName(const char*) { std::abort(); }
void EmergencyQuarantineExternalLauncherAttachBoundary() { std::abort(); }
bool RestoreExeDispatchHookForTitle(const char*) { std::abort(); }
bool RequestInjectedPeerQuitBroadcast(const char*, DWORD) { std::abort(); }
bool ReadHostListenerObservation(LONG*, network::NetworkFamily*, uint16_t*, DWORD*) { std::abort(); }
void HandleTemporaryHostProtocolListenerAck(DWORD, uint16_t) { std::abort(); }
void ConfirmTemporaryHostProtocolListenerAckAtHandoff(DWORD, uint16_t) { std::abort(); }
bool GetHostProtocolOverrideState(HostProtocolOverrideState*) { std::abort(); }
bool BeginOptionsIniAccess(bool, HostProtocolOverrideState*) { std::abort(); }
void EndOptionsIniAccess() { std::abort(); }
bool RequiresNativeVsHumanSyncForHandoff(const NetbridgeStatus*) { std::abort(); }
bool NeedsExternalLauncherSimulationHandoff() { std::abort(); }
bool CompleteExternalLauncherUiAttachment(bool, bool) { std::abort(); }
}

int main()
{
    using namespace netplay::bridge;
    Expect(g_workerEntered && g_workerRelease && g_cleanupEntered && g_cleanupRelease,
           "worker synchronization events are available");
    HostSessionNetworkConfig config;
    HostStartFailure failure = HostStartFailure::None;
    const char* nickname = "nickname longer than the small string buffer";

    for (int failAfter = 0; failAfter != 2; ++failAfter)
    {
        g_armQueuedAllocationFailure = failAfter;
        bool threw = false;
        bool started = false;
        try { started = StartHostSession(10800, nickname, config, false, &failure); }
        catch (...) { threw = true; }
        g_failAllocationAfter = -1;
        Expect(!threw, "worker capture/allocation failure must not escape StartHostSession");
        Expect(!started && !IsSessionStartInProgress(), "failed launch releases startup ownership");
        Expect(GetStatus().phase == static_cast<int>(NetbridgePhase::Failed),
               "failed launch publishes a failed phase");
        Expect(GetStatus().errorMsg[0] != '\0', "failed launch publishes an error");
        HostSessionNetworkConfig activeConfig;
        Expect(!GetActiveHostSessionNetworkConfig(&activeConfig),
               "failed launch clears host configuration");
        Expect(g_workerCalls == failAfter, "failed launch must not enter takeover");
        CancelSession("dismissed_host_error");
        Expect(GetStatus().phase == static_cast<int>(NetbridgePhase::Idle),
               "failed launch can be acknowledged");
        Expect(StartHostSession(10801, "restart", config, false, &failure),
               "a fresh request starts after launch failure");
        Wait(g_workerEntered);
        JoinWorker();
        Expect(!IsSessionStartInProgress()
               && GetStatus().phase == static_cast<int>(NetbridgePhase::Connected)
               && GetStatus().port == 10801,
               "restarted worker commits its own successful result");
    }

    const int cleanupBeforeCancel = g_cleanupCalls;
    Expect(StartHostSession(10802, "cancelled", config, false, &failure), "cancellation fixture starts");
    Wait(g_workerEntered);
    CancelSession("user_cancel");
    CancelSession("user_cancel");
    Expect(IsSessionStartInProgress(), "cancelled startup remains owned until worker retirement");
    Expect(!StartHostSession(10803, "too early", config, false, &failure)
           && failure == HostStartFailure::StartAlreadyInProgress,
           "restart is rejected while cancelled worker is active");
    CancelSession("user_cancel");
    g_blockCleanup = true;
    SetEvent(g_workerRelease);
    Wait(g_cleanupEntered);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Expect(g_start.IsRunning(), "stale-result cleanup retains startup ownership");
    }
    SetEvent(g_cleanupRelease);
    g_startWorker.join();
    g_blockCleanup = false;
    Expect(!IsSessionStartInProgress(), "completed stale cleanup releases startup ownership");
    Expect(GetStatus().phase == static_cast<int>(NetbridgePhase::Idle)
           && GetStatus().processId == 0, "cancelled result never replaces acknowledged status");
    Expect(g_cleanupCalls == cleanupBeforeCancel + 1, "cancelled success cleans native session exactly once");
    Expect(g_abortCalls == 3, "each cancellation reaches the native abort request");

    g_workerSucceeds = false;
    Expect(StartHostSession(10804, "failure", config, false, &failure), "failed takeover fixture starts");
    Wait(g_workerEntered);
    JoinWorker();
    Expect(!IsSessionStartInProgress()
           && GetStatus().phase == static_cast<int>(NetbridgePhase::Failed)
           && std::strcmp(GetStatus().errorMsg, "takeover failed") == 0,
           "worker failure publishes its error and releases ownership");
    g_workerSucceeds = true;
    Expect(StartHostSession(10805, "last restart", config, false, &failure),
           "worker failure allows the next request");
    Wait(g_workerEntered);
    JoinWorker();
    Expect(GetStatus().phase == static_cast<int>(NetbridgePhase::Connected)
           && GetStatus().port == 10805 && GetStatus().processId == 101,
           "next successful generation replaces failed status");

    g_workerSucceeds = false;
    const int cleanupBeforeFailureCancel = g_cleanupCalls;
    Expect(StartHostSession(10806, "cancelled failure", config, false, &failure),
           "cancelled failing worker starts");
    Wait(g_workerEntered);
    CancelSession("user_cancel");
    JoinWorker();
    Expect(GetStatus().phase == static_cast<int>(NetbridgePhase::Idle)
           && g_cleanupCalls == cleanupBeforeFailureCancel,
           "cancelled failed result neither publishes nor cleans a nonexistent session");
    g_workerSucceeds = true;

    Expect(StartHostSession(10807, "shutdown", config, false, &failure),
           "shutdown fixture starts");
    Wait(g_workerEntered);
    ResetEvent(g_abortRequested);
    std::thread shutdownCaller([] { Shutdown(); });
    Wait(g_abortRequested);
    Expect(IsSessionStartInProgress(), "shutdown retains ownership while joining its worker");
    Expect(!StartHostSession(10808, "during shutdown", config, false, &failure)
           && failure == HostStartFailure::StartAlreadyInProgress,
           "shutdown cannot admit a worker before the old worker retires");
    SetEvent(g_workerRelease);
    shutdownCaller.join();
    Expect(!IsSessionStartInProgress()
           && GetStatus().phase == static_cast<int>(NetbridgePhase::Idle),
           "shutdown drains the cancelled worker");

    Expect(StartHostSession(10809, "detached", config, false, &failure),
           "detached worker fixture starts");
    Wait(g_workerEntered);
    HANDLE detachedThread = nullptr;
    Expect(DuplicateHandle(GetCurrentProcess(), g_startWorker.native_handle(),
                           GetCurrentProcess(), &detachedThread, SYNCHRONIZE, FALSE, 0) != FALSE,
           "detached worker remains observable");
    HANDLE detachedRelease = g_workerRelease;
    EmergencyShutdown();
    g_workerRelease = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    Expect(StartHostSession(10810, "new generation", config, false, &failure),
           "new generation starts after emergency retirement");
    Wait(g_workerEntered);
    const int cleanupBeforeLateResult = g_cleanupCalls;
    SetEvent(detachedRelease);
    Wait(detachedThread);
    Expect(IsSessionStartInProgress()
           && GetStatus().phase == static_cast<int>(NetbridgePhase::Connecting)
           && GetStatus().port == 10810,
           "detached old worker cannot release or publish into a later generation");
    Expect(g_cleanupCalls == cleanupBeforeLateResult,
           "detached old result cannot cancel a later native session");
    JoinWorker();
    CloseHandle(detachedThread);
    CloseHandle(detachedRelease);
    Shutdown();
    CloseHandle(g_abortRequested);
    CloseHandle(g_workerEntered);
    CloseHandle(g_workerRelease);
    CloseHandle(g_cleanupEntered);
    CloseHandle(g_cleanupRelease);
    return g_failures ? 1 : 0;
}
