// Session lifecycle: host/join/spectate management, tick loop, cancel, delay.
// Internal helpers live in sister .cpp files; see takeover_internal.h.

#include "netplay/bridge/revival_takeover.h"
#include "netplay/bridge/takeover_internal.h"

#include "logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

#include <windows.h>

namespace netplay::bridge::takeover
{

// ---------------------------------------------------------------------------
// Global variable definitions (declared extern in takeover_internal.h).
// ---------------------------------------------------------------------------

std::mutex g_mutex;
const RevivalAddressProfile* g_activeRevival = &kRevival_1_02e;

HMODULE g_localRevivalModule = nullptr;
RevivalInitFn g_localInitFn = nullptr;
HANDLE g_revivalProcess = nullptr;
DWORD g_revivalProcessId = 0;
int g_localRoleFlag = -1;
uintptr_t g_hostRevivalBase = 0;

HANDLE g_hostMapHandle = nullptr;
SharedBlock* g_hostBlock = nullptr;
HANDLE g_hostInitEvent = nullptr;
HANDLE g_hostConsoleEvent = nullptr;

bool g_injectedReady = false;
HANDLE g_injectedMapHandle = nullptr;
SharedBlock* g_injectedBlock = nullptr;
HANDLE g_injectedInitEvent = nullptr;
HANDLE g_injectedConsoleEvent = nullptr;
volatile LONG g_injectedLastConsoleSerialServed = 0;
volatile LONG g_injectedLastConsoleAuxSerialServed = 0;
volatile LONG g_injectedActiveConsoleAuxSerial = 0;
volatile LONG g_injectedConsoleAuxScriptOffset = 0;
volatile LONG g_injectedAutoConsoleFallbackCount = 0;
volatile LONG g_injectedConsoleOutputHits = 0;
volatile LONG g_injectedTerminateUnknownPidHits = 0;
volatile LONG g_injectedDelayPromptSerial = 0;
volatile LONG g_injectedDelayPromptServedSerial = 0;
volatile LONG g_injectedConnectedFromDelayPromptSerial = 0;
DWORD g_injectedDelayPromptWaitStartTick = 0;
volatile LONG g_injectedSpectateConfirmPromptSerial = 0;
volatile LONG g_injectedSpectateConfirmPromptServedSerial = 0;
DWORD g_injectedSpectateConfirmPromptWaitStartTick = 0;
volatile LONG g_injectedFingerprintReadHits = 0;
uintptr_t g_injectedInitAddress = 0;
bool g_injectedLazyBound = false;
volatile LONG g_injectedLazyBootstrapState = 0;
volatile LONG g_remoteThreadCallIndex = 0;
volatile LONG g_startAbortRequested = 0;
HANDLE g_fakeProcessThreadHandle = nullptr;
bool g_initCapturedFromWrite = false;
DWORD g_lastConnectingDiagnosticTick = 0;
uintptr_t g_lastSessionPtrOffset = 0;
uintptr_t g_lastValidatedSessionPtr = 0;
DWORD g_lastSessionPointerMismatchTick = 0;
DWORD g_lastRuntimeReadyProbeLogTick = 0;
uint32_t g_lastRuntimeReadyProbeMask = 0;
bool g_lastRuntimeReadyProbeMaskValid = false;
bool g_localInitAppliedForSession = false;
uintptr_t g_remoteInjectedSelfBase = 0;
DWORD g_lastLatePatchRetryTick = 0;
DWORD g_lastLatePatchRetryLogTick = 0;
DWORD g_latePatchRetryAttempts = 0;
DWORD g_latePatchRetrySuccesses = 0;
bool g_lastLatePatchRetryResultValid = false;
bool g_lastLatePatchRetryResult = false;
bool g_observedTakeoverCreatePath = false;
std::mutex g_fakeThreadMutex;
std::vector<FakeThreadInfo> g_fakeThreads;
std::mutex g_redirectAllocMutex;
std::vector<RedirectAllocationInfo> g_redirectAllocations;
volatile LONG g_redirectWriteBlockedHits = 0;
volatile LONG g_sessionHistoryRepairHits = 0;
std::mutex g_consoleLogMutex;
std::string g_consolePendingWriteFile;
std::string g_consolePendingWriteFileDisk;
std::string g_consolePendingWriteConsoleA;
std::string g_consolePendingWriteConsoleW;
std::string g_consolePendingWriteConsoleOutputCharacterA;
std::string g_consolePendingWriteConsoleOutputCharacterW;
std::string g_consolePendingOutputDebugStringA;
std::string g_consolePendingOutputDebugStringW;
std::unordered_map<std::string, LONG> g_diskCapturePathHits;
bool g_captureRevivalNativeLogsConfigured = false;
bool g_captureRevivalNativeLogs = false;
bool g_revivalErrorCodeNullGuardPatched = false;
uintptr_t g_revivalErrorCodeNullGuardPatchedBase = 0;
void* g_revivalErrorCodeNullGuardStub = nullptr;
DelayPromptMetrics g_delayPromptMetrics = {};
volatile LONG g_revivalExitIntercepted = 0;
volatile LONG g_revivalExitMode = -1;
bool g_nativeWorkflowLoadedSeen = false;
bool g_nativeWorkflowMatchLoopSeen = false;
bool g_nativeWorkflowTournamentSeen = false;
bool g_nativeWorkflowPeerDiedSeen = false;
bool g_nativeWorkflowHolePunchDiedSeen = false;
bool g_holePunchServerConfigLoaded = false;
std::string g_configuredHolePunchServer;

// ---------------------------------------------------------------------------
// Session lifecycle functions (public API from revival_takeover.h).
// ---------------------------------------------------------------------------

void InitializeHost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    (void)EnsureHostIpc();
    if (!EnsureLocalRevivalLoaded())
    {
        mod::Log("Takeover: host local revival load failed");
    }
    else
    {
        (void)SetLocalRoleFlag(kLocalRoleLocalPlay, "host_initialize");
    }
    mod::Log("Takeover: host initialized");
}

void ShutdownHost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_revivalProcess != nullptr)
    {
        TerminateProcess(g_revivalProcess, 0);
    }
    CloseProcessHandle(nullptr);
    CloseHostIpc();
    g_localRoleFlag = -1;
    g_localInitAppliedForSession = false;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_injectedDelayPromptWaitStartTick = 0;
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    g_lastLatePatchRetryTick = 0;
    g_observedTakeoverCreatePath = false;
    g_remoteInjectedSelfBase = 0;
    mod::Log("Takeover: host shutdown");
}

void EmergencyShutdownHost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    InterlockedExchange(&g_startAbortRequested, 1);
    if (g_revivalProcess != nullptr)
    {
        TerminateProcess(g_revivalProcess, 0);
    }
    CloseProcessHandle(nullptr);
    CloseHostIpc();
    g_localRoleFlag = -1;
    g_localInitAppliedForSession = false;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_injectedDelayPromptWaitStartTick = 0;
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    g_lastLatePatchRetryTick = 0;
    g_observedTakeoverCreatePath = false;
    g_remoteInjectedSelfBase = 0;
}

void RequestAbortStart()
{
    InterlockedExchange(&g_startAbortRequested, 1);
}

void OnTitleSelectionConfirmed(int selection, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!EnsureLocalRevivalLoaded())
    {
        return;
    }

    const bool sessionActive = ProcessAlive(ioStatus);
    if (sessionActive)
    {
        return;
    }

    // If we're currently in tournament mode, perform full cleanup before
    // doing anything else.  The DLL call-site patches make ExitProcess
    // unreachable, so the old ConsumeRevivalExitInterception path never
    // fires — we must clean up here instead.
    //
    // This also handles tournament re-entry (selection == 2 a second
    // time): without cleanup the EXE patches would be saved in their
    // already-patched state and the DLL patches would be skipped
    // because they're already 0xEB.  Resetting g_localRoleFlag to
    // kLocalRoleLocalPlay lets SetLocalRoleFlag call init(3,102) again.
    if (g_localRoleFlag == kLocalRoleTournament)
    {
        RestoreDllExitProcessPatches();
        RestoreTournamentExePatches();
        ForceLocalPlayInit();
        ClearRevivalText();
        DisableRevivalTextRendering();
        g_localRoleFlag = kLocalRoleLocalPlay;
        mod::Log("Takeover: tournament cleanup before selection=%d", selection);
    }

    if (selection == 2)
    {
        // VS Human: create a real tournament session via init(3,102).
        // This gives us win counters, player nicknames, match result
        // logging, and the EXE patches that tournament mode applies.
        // Save the original EXE bytes first so we can restore them when
        // the tournament exits, then neutralize the auto-navigation
        // input queue so the 22-entry button sequence doesn't cause
        // phantom inputs during character select.
        //
        // The DLL call-site patches must be applied BEFORE init(3,102)
        // returns, because the tournament session's tick function runs
        // on the very next frame and would call ExitProcess immediately
        // (mode is still 0 = title screen).
        (void)SaveRenderContext();
        (void)SaveTournamentExePatches();
        (void)SaveAndApplyDllExitProcessPatches();
        (void)SetLocalRoleFlag(kLocalRoleTournament, "title_vs_human");
        (void)NeutralizeTournamentAutoNav();
    }
    else
    {
        (void)SetRoleFlagDirect(kLocalRoleLocalPlay, "title_other");
    }
    RefreshRuntimeStatus(ioStatus);
}

bool StartSession(NetbridgeRole role, uint16_t port, const char* address, const char* nickname, NetbridgeStatus* ioStatus, uint32_t* outConnectStartTick)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    mod::Log(
        "Takeover: StartSession role=%d port=%u address='%s' nickname='%s'",
        static_cast<int>(role),
        static_cast<unsigned>(port),
        (address != nullptr) ? address : "",
        (nickname != nullptr) ? nickname : "");

    InterlockedExchange(&g_startAbortRequested, 0);

    if (!EnsureHostIpc())
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "IPC setup failed");
        return false;
    }
    if (!EnsureLocalRevivalLoaded())
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "EfzRevival.dll unavailable");
        return false;
    }

    if (ProcessAlive(ioStatus))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "Session already active");
        return false;
    }

    CloseProcessHandle(ioStatus);

    if (ioStatus != nullptr)
    {
        ioStatus->role = static_cast<int>(role);
        ioStatus->port = port;
        CopyString(ioStatus->address, sizeof(ioStatus->address), address);
        CopyString(ioStatus->nickname, sizeof(ioStatus->nickname), nickname);
    }
    SetPhase(ioStatus, NetbridgePhase::Connecting, nullptr);
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    if (outConnectStartTick != nullptr)
    {
        *outConnectStartTick = GetTickCount();
    }

    const std::string gameDir = GameDirectory();
    if (!WriteIni(gameDir, static_cast<int>(role), port, address, nickname))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "EfzRevival.ini write failed");
        return false;
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::string exePath = gameDir;
    if (!exePath.empty())
    {
        exePath += "\\";
    }
    exePath += "EfzRevival.exe";

    BOOL created = CreateProcessA(
        exePath.c_str(),
        nullptr,
        nullptr,
        nullptr,
        FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr,
        gameDir.empty() ? nullptr : gameDir.c_str(),
        &si,
        &pi);

    if (!created)
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "CreateProcess(EfzRevival.exe) failed");
        return false;
    }

    mod::Log("Takeover: spawned EfzRevival suspended pid=%lu", static_cast<unsigned long>(pi.dwProcessId));

    uintptr_t remoteBase = 0;
    if (!InjectSelf(pi.hProcess, &remoteBase))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "Self injection failed");
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    std::unordered_map<std::string, uint32_t> patches = BuildPatchMap(remoteBase);

    if (!PatchIat(pi.hProcess, pi.dwProcessId, patches, true))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "IAT patch failed");
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    int localRoleMode = kLocalRoleOnline;
    if (role == NetbridgeRole::Spectate || role == NetbridgeRole::JoinSpectate)
    {
        localRoleMode = kLocalRoleSpectate;
    }

    g_hostBlock->initParams[0] = localRoleMode;
    g_hostBlock->initParams[1] = 102;
    ResetDebugCounters(g_hostBlock);
    g_localInitAppliedForSession = false;
    g_sessionHistoryRepairHits = 0;
    InterlockedExchange(&g_injectedDelayPromptSerial, 0);
    InterlockedExchange(&g_injectedDelayPromptServedSerial, 0);
    InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, 0);
    g_injectedDelayPromptWaitStartTick = 0;
    InterlockedExchange(&g_injectedSpectateConfirmPromptSerial, 0);
    InterlockedExchange(&g_injectedSpectateConfirmPromptServedSerial, 0);
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_holePunchServerConfigLoaded = false;
    g_configuredHolePunchServer.clear();
    InterlockedExchange(&g_injectedFingerprintReadHits, 0);
    {
        std::lock_guard<std::mutex> consoleLock(g_consoleLogMutex);
        g_consolePendingWriteFile.clear();
        g_consolePendingWriteFileDisk.clear();
        g_consolePendingWriteConsoleA.clear();
        g_consolePendingWriteConsoleW.clear();
        g_consolePendingWriteConsoleOutputCharacterA.clear();
        g_consolePendingWriteConsoleOutputCharacterW.clear();
        g_consolePendingOutputDebugStringA.clear();
        g_consolePendingOutputDebugStringW.clear();
        g_diskCapturePathHits.clear();
    }
    InterlockedIncrement(&g_hostBlock->initSerial);

    std::string primaryInput;
    std::string auxInput;
    int menuChoice = 0;
    if (role == NetbridgeRole::Host)
    {
        menuChoice = 1;
        primaryInput = "1\r\n";
        auxInput = "\r\n";
    }
    else if (role == NetbridgeRole::Join)
    {
        menuChoice = 3;
        primaryInput = "3\r\n";

        if (address != nullptr && address[0] != '\0')
        {
            std::string clipboardAddress = address;
            clipboardAddress.erase(
                std::remove_if(
                    clipboardAddress.begin(),
                    clipboardAddress.end(),
                    [](char c) { return c == '\r' || c == '\n'; }),
                clipboardAddress.end());

            if (!clipboardAddress.empty())
            {
                if (clipboardAddress.find(':') == std::string::npos && port > 0)
                {
                    char portSuffix[16] = {};
                    std::snprintf(portSuffix, sizeof(portSuffix), ":%u", static_cast<unsigned>(port));
                    clipboardAddress += portSuffix;
                }

                if (TryWriteClipboardAscii(clipboardAddress.c_str()))
                {
                    mod::Log("Takeover: join clipboard seeded with address '%s'", clipboardAddress.c_str());
                }
                else
                {
                    mod::Log("Takeover: join clipboard write failed; Revival will use existing clipboard");
                }
            }
        }
    }
    else if (role == NetbridgeRole::JoinSpectate)
    {
        // Join (choice 3) then auto-accept spectate redirect ("1" = Yes).
        // This is the correct flow when the lobby shows players already in a
        // match — we connect via join and Revival will prompt "Host already
        // playing, join as a spectator?" which we answer automatically.
        menuChoice = 3;
        primaryInput = "3\r\n";
        auxInput = "1\r\n";

        if (address != nullptr && address[0] != '\0')
        {
            std::string clipboardAddress = address;
            clipboardAddress.erase(
                std::remove_if(
                    clipboardAddress.begin(),
                    clipboardAddress.end(),
                    [](char c) { return c == '\r' || c == '\n'; }),
                clipboardAddress.end());

            if (!clipboardAddress.empty())
            {
                if (clipboardAddress.find(':') == std::string::npos && port > 0)
                {
                    char portSuffix[16] = {};
                    std::snprintf(portSuffix, sizeof(portSuffix), ":%u", static_cast<unsigned>(port));
                    clipboardAddress += portSuffix;
                }

                if (TryWriteClipboardAscii(clipboardAddress.c_str()))
                {
                    mod::Log("Takeover: join-spectate clipboard seeded with address '%s'", clipboardAddress.c_str());
                }
                else
                {
                    mod::Log("Takeover: join-spectate clipboard write failed; Revival will use existing clipboard");
                }
            }
        }
    }
    else if (role == NetbridgeRole::Spectate)
    {
        menuChoice = 4;
        primaryInput = "4\r\n";

        if (address != nullptr && address[0] != '\0')
        {
            std::string clipboardAddress = address;
            clipboardAddress.erase(
                std::remove_if(
                    clipboardAddress.begin(),
                    clipboardAddress.end(),
                    [](char c) { return c == '\r' || c == '\n'; }),
                clipboardAddress.end());

            if (clipboardAddress.empty())
            {
                mod::Log("Takeover: spectate address empty after trim; using clipboard option as-is");
            }
            else
            {
                if (clipboardAddress.find(':') == std::string::npos && port > 0)
                {
                    char portSuffix[16] = {};
                    std::snprintf(portSuffix, sizeof(portSuffix), ":%u", static_cast<unsigned>(port));
                    clipboardAddress += portSuffix;
                }

                if (!TryWriteClipboardAscii(clipboardAddress.c_str()))
                {
                    mod::Log("Takeover: spectate clipboard write failed; using option 4 with existing clipboard");
                }
                else
                {
                    mod::Log("Takeover: spectate clipboard seeded with address '%s'", clipboardAddress.c_str());
                }
            }
        }
    }

    if (primaryInput.empty())
    {
        primaryInput = "1\r\n";
        menuChoice = 1;
    }

    CopyString(g_hostBlock->consoleInput, sizeof(g_hostBlock->consoleInput), primaryInput.c_str());
    InterlockedIncrement(&g_hostBlock->consoleSerial);
    g_hostBlock->consoleInputAux[0] = '\0';
    LONG auxSerialForSession = 0;
    if (!auxInput.empty())
    {
        CopyString(g_hostBlock->consoleInputAux, sizeof(g_hostBlock->consoleInputAux), auxInput.c_str());
        auxSerialForSession = InterlockedIncrement(&g_hostBlock->consoleAuxSerial);
    }

    ResetEvent(g_hostInitEvent);
    ResetEvent(g_hostConsoleEvent);

    const DWORD resumeResult = ResumeThread(pi.hThread);
    mod::Log("Takeover: resumed main thread result=%lu", static_cast<unsigned long>(resumeResult));

    g_revivalProcess = pi.hProcess;
    g_revivalProcessId = pi.dwProcessId;
    g_remoteInjectedSelfBase = remoteBase;
    g_lastLatePatchRetryTick = GetTickCount();
    g_lastLatePatchRetryLogTick = 0;
    g_latePatchRetryAttempts = 0;
    g_latePatchRetrySuccesses = 0;
    g_lastLatePatchRetryResultValid = false;
    g_lastLatePatchRetryResult = false;
    g_observedTakeoverCreatePath = false;
    if (ioStatus != nullptr)
    {
        ioStatus->processId = g_revivalProcessId;
    }

    CloseHandle(pi.hThread);

    SetEvent(g_hostConsoleEvent);
    mod::Log(
        "Takeover: signaled console script menu=%d primaryLen=%u auxLen=%u auxSerial=%ld",
        menuChoice,
        static_cast<unsigned>(std::strlen(g_hostBlock->consoleInput)),
        static_cast<unsigned>(std::strlen(g_hostBlock->consoleInputAux)),
        static_cast<long>(auxSerialForSession));
    SetPhase(ioStatus, NetbridgePhase::Connecting, "awaiting handshake");
    RefreshRuntimeStatus(ioStatus);
    mod::Log("Takeover: start session armed (asynchronous handshake via Tick)");
    return true;
}

bool ApplyInputDelay(int delayFrames, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (delayFrames < 0 || delayFrames > 20)
    {
        mod::Log("Takeover: ApplyInputDelay rejected out-of-range value=%d", delayFrames);
        return false;
    }

    LONG delayPromptSerial = 0;
    LONG delayPromptServedSerial = 0;
    ReadDelayPromptSignal(&delayPromptSerial, &delayPromptServedSerial);
    const bool promptPending = delayPromptSerial > 0 && delayPromptServedSerial < delayPromptSerial;

    auto queuePromptInput = [&](int value) -> bool {
        if (g_hostBlock == nullptr)
        {
            return false;
        }

        g_hostBlock->delayInputValue = value;
        const LONG inputSerial = InterlockedIncrement(&g_hostBlock->delayInputSerial);
        if (g_hostConsoleEvent != nullptr)
        {
            SetEvent(g_hostConsoleEvent);
        }
        mod::Log(
            "Takeover: ApplyInputDelay queued prompt input value=%d promptSerial=%ld inputSerial=%ld",
            value,
            static_cast<long>(delayPromptSerial),
            static_cast<long>(inputSerial));
        RefreshRuntimeStatus(ioStatus);
        if (ioStatus != nullptr)
        {
            ioStatus->rollbackFrames = value;
        }
        return true;
    };

    if (promptPending && queuePromptInput(delayFrames))
    {
        return true;
    }

    const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
    if (sessionPtr == 0)
    {
        mod::Log("Takeover: ApplyInputDelay failed (session pointer unavailable)");
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    void* const delayAddress = reinterpret_cast<void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay);
    if (!IsWritableRange(delayAddress, sizeof(int)))
    {
        mod::Log(
            "Takeover: ApplyInputDelay failed (delay field not writable addr=0x%p)",
            delayAddress);
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    *reinterpret_cast<int*>(delayAddress) = delayFrames;
    mod::Log(
        "Takeover: ApplyInputDelay applied value=%d session=0x%08lX",
        delayFrames,
        static_cast<unsigned long>(sessionPtr));
    RefreshRuntimeStatus(ioStatus);
    if (ioStatus != nullptr)
    {
        ioStatus->rollbackFrames = delayFrames;
    }
    return true;
}

bool AnswerSpectateConfirm(bool acceptSpectate, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    LONG promptSerial = 0;
    LONG promptServedSerial = 0;
    ReadSpectateConfirmPromptSignal(&promptSerial, &promptServedSerial);
    const bool promptPending = promptSerial > 0 && promptServedSerial < promptSerial;

    if (!promptPending)
    {
        mod::Log("Takeover: AnswerSpectateConfirm rejected (no pending prompt serial=%ld served=%ld)",
                 static_cast<long>(promptSerial), static_cast<long>(promptServedSerial));
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    if (g_hostBlock == nullptr)
    {
        mod::Log("Takeover: AnswerSpectateConfirm rejected (no shared block)");
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    const int value = acceptSpectate ? 1 : 2;
    g_hostBlock->spectateConfirmInputValue = value;
    const LONG inputSerial = InterlockedIncrement(&g_hostBlock->spectateConfirmInputSerial);
    if (g_hostConsoleEvent != nullptr)
    {
        SetEvent(g_hostConsoleEvent);
    }
    mod::Log(
        "Takeover: AnswerSpectateConfirm queued input accept=%d value=%d promptSerial=%ld inputSerial=%ld",
        acceptSpectate ? 1 : 0,
        value,
        static_cast<long>(promptSerial),
        static_cast<long>(inputSerial));
    RefreshRuntimeStatus(ioStatus);
    return true;
}

bool PrepareVsHumanHandoff(NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!EnsureLocalRevivalLoaded())
    {
        mod::Log("Takeover: PrepareVsHumanHandoff failed (EfzRevival.dll unavailable)");
        return false;
    }

    if (ioStatus == nullptr || ioStatus->localInitApplied == 0)
    {
        static DWORD s_lastNotReadyLogTick = 0;
        const DWORD now = GetTickCount();
        if (s_lastNotReadyLogTick == 0 || now - s_lastNotReadyLogTick >= 2000)
        {
            s_lastNotReadyLogTick = now;
            mod::Log(
                "Takeover: PrepareVsHumanHandoff deferred localInitApplied=%d phase=%s",
                ioStatus != nullptr ? ioStatus->localInitApplied : 0,
                ioStatus != nullptr ? netplay::bridge::PhaseToString(static_cast<NetbridgePhase>(ioStatus->phase)) : "unknown");
        }
        return false;
    }

    const bool roleSet = (g_localRoleFlag >= 0);
    RefreshRuntimeStatus(ioStatus);

    const bool syncReady = IsSyncReadyForVsHuman(ioStatus);
    const int mode = ioStatus != nullptr ? ioStatus->syncGameMode : -1;
    const int flag1084 = ioStatus != nullptr ? ioStatus->syncMode0Flag1084 : -1;
    const int sessionByte = ioStatus != nullptr ? ioStatus->syncSessionByte : -1;
    const int flag4964 = ioStatus != nullptr ? ioStatus->syncGlobalFlag4964 : -1;
    const int flag4965 = ioStatus != nullptr ? ioStatus->syncGlobalFlag4965 : -1;
    const int roleFlag = ioStatus != nullptr ? ioStatus->roleFlag : -1;

    static DWORD s_lastLogTick = 0;
    static int s_lastRoleSet = -1;
    static int s_lastSyncReady = -1;
    static int s_lastMode = std::numeric_limits<int>::min();
    static int s_lastFlag1084 = std::numeric_limits<int>::min();
    static int s_lastSessionByte = std::numeric_limits<int>::min();
    static int s_lastFlag4964 = std::numeric_limits<int>::min();
    static int s_lastFlag4965 = std::numeric_limits<int>::min();
    static int s_lastRoleFlag = std::numeric_limits<int>::min();
    const DWORD now = GetTickCount();
    const bool changed =
        s_lastRoleSet != (roleSet ? 1 : 0)
        || s_lastSyncReady != (syncReady ? 1 : 0)
        || s_lastMode != mode
        || s_lastFlag1084 != flag1084
        || s_lastSessionByte != sessionByte
        || s_lastFlag4964 != flag4964
        || s_lastFlag4965 != flag4965
        || s_lastRoleFlag != roleFlag;
    if (changed || s_lastLogTick == 0 || now - s_lastLogTick >= 3000)
    {
        s_lastRoleSet = roleSet ? 1 : 0;
        s_lastSyncReady = syncReady ? 1 : 0;
        s_lastMode = mode;
        s_lastFlag1084 = flag1084;
        s_lastSessionByte = sessionByte;
        s_lastFlag4964 = flag4964;
        s_lastFlag4965 = flag4965;
        s_lastRoleFlag = roleFlag;
        s_lastLogTick = now;
        mod::Log(
            "Takeover: PrepareVsHumanHandoff roleSet=%d syncReady=%d mode=%d flag1084=%d sessionByte=%d flags=%d/%d roleFlag=%d",
            roleSet ? 1 : 0,
            syncReady ? 1 : 0,
            mode,
            flag1084,
            sessionByte,
            flag4964,
            flag4965,
            roleFlag);
    }
    return roleSet || syncReady;
}

void Tick(NetbridgeStatus* ioStatus, uint32_t* ioConnectStartTick)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ioStatus == nullptr)
    {
        return;
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(ioStatus->phase);
    if (phase != NetbridgePhase::Connecting
        && phase != NetbridgePhase::DelaySetup
        && phase != NetbridgePhase::Connected)
    {
        RefreshRuntimeStatus(ioStatus);
        return;
    }

    if (!ProcessAlive(ioStatus))
    {
        RefreshRuntimeStatus(ioStatus);
        const bool runtimeReady = HasRuntimeReadySignal(ioStatus);
        if (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
        {
            if (g_localInitAppliedForSession)
            {
                if (runtimeReady)
                {
                    SetPhase(ioStatus, NetbridgePhase::Connected, nullptr);
                    if (ioConnectStartTick != nullptr)
                    {
                        *ioConnectStartTick = GetTickCount();
                    }
                }
                else if (ioStatus->delaySetupReady != 0)
                {
                    SetPhase(ioStatus, NetbridgePhase::DelaySetup, nullptr);
                    mod::Log("Takeover: helper process exited after init; keeping delay setup stage active");
                }
                else
                {
                    SetPhase(ioStatus, NetbridgePhase::Connecting, "awaiting runtime sync");
                    mod::Log("Takeover: helper process exited after init; still waiting for runtime sync");
                }
            }
            else
            {
                SetPhase(ioStatus, NetbridgePhase::Failed, "EfzRevival process ended during connect");
                ReinitLocalPlay();
            }
        }
        else
        {
            if (!runtimeReady)
            {
                SetPhase(ioStatus, NetbridgePhase::SessionEnded, nullptr);
                ReinitLocalPlay();
            }
            else
            {
                mod::Log("Takeover: helper process exited but runtime session still active");
            }
        }
        return;
    }

    if (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
    {
        const DWORD now = GetTickCount();
        const LONG cpHits = static_cast<LONG>(g_hostBlock != nullptr ? g_hostBlock->dbgCreateProcessHits : 0);
        const LONG wpmHits = static_cast<LONG>(g_hostBlock != nullptr ? g_hostBlock->dbgWriteProcessHits : 0);
        const LONG crtHits = static_cast<LONG>(g_hostBlock != nullptr ? g_hostBlock->dbgCreateRemoteThreadHits : 0);
        if (!g_observedTakeoverCreatePath && (cpHits > 0 || wpmHits > 0 || crtHits > 0))
        {
            g_observedTakeoverCreatePath = true;
            mod::Log(
                "Takeover: observed takeover create path hits cp=%ld wpm=%ld crt=%ld",
                static_cast<long>(cpHits),
                static_cast<long>(wpmHits),
                static_cast<long>(crtHits));
        }

        if (!g_observedTakeoverCreatePath
            && g_revivalProcess != nullptr
            && g_revivalProcessId != 0
            && g_remoteInjectedSelfBase != 0
            && (g_lastLatePatchRetryTick == 0 || now - g_lastLatePatchRetryTick >= 500))
        {
            const std::unordered_map<std::string, uint32_t> patches = BuildPatchMap(g_remoteInjectedSelfBase);
            const bool patchedLate = PatchIat(g_revivalProcess, g_revivalProcessId, patches, false);
            ++g_latePatchRetryAttempts;
            if (patchedLate)
            {
                ++g_latePatchRetrySuccesses;
            }
            const bool resultChanged = !g_lastLatePatchRetryResultValid || g_lastLatePatchRetryResult != patchedLate;
            if (resultChanged
                || g_lastLatePatchRetryLogTick == 0
                || now - g_lastLatePatchRetryLogTick >= kLatePatchRetryLogIntervalMs)
            {
                mod::Log(
                    "Takeover: late PatchIat retry result=%d attempts=%lu success=%lu elapsed=%lums",
                    patchedLate ? 1 : 0,
                    static_cast<unsigned long>(g_latePatchRetryAttempts),
                    static_cast<unsigned long>(g_latePatchRetrySuccesses),
                    static_cast<unsigned long>(ioStatus->phaseTick != 0 ? (now - ioStatus->phaseTick) : 0));
                g_lastLatePatchRetryLogTick = now;
            }
            g_lastLatePatchRetryResult = patchedLate;
            g_lastLatePatchRetryResultValid = true;
            g_lastLatePatchRetryTick = now;
        }

        if (g_lastConnectingDiagnosticTick == 0 || now - g_lastConnectingDiagnosticTick >= 2000)
        {
            g_lastConnectingDiagnosticTick = now;
            const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
            const uintptr_t looseSessionPtr = ReadSessionPointerFromRevivalLoose();
            int pingMs = -1;
            int delayFrames = -1;
            const int roleFromDll = ReadRoleFlagFromRevival();
            RevivalSyncFlags syncFlags = {};
            (void)ReadRevivalSyncFlags(&syncFlags);
            if (sessionPtr != 0)
            {
                (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetPingMs), &pingMs);
                (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay), &delayFrames);
            }
            if (sessionPtr != 0
                && looseSessionPtr != 0
                && looseSessionPtr != sessionPtr
                && (g_lastSessionPointerMismatchTick == 0 || now - g_lastSessionPointerMismatchTick >= 2000))
            {
                g_lastSessionPointerMismatchTick = now;
                mod::Log(
                    "Takeover: session pointer mismatch strict=0x%08lX loose=0x%08lX offset=0x%04lX",
                    static_cast<unsigned long>(sessionPtr),
                    static_cast<unsigned long>(looseSessionPtr),
                    static_cast<unsigned long>(g_lastSessionPtrOffset));
            }
            const int pingDiag = (pingMs >= 0 && pingMs < 60000) ? pingMs : -1;
            const int delayDiag = (delayFrames >= 0 && delayFrames < 128) ? delayFrames : -1;
            const int exeSyncDiag =
                (syncFlags.gameMode >= 0 || syncFlags.mode0Flag1084 >= 0)
                    ? 1
                    : 0;
            const int dllSyncDiag =
                (syncFlags.sessionByte >= 0
                 || syncFlags.globalFlag4964 >= 0
                 || syncFlags.globalFlag4965 >= 0)
                    ? 1
                    : 0;
            const int roleFromDllDiag = (roleFromDll >= 0) ? 1 : 0;
            const int sessionFromDllDiag = (sessionPtr != 0) ? 1 : 0;
            mod::Log(
                "Takeover: connecting diag session=0x%08lX sessionOff=0x%04lX roleFlag=%d ping=%d delay=%d sync(mode=%d flag1084=%d session=%d flags=%d/%d ready=%d) src(exe=%d dll=%d roleDll=%d sessDll=%d) hits(rc=%ld auto=%ld cp=%ld wpm=%ld crt=%ld)",
                static_cast<unsigned long>(sessionPtr),
                static_cast<unsigned long>(g_lastSessionPtrOffset),
                ioStatus->roleFlag,
                pingDiag,
                delayDiag,
                syncFlags.gameMode,
                syncFlags.mode0Flag1084,
                syncFlags.sessionByte,
                syncFlags.globalFlag4964,
                syncFlags.globalFlag4965,
                syncFlags.inRollbackSyncState ? 1 : 0,
                exeSyncDiag,
                dllSyncDiag,
                roleFromDllDiag,
                sessionFromDllDiag,
                static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgReadConsoleHits : 0),
                static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgReadConsoleAutoHits : 0),
                static_cast<long>(cpHits),
                static_cast<long>(wpmHits),
                static_cast<long>(crtHits));
        }
    }

    if ((phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
        && !g_localInitAppliedForSession
        && g_hostInitEvent != nullptr
        && g_hostBlock != nullptr
        && g_localInitFn != nullptr)
    {
        const DWORD initReady = WaitForSingleObject(g_hostInitEvent, 0);
        if (initReady == WAIT_OBJECT_0)
        {
            int initParams[2] = {g_hostBlock->initParams[0], g_hostBlock->initParams[1]};
            if (initParams[1] == 0)
            {
                initParams[1] = 102;
            }

            mod::Log(
                "Takeover: init handshake observed (deferred) mode=%d magic=%d serial=%ld",
                initParams[0],
                initParams[1],
                static_cast<long>(g_hostBlock->initSerial));

            const int initResult = g_localInitFn(initParams);
            g_localRoleFlag = initParams[0];
            g_localInitAppliedForSession = true;
            mod::Log(
                "Takeover: local init(mode=%d magic=%d) result=%d [deferred]",
                initParams[0],
                initParams[1],
                initResult);

            const bool startInitOk = InvokeStartInitPlayer(initParams[0]);
            mod::Log(
                "Takeover: InvokeStartInitPlayer result=%d [deferred]",
                startInitOk ? 1 : 0);

            // Apply DLL ExitProcess call-site patches for ALL session modes.
            // The Revival DLL's tick function calls ExitProcess(0) when the
            // game mode returns to 0 (title screen).  Without these patches
            // the main thread would be suspended by the IAT safety fallback.
            (void)SaveAndApplyDllExitProcessPatches();

            StabilizeOnlineSessionBindingAfterInit(initParams[0]);
        }
    }

    if (g_localInitAppliedForSession)
    {
        RepairRollbackHistoryBindingsIfNeeded();
    }

    if (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
    {
        LONG delayPromptSerial = 0;
        LONG delayPromptServedSerial = 0;
        ReadDelayPromptSignal(&delayPromptSerial, &delayPromptServedSerial);
        const LONG observedSerial = InterlockedCompareExchange(&g_injectedConnectedFromDelayPromptSerial, 0, 0);
        if (delayPromptSerial > 0 && delayPromptSerial != observedSerial)
        {
            InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, delayPromptSerial);
            mod::Log(
                "Takeover: delay prompt observed promptSerial=%ld servedSerial=%ld (waiting for init handshake/runtime sync before connected)",
                static_cast<long>(delayPromptSerial),
                static_cast<long>(delayPromptServedSerial));
        }
    }

    RefreshRuntimeStatus(ioStatus);
    NetbridgePhase currentPhase = static_cast<NetbridgePhase>(ioStatus->phase);
    RuntimeReadyProbe runtimeProbe = {};
    if (currentPhase == NetbridgePhase::Connecting || currentPhase == NetbridgePhase::DelaySetup)
    {
        runtimeProbe = EvaluateRuntimeReadyProbe(ioStatus);
        const uint32_t probeMask = BuildRuntimeReadyProbeMask(runtimeProbe);
        const DWORD now = GetTickCount();
        const bool changed = !g_lastRuntimeReadyProbeMaskValid || g_lastRuntimeReadyProbeMask != probeMask;
        if (changed || g_lastRuntimeReadyProbeLogTick == 0 || now - g_lastRuntimeReadyProbeLogTick >= 3000)
        {
            g_lastRuntimeReadyProbeMask = probeMask;
            g_lastRuntimeReadyProbeMaskValid = true;
            g_lastRuntimeReadyProbeLogTick = now;
            mod::Log(
                "Takeover: runtime-ready probe ready=%d source=%s localInit=%d prompt=%d input=%d session=%d bind(pid=%d handle=%d) syncReady=%d role=%d",
                runtimeProbe.ready ? 1 : 0,
                runtimeProbe.source != nullptr ? runtimeProbe.source : "none",
                runtimeProbe.localInitApplied ? 1 : 0,
                runtimeProbe.delayPromptSeen ? 1 : 0,
                runtimeProbe.delayInputApplied ? 1 : 0,
                runtimeProbe.sessionPointerValid ? 1 : 0,
                runtimeProbe.helperPidMatches ? 1 : 0,
                runtimeProbe.helperHandleMatches ? 1 : 0,
                runtimeProbe.nativeSyncReady ? 1 : 0,
                ioStatus->roleFlag);
        }
    }

    if ((currentPhase == NetbridgePhase::Connecting || currentPhase == NetbridgePhase::DelaySetup)
        && g_localInitAppliedForSession
        && ioStatus->delaySetupReady != 0)
    {
        if (currentPhase != NetbridgePhase::DelaySetup)
        {
            SetPhase(ioStatus, NetbridgePhase::DelaySetup, nullptr);
            currentPhase = NetbridgePhase::DelaySetup;
            mod::Log(
                "Takeover: promoted to delay setup prompt=%d/%d ping=%d delay=%d",
                ioStatus->delayPromptSerial,
                ioStatus->delayPromptServedSerial,
                ioStatus->pingMs,
                ioStatus->rollbackFrames);
        }
    }

    if ((currentPhase == NetbridgePhase::Connecting || currentPhase == NetbridgePhase::DelaySetup)
        && g_localInitAppliedForSession
        && runtimeProbe.ready)
    {
        SetPhase(ioStatus, NetbridgePhase::Connected, nullptr);
        if (ioConnectStartTick != nullptr)
        {
            *ioConnectStartTick = GetTickCount();
        }
        mod::Log(
            "Takeover: promoted to connected after runtime ready source=%s sync=%d/%d/%d ping=%d delay=%d",
            runtimeProbe.source != nullptr ? runtimeProbe.source : "none",
            ioStatus->syncGameMode,
            ioStatus->syncMode0Flag1084,
            ioStatus->syncSessionByte,
            ioStatus->pingMs,
            ioStatus->rollbackFrames);
    }
}

// ---------------------------------------------------------------------------
// CancelSessionUnlocked — shared body for CancelSession and exit interception.
// Caller MUST hold g_mutex.
// ---------------------------------------------------------------------------
static void CancelSessionUnlocked(const char* reason, NetbridgeStatus* ioStatus)
{
    InterlockedExchange(&g_startAbortRequested, 1);
    FlushPendingConsoleOutput("cancel");

    const bool hadProcess = ProcessAlive(ioStatus);
    if (hadProcess && g_revivalProcess != nullptr)
    {
        TerminateProcess(g_revivalProcess, 0);
    }
    CloseProcessHandle(ioStatus);
    RestoreDllExitProcessPatches();
    ReinitLocalPlay();
    g_localInitAppliedForSession = false;
    InterlockedExchange(&g_injectedDelayPromptSerial, 0);
    InterlockedExchange(&g_injectedDelayPromptServedSerial, 0);
    InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, 0);
    g_injectedDelayPromptWaitStartTick = 0;
    InterlockedExchange(&g_injectedSpectateConfirmPromptSerial, 0);
    InterlockedExchange(&g_injectedSpectateConfirmPromptServedSerial, 0);
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    {
        std::lock_guard<std::mutex> consoleLock(g_consoleLogMutex);
        g_consolePendingWriteFile.clear();
        g_consolePendingWriteFileDisk.clear();
        g_consolePendingWriteConsoleA.clear();
        g_consolePendingWriteConsoleW.clear();
        g_consolePendingWriteConsoleOutputCharacterA.clear();
        g_consolePendingWriteConsoleOutputCharacterW.clear();
        g_consolePendingOutputDebugStringA.clear();
        g_consolePendingOutputDebugStringW.clear();
        g_diskCapturePathHits.clear();
    }

    if (ioStatus != nullptr)
    {
        if (reason != nullptr && (std::strcmp(reason, "user_cancel") == 0 || std::strcmp(reason, "leave_menu") == 0 || std::strcmp(reason, "external_cancel") == 0))
        {
            SetPhase(ioStatus, NetbridgePhase::Idle, nullptr);
        }
        else if (hadProcess)
        {
            SetPhase(ioStatus, NetbridgePhase::SessionEnded, nullptr);
        }
        else
        {
            SetPhase(ioStatus, NetbridgePhase::Idle, nullptr);
        }
        RefreshRuntimeStatus(ioStatus);
    }

    mod::Log("Takeover: cancel reason='%s' hadProcess=%d", (reason != nullptr) ? reason : "", hadProcess ? 1 : 0);
}

void CancelSession(const char* reason, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    CancelSessionUnlocked(reason, ioStatus);
}

bool ConsumeRevivalExitInterception(int* outMode, NetbridgeStatus* ioStatus)
{
    // Fast unlocked pre-check avoids the mutex on every frame.
    if (InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0) == 0)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    // Double-check under lock and atomically clear the flag.
    if (InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 1) != 1)
    {
        return false;
    }

    const int mode = static_cast<int>(InterlockedExchange(&g_revivalExitMode, -1));
    if (outMode != nullptr)
    {
        *outMode = mode;
    }

    CancelSessionUnlocked("exit_intercepted", ioStatus);

    // Revert any EXE patches applied by the tournament constructor (inline
    // hooks at 0x763F04/0x763E50, byte clear at 0x754C1A, NOPs at 0x7599ED).
    // These must be restored before the title screen code runs again.
    // DLL ExitProcess patches are already restored by CancelSessionUnlocked.
    if (mode == kLocalRoleTournament)
    {
        RestoreTournamentExePatches();
    }

    // Create a fresh local play session to replace the old (neutralised)
    // session object.  Without this, the tournament session's data
    // (nicknames, win counts) bleeds into the title screen and the dummy
    // vtable prevents per-frame dispatch from functioning correctly.
    //
    // ForceLocalPlayInit calls init(2,102) AND immediately invokes
    // vtable[1] on the new session so that all fields (particularly the
    // BGM audio pointer at offset 668) are initialised before any other
    // EXE hooks dispatch to it in the same frame.
    ForceLocalPlayInit();

    // Clear stale tournament text overlays (nicknames, win counts).
    //
    // Text entries live in the EfzRender object in EFZ.exe memory and
    // persist across session transitions.  init(2,102) internally calls
    // the global reset function (sub_1006CC30) which zeroes the Revival
    // DLL's EfzRender* global (dword_100A0778), making the wrapper
    // functions unable to reach the text buffer.  We restore the saved
    // pointer and issue a clearTextRender call AFTER ForceLocalPlayInit
    // so the clear takes effect on the very next rendered frame.
    //
    // NeutralizeExitProcess also calls ClearRevivalText, but that clear
    // can be undone by init(2,102)'s reset.  This second call ensures
    // the text is definitively cleared.
    if (mode == kLocalRoleTournament)
    {
        ClearRevivalText();
        DisableRevivalTextRendering();
    }

    mod::Log("Takeover: consumed exit interception mode=%d", mode);
    return true;
}

DelayPromptMetrics GetDelayPromptMetrics()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    DelayPromptMetrics metrics = g_delayPromptMetrics;
    if (g_hostBlock != nullptr)
    {
        const LONG metricsSerial = InterlockedCompareExchange(&g_hostBlock->delayMetricsSerial, 0, 0);
        if (metricsSerial > 0)
        {
            metrics.serial = static_cast<int>(metricsSerial);
            metrics.averagePingMs = g_hostBlock->delayAveragePingMs;
            metrics.minPingMs = g_hostBlock->delayMinPingMs;
            metrics.maxPingMs = g_hostBlock->delayMaxPingMs;
            metrics.recommendedDelay = g_hostBlock->delayRecommended;
            metrics.minDelay = g_hostBlock->delayRangeMin;
            metrics.maxDelay = g_hostBlock->delayRangeMax;
        }
        const LONG inputSerial = InterlockedCompareExchange(&g_hostBlock->delayInputSerial, 0, 0);
        metrics.inputSerial = static_cast<int>(inputSerial);
        metrics.inputValue = g_hostBlock->delayInputValue;
    }
    return metrics;
}

} // namespace netplay::bridge::takeover


















