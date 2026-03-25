// Session lifecycle: host/join/spectate management, tick loop, cancel, delay.
// Internal helpers live in sister .cpp files; see takeover_internal.h.

#include "netplay/bridge/revival_takeover.h"
#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/options_menu.h"

#include "crash_handler.h"
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
int g_netplayRole = kNetplayRoleNone;
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
DWORD g_lastSpectateConsoleSnapshotTick = 0;
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

// Job object for automatic child-process cleanup.  When the host process
// terminates (even by crash), the kernel closes all handles to the job,
// which kills every process assigned to it — ensuring EfzRevival.exe and
// any grandchildren (cmd.exe / conhost.exe) never linger in the background.
static HANDLE g_childJobObject = nullptr;

namespace
{
std::string TrimAsciiCopy(const std::string& text)
{
    size_t start = 0;
    while (start < text.size()
        && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n'))
    {
        ++start;
    }
    size_t end = text.size();
    while (end > start
        && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }
    return text.substr(start, end - start);
}

void AppendConsolePreview(std::string* out, const char* tag, const std::string& text)
{
    if (out == nullptr || tag == nullptr)
    {
        return;
    }

    const std::string trimmed = TrimAsciiCopy(text);
    if (trimmed.empty())
    {
        return;
    }

    if (!out->empty())
    {
        out->append(" | ");
    }
    out->append(tag);
    out->push_back('=');
    out->push_back('\'');
    constexpr size_t kMaxPreviewLen = 80;
    if (trimmed.size() > kMaxPreviewLen)
    {
        out->append(trimmed.substr(0, kMaxPreviewLen));
        out->append("...");
    }
    else
    {
        out->append(trimmed);
    }
    out->push_back('\'');
}

void LogPendingSpectateConsoleSnapshot()
{
    std::string summary;
    {
        std::lock_guard<std::mutex> lock(g_consoleLogMutex);
        AppendConsolePreview(&summary, "WriteFile", g_consolePendingWriteFile);
        AppendConsolePreview(&summary, "WriteFileDisk", g_consolePendingWriteFileDisk);
        AppendConsolePreview(&summary, "WriteConsoleA", g_consolePendingWriteConsoleA);
        AppendConsolePreview(&summary, "WriteConsoleW", g_consolePendingWriteConsoleW);
        AppendConsolePreview(&summary, "WriteConsoleOutputCharacterA", g_consolePendingWriteConsoleOutputCharacterA);
        AppendConsolePreview(&summary, "WriteConsoleOutputCharacterW", g_consolePendingWriteConsoleOutputCharacterW);
        AppendConsolePreview(&summary, "OutputDebugStringA", g_consolePendingOutputDebugStringA);
        AppendConsolePreview(&summary, "OutputDebugStringW", g_consolePendingOutputDebugStringW);
    }

    if (!summary.empty())
    {
        mod::Log("Takeover: spectate console snapshot %s", summary.c_str());
    }
}
} // namespace

// ---------------------------------------------------------------------------
// Version detection
// ---------------------------------------------------------------------------

/// Detect the loaded Revival DLL version by reading its PE TimeDateStamp
/// and matching against known profiles.  Sets g_activeRevival to the
/// matching profile (or leaves it at the default 1.02e if detection fails).
/// Call this once early — before any profile-dependent code runs.
void DetectRevivalVersion()
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("DetectRevivalVersion: EfzRevival.dll not loaded — keeping "
                 "default profile %s", g_activeRevival->versionTag);
        return;
    }

    // Read PE TimeDateStamp from the loaded image header.
    const auto* base = reinterpret_cast<const uint8_t*>(revival);
    const auto dosE_lfanew = *reinterpret_cast<const int32_t*>(base + 0x3C);
    if (dosE_lfanew < 0 || dosE_lfanew > 0x1000)
    {
        mod::Log("DetectRevivalVersion: invalid e_lfanew=0x%X — keeping default",
                 static_cast<unsigned>(dosE_lfanew));
        return;
    }

    const auto* peSignature = reinterpret_cast<const uint32_t*>(base + dosE_lfanew);
    if (*peSignature != 0x00004550u)  // "PE\0\0"
    {
        mod::Log("DetectRevivalVersion: bad PE signature 0x%08X — keeping default",
                 static_cast<unsigned>(*peSignature));
        return;
    }

    // COFF header TimeDateStamp is at PE+8.
    const uint32_t timestamp = *reinterpret_cast<const uint32_t*>(base + dosE_lfanew + 8);

    // Search known profiles.
    for (size_t i = 0; i < kRevivalProfileCount; ++i)
    {
        if (kAllRevivalProfiles[i]->peTimestamp == timestamp)
        {
            g_activeRevival = kAllRevivalProfiles[i];
            mod::Log("DetectRevivalVersion: matched timestamp 0x%08X → %s",
                     static_cast<unsigned>(timestamp),
                     g_activeRevival->versionTag);
            return;
        }
    }

    // Unknown timestamp — keep default and warn.
    mod::Log("DetectRevivalVersion: UNKNOWN timestamp 0x%08X — keeping default %s. "
             "Addresses may be wrong!",
             static_cast<unsigned>(timestamp),
             g_activeRevival->versionTag);
}

// ---------------------------------------------------------------------------
// Job object helpers
// ---------------------------------------------------------------------------

static HANDLE EnsureChildJobObject()
{
    if (g_childJobObject != nullptr)
        return g_childJobObject;

    g_childJobObject = CreateJobObjectA(nullptr, nullptr);
    if (g_childJobObject == nullptr)
    {
        mod::Log("Takeover: CreateJobObject failed err=%lu",
                 static_cast<unsigned long>(GetLastError()));
        return nullptr;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(g_childJobObject,
                                 JobObjectExtendedLimitInformation,
                                 &jeli, sizeof(jeli)))
    {
        mod::Log("Takeover: SetInformationJobObject failed err=%lu",
                 static_cast<unsigned long>(GetLastError()));
        CloseHandle(g_childJobObject);
        g_childJobObject = nullptr;
        return nullptr;
    }

    mod::Log("Takeover: created kill-on-close job object handle=%p",
             g_childJobObject);
    return g_childJobObject;
}

static void CloseChildJobObject()
{
    if (g_childJobObject != nullptr)
    {
        // Closing the handle triggers JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
        // terminating any surviving child processes.
        mod::Log("Takeover: closing child job object handle=%p",
                 g_childJobObject);
        CloseHandle(g_childJobObject);
        g_childJobObject = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Session lifecycle functions (public API from revival_takeover.h).
// ---------------------------------------------------------------------------

void InitializeHost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    PrimeManagedLogEfzHistory();
    (void)EnsureHostIpc();
    if (!EnsureLocalRevivalLoaded())
    {
        mod::Log("Takeover: host local revival load failed");
    }
    else
    {
        // DetectRevivalVersion() is now called inside EnsureLocalRevivalLoaded()
        // before any profile-dependent operations (frame hook, etc.).
        (void)SetLocalRoleFlag(kLocalRoleLocalPlay, "host_initialize");
    }

    {
        const auto selfPatches = BuildPatchMap(reinterpret_cast<uintptr_t>(SelfModule()));
        std::unordered_map<std::string, uint32_t> hostLogPatches;
        const auto writeFileIt = selfPatches.find("WriteFile");
        if (writeFileIt != selfPatches.end())
        {
            hostLogPatches.emplace(writeFileIt->first, writeFileIt->second);
            const bool patchedHostLogIat =
                PatchIat(GetCurrentProcess(), GetCurrentProcessId(), hostLogPatches, true);
            mod::Log(
                "Takeover: host logEfz WriteFile IAT patch result=%d",
                patchedHostLogIat ? 1 : 0);
        }
        else
        {
            mod::Log("Takeover: host logEfz WriteFile IAT patch unavailable (missing stub)");
        }
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
    CloseChildJobObject();
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
    CloseChildJobObject();
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
        if (netplay::options::UseTournamentModeForOfflineVsHuman())
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
            (void)SetLocalRoleFlag(kLocalRoleTournament, "title_vs_human_tournament");
            (void)NeutralizeTournamentAutoNav();
        }
        else
        {
            // Regular VS Human: keep Revival in its local-play role and let
            // the native EFZ title flow proceed without tournament-only hooks.
            (void)SetRoleFlagDirect(kLocalRoleLocalPlay, "title_vs_human_local");
            mod::Log("Takeover: offline VS Human configured for regular local VS");
        }
    }
    else
    {
        (void)SetRoleFlagDirect(kLocalRoleLocalPlay, "title_other");
    }
    RefreshRuntimeStatus(ioStatus);
}

bool StartSession(
    NetbridgeRole role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni,
    NetbridgeStatus* ioStatus,
    uint32_t* outConnectStartTick)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    mod::Log(
        "Takeover: StartSession role=%d port=%u address='%s' nickname='%s' writeNicknameToIni=%d",
        static_cast<int>(role),
        static_cast<unsigned>(port),
        (address != nullptr) ? address : "",
        (nickname != nullptr) ? nickname : "",
        writeNicknameToIni ? 1 : 0);

    // --- Session-start diagnostic dump (2nd-session crash investigation) ---
    ResetForceLocalPlayInitCount();
    ResetGameModeValidation();

    // --- Session boundary cleanup logging ---
    // Log EXE hook bytes at both hook sites for cross-session tracking.
    {
        uint8_t hookA[10] = {};
        uint8_t hookB[8] = {};
        memcpy(hookA, reinterpret_cast<const void*>(0x401582), 10);
        memcpy(hookB, reinterpret_cast<const void*>(0x401642), 8);
        mod::Log(
            "StartSession: EXE hooks pre-session "
            "0x401582=[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X] "
            "0x401642=[%02X %02X %02X %02X %02X %02X %02X %02X]",
            hookA[0], hookA[1], hookA[2], hookA[3], hookA[4],
            hookA[5], hookA[6], hookA[7], hookA[8], hookA[9],
            hookB[0], hookB[1], hookB[2], hookB[3], hookB[4],
            hookB[5], hookB[6], hookB[7]);
    }

    // Clear stale exit-interception flags from a previous session.
    // If g_revivalExitIntercepted leaked from session 1 (e.g. ExitProcess
    // raced with CancelSession), ConsumeRevivalExitInterception would fire
    // during session 2's setup — running the full reverse-init cleanup and
    // destroying the new session.
    if (InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0) != 0)
    {
        mod::Log("StartSession: clearing stale g_revivalExitIntercepted=%ld g_revivalExitMode=%ld",
                 static_cast<long>(g_revivalExitIntercepted),
                 static_cast<long>(g_revivalExitMode));
        InterlockedExchange(&g_revivalExitIntercepted, 0);
        InterlockedExchange(&g_revivalExitMode, -1);
    }

    LogSessionDiagnosticState("StartSession_entry");

    InterlockedExchange(&g_startAbortRequested, 0);

    if (!EnsureHostIpc())
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "IPC setup failed");
        return false;
    }

    // Zero the entire shared block to prevent stale data from session 1
    // leaking into session 2.  Re-populate the header fields that
    // EnsureHostIpc wrote (magic, version, hostPid, hostRevivalBase)
    // since StartSession's later code repopulates the rest.
    if (g_hostBlock != nullptr)
    {
        mod::Log("StartSession: resetting SharedBlock (initSerial=%ld consoleSerial=%ld)",
                 static_cast<long>(g_hostBlock->initSerial),
                 static_cast<long>(g_hostBlock->consoleSerial));
        const uint32_t savedMagic = g_hostBlock->magic;
        const uint32_t savedVersion = g_hostBlock->version;
        const uint32_t savedPid = g_hostBlock->hostPid;
        const uint32_t savedBase = g_hostBlock->hostRevivalBase;
        memset(g_hostBlock, 0, sizeof(SharedBlock));
        g_hostBlock->magic = savedMagic;
        g_hostBlock->version = savedVersion;
        g_hostBlock->hostPid = savedPid;
        g_hostBlock->hostRevivalBase = savedBase;
        // Restore default values for delay prompt fields.
        g_hostBlock->delayInputValue = -1;
        g_hostBlock->delayAveragePingMs = -1;
        g_hostBlock->delayMinPingMs = -1;
        g_hostBlock->delayMaxPingMs = -1;
        g_hostBlock->delayRecommended = -1;
        g_hostBlock->delayRangeMax = 20;
    }

    if (!EnsureLocalRevivalLoaded())
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "EfzRevival.dll unavailable");
        return false;
    }

    // Save the EfzRender* pointer now so that ClearRevivalText /
    // DisableRevivalTextRendering can restore it during CancelSession.
    // Tournament mode already does this in OnTitleSelectionConfirmed,
    // but online sessions (host/join/spectate) skipped it — causing
    // both clear and disable to silently fail on the cleanup path.
    (void)SaveRenderContext();

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
    if (!WriteIni(gameDir, static_cast<int>(role), port, address, nickname, writeNicknameToIni))
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

    // ---- Wine/Proton path --------------------------------------------------
    // Both Wine and native paths use CREATE_SUSPENDED so that the child's
    // main thread is frozen before its entry point.  We inject our DLL via
    // CreateRemoteThread(LoadLibraryA), patch the IAT, then ResumeThread.
    // This ensures our ReadConsoleA hook is in place before main() runs.
    //
    // Under Wine we also set WINEDLLOVERRIDES as a belt-and-suspenders
    // measure (it does not force-load custom DLLs on current Wine, but
    // may help on future versions).
    //
    // SelfPatchIat() in DllMain(DLL_PROCESS_ATTACH) provides an additional
    // safety net under Wine, patching the EXE's IAT from within the
    // process before the loader lock is released.
    // --------------------------------------------------------------------
    const bool useWinePath = IsRunningUnderWine();

    if (useWinePath)
    {
        // Build an environment block that includes WINEDLLOVERRIDES so Wine
        // force-loads our DLL into the child process.
        //
        // Get our DLL's full path.  Wine's WINEDLLOVERRIDES expects the
        // module name (without extension), e.g. "efz_netplay_mod=n".
        // Using just the basename is the documented format for Wine.
        // We also set the DLL's directory on the search path so Wine can
        // find the native DLL file when the override triggers.
        const std::string selfPath = ModulePath(SelfModule());
        if (selfPath.empty())
        {
            SetPhase(ioStatus, NetbridgePhase::Failed, "[Wine] failed to get self module path");
            return false;
        }

        // Build the override string: "efz_netplay_mod=n"
        // n = native — tells Wine to load the DLL from the filesystem.
        std::string baseName = BaseLower(selfPath);
        {
            // Strip .dll extension if present.
            const size_t dot = baseName.rfind('.');
            if (dot != std::string::npos)
                baseName.erase(dot);
        }
        std::string overrideValue = baseName + "=n";

        // Merge with any existing WINEDLLOVERRIDES.
        char existingOverride[4096] = {};
        const DWORD existingLen = GetEnvironmentVariableA(
            "WINEDLLOVERRIDES", existingOverride, sizeof(existingOverride));
        if (existingLen > 0 && existingLen < sizeof(existingOverride))
        {
            overrideValue = std::string(existingOverride) + ";" + overrideValue;
        }

        // Build an environment block — a double-null-terminated sequence of
        // "KEY=VALUE\0" strings.  We inherit the current environment and
        // append/override WINEDLLOVERRIDES.
        std::vector<char> envBlock;
        {
            // Get the current environment.
            char* currentEnv = GetEnvironmentStringsA();
            if (currentEnv != nullptr)
            {
                // Walk the double-null-terminated block.
                const char* p = currentEnv;
                bool overrideWritten = false;
                while (*p != '\0')
                {
                    const size_t entryLen = std::strlen(p);
                    // Check if this is the WINEDLLOVERRIDES entry.
                    if (_strnicmp(p, "WINEDLLOVERRIDES=", 17) == 0)
                    {
                        // Replace with our merged value.
                        std::string merged = "WINEDLLOVERRIDES=" + overrideValue;
                        envBlock.insert(envBlock.end(), merged.begin(), merged.end());
                        envBlock.push_back('\0');
                        overrideWritten = true;
                    }
                    else
                    {
                        envBlock.insert(envBlock.end(), p, p + entryLen + 1);
                    }
                    p += entryLen + 1;
                }
                if (!overrideWritten)
                {
                    std::string entry = "WINEDLLOVERRIDES=" + overrideValue;
                    envBlock.insert(envBlock.end(), entry.begin(), entry.end());
                    envBlock.push_back('\0');
                }
                envBlock.push_back('\0'); // double-null terminator
                FreeEnvironmentStringsA(currentEnv);
            }
        }

        BOOL created = CreateProcessA(
            exePath.c_str(),
            nullptr,
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW,
            envBlock.empty() ? nullptr : envBlock.data(),
            gameDir.empty() ? nullptr : gameDir.c_str(),
            &si,
            &pi);

        if (!created)
        {
            mod::Log("Takeover [Wine]: CreateProcess failed err=%lu",
                     static_cast<unsigned long>(GetLastError()));
            SetPhase(ioStatus, NetbridgePhase::Failed, "[Wine] CreateProcess(EfzRevival.exe) failed");
            return false;
        }

        mod::Log("Takeover [Wine]: spawned EfzRevival suspended pid=%lu (DLL override active)",
                 static_cast<unsigned long>(pi.dwProcessId));
    }
    else
    {
        // ---- Native Windows path -------------------------------------------
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
    }

    // Assign to kill-on-close job so that if the host process terminates
    // (crash, Alt+F4, etc.) without explicit cleanup, all child processes
    // (EfzRevival.exe, cmd.exe, conhost.exe, ...) are killed automatically.
    HANDLE job = EnsureChildJobObject();
    if (job != nullptr)
    {
        if (!AssignProcessToJobObject(job, pi.hProcess))
        {
            mod::Log("Takeover: AssignProcessToJobObject failed pid=%lu err=%lu",
                     static_cast<unsigned long>(pi.dwProcessId),
                     static_cast<unsigned long>(GetLastError()));
        }
        else
        {
            mod::Log("Takeover: assigned pid=%lu to kill-on-close job",
                     static_cast<unsigned long>(pi.dwProcessId));
        }
    }

    uintptr_t remoteBase = 0;

    // Inject our DLL into the child process.
    // - Under Wine: InjectSelf() first checks if WINEDLLOVERRIDES loaded the
    //   DLL, then falls back to CreateRemoteThread(LoadLibraryA).
    // - Native Windows: InjectSelf() uses CreateRemoteThread(LoadLibraryA).
    if (!InjectSelf(pi.hProcess, &remoteBase))
    {
        const char* reason = useWinePath
            ? "[Wine] DLL injection failed"
            : "Self injection failed";
        SetPhase(ioStatus, NetbridgePhase::Failed, reason);
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
    g_lastSpectateConsoleSnapshotTick = 0;
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
    CloseMirrorLogFiles();
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
        // Join (choice 3). Prompt handling is done from the host-side UI based
        // on the detected prompt kind:
        // - "Host already playing, join as a spectator?" -> auto-answer Yes (1)
        // - "Host not yet playing, join as a player?"   -> show Join/Wait/Cancel
        //   in the in-game overlay and let the host-side UI decide whether to
        //   keep waiting, restart as a normal Join, or cancel cleanly.
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

    // --- Pre-launch ring buffer flush (spectate only) ----------------------
    // Revival's named shared-memory ring buffers ("InputP1", "InputP2", etc.)
    // may still contain stale data from a previous session if the kernel
    // objects haven't been destroyed.  Flush them NOW — before the child
    // process is resumed — so that only fresh data from the new session is
    // present when the DLL eventually starts consuming.
    //
    // Previously this flush lived inside the Tick() init-handshake block
    // (after the child signalled its init event).  That placement caused a
    // spectator desync: between the child's WriteProcessMemory IAT hook
    // signalling the init event and the next game-frame's Tick() detecting
    // it (~16 ms), the child had already started writing the host's
    // historical input stream into the ring buffers.  The flush then
    // discarded those early entries — the very beginning of the charselect
    // replay — leaving the DLL to start mid-stream against a freshly-
    // initialised charselect state.
    //
    // By flushing here (child still suspended), we clear only genuinely
    // stale data; once the child resumes and connects, every input frame
    // it writes is preserved for the DLL to consume.
    //
    // Extended to ALL roles (host/join/spectate): the named shared memory
    // ring buffers persist as kernel objects as long as any process holds a
    // handle.  The DLL in the host process survives across sessions, so
    // stale ring buffer mappings can contaminate session 2 for any role.
    // -------------------------------------------------------------------
    {
        struct RingMapping {
            const char* name;
            HANDLE      hMap;
            volatile DWORD* view;
        };
        RingMapping mappings[] = {
            {"InputP1",   nullptr, nullptr},
            {"InputP2",   nullptr, nullptr},
            {"PaletteP1", nullptr, nullptr},
            {"PaletteP2", nullptr, nullptr},
            {"Sync",      nullptr, nullptr},
            {"Quit",      nullptr, nullptr},
            {"LoadMatch", nullptr, nullptr},
            {"Init",      nullptr, nullptr},
            {"Net",       nullptr, nullptr},
        };
        constexpr int kMappingCount = 9;

        for (int mi = 0; mi < kMappingCount; ++mi)
        {
            mappings[mi].hMap = OpenFileMappingA(
                FILE_MAP_ALL_ACCESS, FALSE, mappings[mi].name);
            if (mappings[mi].hMap != nullptr)
            {
                mappings[mi].view = static_cast<volatile DWORD*>(
                    MapViewOfFile(mappings[mi].hMap,
                                 FILE_MAP_ALL_ACCESS, 0, 0, 8));
            }
        }
        for (int mi = 0; mi < kMappingCount; ++mi)
        {
            if (mappings[mi].view == nullptr)
                continue;
            const DWORD oldHead = mappings[mi].view[0];
            const DWORD oldTail = mappings[mi].view[1];
            if (oldHead != oldTail)
            {
                mappings[mi].view[0] = oldTail;
                mod::Log(
                    "Takeover: pre-launch flushed stale '%s' "
                    "head=%lu->%lu tail=%lu",
                    mappings[mi].name,
                    static_cast<unsigned long>(oldHead),
                    static_cast<unsigned long>(oldTail),
                    static_cast<unsigned long>(oldTail));
            }
        }
        // Alignment verification is unnecessary here — no producer is
        // running yet, so no interleaved writes can occur.
        for (int mi = 0; mi < kMappingCount; ++mi)
        {
            if (mappings[mi].view != nullptr)
                UnmapViewOfFile(const_cast<DWORD*>(mappings[mi].view));
            if (mappings[mi].hMap != nullptr)
                CloseHandle(mappings[mi].hMap);
        }
    }

    // Both Wine and native: the process was created suspended, resume it
    // now that injection and IAT patching are complete.
    {
        const DWORD resumeResult = ResumeThread(pi.hThread);
        mod::Log("Takeover: resumed main thread result=%lu%s",
                 static_cast<unsigned long>(resumeResult),
                 useWinePath ? " [Wine]" : "");
    }

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

    // Input delay is only applicable to online sessions (mode 0).
    // Spectator sessions have a different layout and the delay offset
    // would write into unrelated spectator fields.
    // During the connecting phase g_localRoleFlag is still kLocalRoleLocalPlay
    // because the init handshake hasn't completed yet.  Check the *intended*
    // role stored in the shared block as well so the delay prompt works.
    const bool currentRoleOnline = (g_localRoleFlag == kLocalRoleOnline);
    const bool intendedRoleOnline = (g_hostBlock != nullptr
        && g_hostBlock->initParams[0] == kLocalRoleOnline);
    if (!currentRoleOnline && !intendedRoleOnline)
    {
        mod::Log("Takeover: ApplyInputDelay rejected (role=%d, intended=%d, online-only)",
                 g_localRoleFlag,
                 g_hostBlock ? g_hostBlock->initParams[0] : -1);
        return false;
    }

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

    // Also write the saved target prediction window (inputDelay + 8, i.e.
    // +696 in 1.02e).  Between matches, BuildMatchInfoAndHUD restores
    // the active inputDelay (+688) FROM the target window (+696).  If we
    // only update +688 here, the target stays at 0 and all subsequent
    // matches start with inputDelay=0, causing 2-3 iterations per frame
    // (the ~1.5x speed-up bug).
    void* const targetWindowAddress = reinterpret_cast<void*>(
        sessionPtr + g_activeRevival->sessionOffsetInputDelay + 8);
    if (IsWritableRange(targetWindowAddress, sizeof(int)))
    {
        *reinterpret_cast<int*>(targetWindowAddress) = delayFrames;
    }

    mod::Log(
        "Takeover: ApplyInputDelay applied value=%d session=0x%08lX (+696=%d)",
        delayFrames,
        static_cast<unsigned long>(sessionPtr),
        delayFrames);
    RefreshRuntimeStatus(ioStatus);
    if (ioStatus != nullptr)
    {
        ioStatus->rollbackFrames = delayFrames;
    }
    return true;
}

bool AnswerSpectatePromptChoice(int choice, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    LONG promptSerial = 0;
    LONG promptServedSerial = 0;
    int promptKind = static_cast<int>(NetbridgeSpectatePromptKind::None);
    ReadSpectateConfirmPromptSignal(&promptSerial, &promptServedSerial, &promptKind);
    const bool promptPending = promptSerial > 0 && promptServedSerial < promptSerial;

    if (!promptPending)
    {
        mod::Log(
            "Takeover: AnswerSpectatePromptChoice rejected (no pending prompt serial=%ld served=%ld)",
            static_cast<long>(promptSerial),
            static_cast<long>(promptServedSerial));
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    if (g_hostBlock == nullptr)
    {
        mod::Log("Takeover: AnswerSpectatePromptChoice rejected (no shared block)");
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    int maxChoice = 2;
    if (promptKind == static_cast<int>(NetbridgeSpectatePromptKind::HostNotYetPlaying))
    {
        maxChoice = 3;
    }
    if (choice < 1 || choice > maxChoice)
    {
        mod::Log(
            "Takeover: AnswerSpectatePromptChoice rejected (invalid choice=%d kind=%d maxChoice=%d)",
            choice,
            promptKind,
            maxChoice);
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    g_hostBlock->spectateConfirmInputValue = choice;
    const LONG inputSerial = InterlockedIncrement(&g_hostBlock->spectateConfirmInputSerial);
    if (g_hostConsoleEvent != nullptr)
    {
        SetEvent(g_hostConsoleEvent);
    }
    mod::Log(
        "Takeover: AnswerSpectatePromptChoice queued choice=%d kind=%d promptSerial=%ld inputSerial=%ld",
        choice,
        promptKind,
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
            // Connected phase: peer process has exited.  Regardless of
            // whether the rollback runtime still reports ready, terminate
            // the session.  Keeping it alive leads to a stuck state where
            // no further progress is possible.
            SetPhase(ioStatus, NetbridgePhase::SessionEnded,
                     runtimeReady ? "peer disconnected" : nullptr);
            ReinitLocalPlay();
            if (runtimeReady)
            {
                mod::Log("Takeover: helper process exited during Connected phase — forcing SessionEnded (runtime was still ready)");
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

        const bool spectateRoleActive =
            g_hostBlock != nullptr && g_hostBlock->initParams[0] == kLocalRoleSpectate;
        if (spectateRoleActive
            && (g_lastSpectateConsoleSnapshotTick == 0
                || now - g_lastSpectateConsoleSnapshotTick >= 4000))
        {
            g_lastSpectateConsoleSnapshotTick = now;
            LogPendingSpectateConsoleSnapshot();
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
            // --- Diagnostic dump before init handshake (2nd-session crash investigation) ---
            LogSessionDiagnosticState("Tick_init_handshake_pre");

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

            // --- Full init write snapshot BEFORE ---
            LogInitWriteSnapshot("Tick_init_pre");

            // Destroy the current session to prevent leaking the old object.
            // This is the root cause fix for the 2nd-session crash (H1).
            const uintptr_t oldSessionPtr = ReadSessionPointerFromRevival();
            DestroyCurrentSession("Tick_init_handshake");

            // Prevent init() from chaining another trampoline at 0x401582.
            mod::Log(
                "Tick_init_handshake: about to save EXE hook bytes before "
                "init() mode=%d oldSession=0x%08lX",
                initParams[0],
                static_cast<unsigned long>(oldSessionPtr));
            SaveExeFrameHookBytes();
            // Save the 8 bytes at 0x401642 (per-frame dispatch replacement
            // hook) so init()'s EFZ_BufferProcess_WithSize doesn't leak a
            // new malloc'd trampoline on every session.
            SaveExeDispatchHookBytes();
            // Restore original (pre-hook) bytes at mode-ctor hook sites
            // BEFORE init() so the new trampoline copies clean EXE bytes
            // instead of stale hooks from a previous session's mode.
            RestoreModeCtorOriginalBytes();
            ResetModeConstructorTrampolineCache();

            // Dump the 10 bytes at 0x401582 right before init().
            {
                uint8_t pre[10] = {};
                memcpy(pre, reinterpret_cast<const void*>(0x401582), 10);
                mod::Log(
                    "Tick_init_handshake: 0x401582 pre-init  "
                    "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
                    pre[0], pre[1], pre[2], pre[3], pre[4],
                    pre[5], pre[6], pre[7], pre[8], pre[9]);
            }

            mod::Log("Tick_init_handshake: calling init(mode=%d, magic=%d)",
                     initParams[0], initParams[1]);
            const int initResult = g_localInitFn(initParams);

            // Dump the 10 bytes AFTER init() to see what sub_1006F160 wrote.
            {
                uint8_t post[10] = {};
                memcpy(post, reinterpret_cast<const void*>(0x401582), 10);
                mod::Log(
                    "Tick_init_handshake: 0x401582 post-init "
                    "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
                    post[0], post[1], post[2], post[3], post[4],
                    post[5], post[6], post[7], post[8], post[9]);
            }

            // Undo the EXE frame-hook chain growth at 0x401582.
            // Mode-ctor originals were already restored before init();
            // for non-local modes init() installs fresh hooks that don't
            // chain through stale trampolines.
            mod::Log("Tick_init_handshake: restoring saved EXE hook bytes (mode=%d)", initParams[0]);
            RestoreExeFrameHookBytes();

            // Verify the restore worked.
            {
                uint8_t verify[10] = {};
                memcpy(verify, reinterpret_cast<const void*>(0x401582), 10);
                mod::Log(
                    "Tick_init_handshake: 0x401582 restored  "
                    "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
                    verify[0], verify[1], verify[2], verify[3], verify[4],
                    verify[5], verify[6], verify[7], verify[8], verify[9]);
            }

            // Restore saved 0x401642 bytes to undo init()'s new trampoline.
            RestoreExeDispatchHookBytes();

            // Fix up relative instructions in mode-constructor trampolines.
            FixupModeConstructorTrampolines("Tick_init_handshake");

            // --- Dump bytes at 0x401642 (double-speed investigation) --------
            // After init(), check that the REPLACEMENT hook at 0x401642 is
            // still intact (first byte should be 0xE9 = JMP near).  If the
            // hook was undone, the main loop's vtable[1] call AND Revival's
            // sub_1006D0B0 both run the screen update → doubled game speed.
            {
                uint8_t hb[16] = {};
                for (int i = 0; i < 16; ++i)
                {
                    if (!SafeReadByte(reinterpret_cast<const void*>(0x401642 + i), &hb[i]))
                        hb[i] = 0xCC;
                }
                mod::Log(
                    "Tick_init_handshake: 0x401642 post-init "
                    "[%02X %02X %02X %02X %02X %02X %02X %02X "
                    " %02X %02X %02X %02X %02X %02X %02X %02X]",
                    hb[0], hb[1], hb[2], hb[3], hb[4], hb[5], hb[6], hb[7],
                    hb[8], hb[9], hb[10], hb[11], hb[12], hb[13], hb[14], hb[15]);
            }

            g_localRoleFlag = initParams[0];
            g_localInitAppliedForSession = true;

            const uintptr_t newSessionPtr = ReadSessionPointerFromRevival();

            // --- Full init write snapshot AFTER init() ---
            LogInitWriteSnapshot("Tick_init_post");

            mod::Log(
                "Takeover: local init(mode=%d magic=%d) result=%d [deferred] oldSession=0x%08lX newSession=0x%08lX",
                initParams[0],
                initParams[1],
                initResult,
                static_cast<unsigned long>(oldSessionPtr),
                static_cast<unsigned long>(newSessionPtr));

            // Zero out the initComplete field on the new session BEFORE
            // calling InvokeStartInitPlayer.  When the heap reuses the same
            // address as a previous session, initComplete may still be 1
            // (stale), which would cause InvokeStartInitPlayer to skip the
            // critical sub_10072880 call — leaving the session in an
            // uninitialized state and freezing the game.
            //
            // Only meaningful for online sessions (mode 0) where
            // InvokeStartInitPlayer runs.  For spectator/local/tournament
            // sessions, the initComplete offset overlaps with different
            // fields in the smaller session object.
            if (initParams[0] == kLocalRoleOnline
                && newSessionPtr != 0 && newSessionPtr >= 0x00100000u)
            {
                int prevInitComplete = -1;
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(newSessionPtr + g_activeRevival->sessionOffsetInitComplete),
                    &prevInitComplete);
                if (prevInitComplete == 1)
                {
                    DWORD oldProt = 0;
                    void* initCompAddr = reinterpret_cast<void*>(
                        newSessionPtr + g_activeRevival->sessionOffsetInitComplete);
                    if (VirtualProtect(initCompAddr, sizeof(int), PAGE_READWRITE, &oldProt))
                    {
                        int zero = 0;
                        memcpy(initCompAddr, &zero, sizeof(int));
                        VirtualProtect(initCompAddr, sizeof(int), oldProt, &oldProt);
                        mod::Log(
                            "Takeover: cleared stale initComplete=1 on reused session 0x%08lX before StartInitPlayer",
                            static_cast<unsigned long>(newSessionPtr));
                    }
                    else
                    {
                        mod::Log(
                            "Takeover: FAILED to clear stale initComplete=1 on session 0x%08lX — "
                            "VirtualProtect err=%lu (StartInitPlayer will likely skip sub_10072880!)",
                            static_cast<unsigned long>(newSessionPtr),
                            static_cast<unsigned long>(GetLastError()));
                    }
                }
            }

            const bool startInitOk = InvokeStartInitPlayer(initParams[0]);

            // --- Detect netplay role (host / client / spectator) ---
            if (initParams[0] == kLocalRoleSpectate)
            {
                g_netplayRole = kNetplayRoleSpectator;
                mod::Log("Takeover: netplay role = Spectator");
            }
            else if (initParams[0] == kLocalRoleOnline && startInitOk)
            {
                // Read the activePlayer field that StartInitPlayer just wrote.
                // 0 = host (P1), 1 = joiner/client (P2 — inputs were swapped).
                const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
                int activePlayer = -1;
                if (sessionPtr != 0)
                {
                    (void)SafeReadInt(
                        reinterpret_cast<const void*>(
                            sessionPtr + g_activeRevival->sessionOffsetActivePlayer),
                        &activePlayer);
                }
                if (activePlayer == 1)
                {
                    g_netplayRole = kNetplayRoleClient;
                    mod::Log("Takeover: netplay role = Client (P2, inputs swapped)");
                }
                else
                {
                    g_netplayRole = kNetplayRoleHost;
                    mod::Log("Takeover: netplay role = Host (P1, activePlayer=%d)",
                             activePlayer);
                }
            }

            // --- Snapshot after StartInitPlayer (writes session fields) ---
            LogInitWriteSnapshot("Tick_startInitPlayer_post");

            mod::Log(
                "Takeover: InvokeStartInitPlayer result=%d [deferred]",
                startInitOk ? 1 : 0);

            // For spectator mode, init(1,102) creates the spectator object
            // but does NOT allocate its BGM manager (offset +1068).  The
            // full init (EFZ_Spectator_Init) only runs when the game hits
            // address 0x401582 → sub_1006E590 → vtable+4.  However, the
            // BGM dispatch hook at 0x40DE80 is already installed from a
            // previous session, and if the game triggers a BGM event before
            // 0x401582 fires, it dispatches to the uninitialized spectator
            // and crashes on the NULL BGM manager.  Calling vtable+4 here
            // ensures the spectator is fully initialized before any hook
            // can dispatch to it — identical to what ForceLocalPlayInit
            // does for mode 2.
            if (initParams[0] == kLocalRoleSpectate)
            {
                const bool vtableInitOk = InvokeSessionVtableInit("Tick_spectate");
                mod::Log(
                    "Takeover: spectator vtable init result=%d [deferred]",
                    vtableInitOk ? 1 : 0);
                LogInitWriteSnapshot("Tick_spectateVtable1_post");
            }

            // Init-snapshot: record the DLL ExitProcess Jcc call-site bytes
            // before patching them.  RestoreDllExitProcessPatches() (called in
            // CancelSessionUnlocked on exit) will undo exactly these changes.
            // For online/spectate the Jcc sites only cover the tournament tick
            // path; the online ExitProcess path is handled by the IAT hook +
            // OurFrameDispatch longjmp.  Logging here lets us correlate the
            // init snapshot with the corresponding restore in the log file.
            mod::Log(
                "Takeover: init snapshot step SaveAndApplyDllExitProcessPatches "
                "for mode=%d (to be reversed by RestoreDllExitProcessPatches on exit)",
                initParams[0]);
            (void)SaveAndApplyDllExitProcessPatches();
            mod::Log(
                "Takeover: init snapshot complete for mode=%d",
                initParams[0]);

            StabilizeOnlineSessionBindingAfterInit(initParams[0]);

            // NOTE: The spectate ring-buffer flush that used to live here
            // has been moved to StartSession (before ResumeThread).  Flushing
            // here — after the child process has already been running for up
            // to a game frame — discarded the beginning of the host's input
            // replay stream, causing spectator desync at charselect.
            // See the "Pre-launch ring buffer flush" block in StartSession.

            // --- Final snapshot after all init steps complete ---
            LogInitWriteSnapshot("Tick_initSequence_complete");
            LogSessionDiagnosticState("Tick_init_handshake_post");
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

    // --- Console error detection ---
    // If the Revival process reported a connection error (e.g., "Connection timed out",
    // "Source quit or timed out", "Host timed out", "Remote timed out", "Peer died",
    // "Socket error"), act immediately.
    if (ioStatus->consoleErrorSerial > 0)
    {
        if (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
        {
            mod::Log(
                "Takeover: console error detected serial=%d text='%s' phase=%d — transitioning to Failed",
                ioStatus->consoleErrorSerial,
                ioStatus->consoleErrorText,
                static_cast<int>(phase));
            SetPhase(ioStatus, NetbridgePhase::Failed, ioStatus->consoleErrorText);
            ReinitLocalPlay();
            return;
        }
        if (phase == NetbridgePhase::Connected)
        {
            // During Connected phase (charselect/gameplay after handoff),
            // the helper process detected a network disconnect.  The per-
            // frame tick hook (OurPerFrameTickHook) also polls this flag
            // and performs full recovery.  Here we transition the bridge
            // phase so any code that checks GetStatus() sees the session
            // has ended.  The heavy recovery (ForceLocalPlayInit, restore
            // patches, ForceGameModeToTitle) is handled by the tick hook.
            mod::Log(
                "Takeover: console error detected serial=%d text='%s' phase=Connected — transitioning to SessionEnded",
                ioStatus->consoleErrorSerial,
                ioStatus->consoleErrorText);
            SetPhase(ioStatus, NetbridgePhase::SessionEnded, ioStatus->consoleErrorText);
            return;
        }
    }

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

    // Spectator sessions don't go through the rollback sync handshake, so
    // the runtime-ready probe never fires.  Promote to Connected as soon as
    // init is applied and the DLL exit-process patches are saved (the last
    // step of the spectator init sequence).  This allows phase-gated code
    // (name reading, disconnect handling) to work correctly for spectators.
    if ((currentPhase == NetbridgePhase::Connecting || currentPhase == NetbridgePhase::DelaySetup)
        && g_localInitAppliedForSession
        && g_localRoleFlag == kLocalRoleSpectate
        && AreDllExitPatchesSaved())
    {
        SetPhase(ioStatus, NetbridgePhase::Connected, nullptr);
        if (ioConnectStartTick != nullptr)
        {
            *ioConnectStartTick = GetTickCount();
        }
        mod::Log(
            "Takeover: spectator promoted to connected (init applied, exit patches saved)");
    }
}

// ---------------------------------------------------------------------------
// CancelSessionUnlocked — shared body for CancelSession and exit interception.
// Caller MUST hold g_mutex.
// ---------------------------------------------------------------------------
static void CancelSessionUnlocked(const char* reason, NetbridgeStatus* ioStatus)
{
    // --- Diagnostic dump before teardown (2nd-session crash investigation) ---
    LogSessionDiagnosticState("CancelSession_entry");
    mod::Log(
        "DIAG[CancelSession]: reason='%s' forceLocalPlayInitCount=%d",
        (reason != nullptr) ? reason : "",
        GetForceLocalPlayInitCount());

    // --- Session boundary cleanup logging ---
    {
        uint8_t hookA[10] = {};
        uint8_t hookB[8] = {};
        memcpy(hookA, reinterpret_cast<const void*>(0x401582), 10);
        memcpy(hookB, reinterpret_cast<const void*>(0x401642), 8);
        mod::Log(
            "CancelSession: EXE hooks at teardown "
            "0x401582=[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X] "
            "0x401642=[%02X %02X %02X %02X %02X %02X %02X %02X]",
            hookA[0], hookA[1], hookA[2], hookA[3], hookA[4],
            hookA[5], hookA[6], hookA[7], hookA[8], hookA[9],
            hookB[0], hookB[1], hookB[2], hookB[3], hookB[4],
            hookB[5], hookB[6], hookB[7]);
    }
    // Log init-once guard state at teardown.
    if (g_activeRevival != nullptr)
    {
        HMODULE revival = GetModuleHandleA("EfzRevival.dll");
        if (revival != nullptr)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
            const uint16_t guardVal = *reinterpret_cast<const volatile uint16_t*>(
                base + g_activeRevival->initOnceGuardOffset);
            const uintptr_t timerPtr = *reinterpret_cast<const volatile uintptr_t*>(
                base + g_activeRevival->timerPtrOffset);
            const uintptr_t renderCtx = *reinterpret_cast<const volatile uintptr_t*>(
                base + g_activeRevival->renderContextGlobalOffset);
            mod::Log(
                "CancelSession: DLL globals guard=0x%04X timer=0x%08lX renderCtx=0x%08lX",
                static_cast<unsigned>(guardVal),
                static_cast<unsigned long>(timerPtr),
                static_cast<unsigned long>(renderCtx));
        }
    }

    InterlockedExchange(&g_startAbortRequested, 1);
    FlushPendingConsoleOutput("cancel");

    const bool hadProcess = ProcessAlive(ioStatus);
    if (hadProcess && g_revivalProcess != nullptr)
    {
        TerminateProcess(g_revivalProcess, 0);
    }
    CloseProcessHandle(ioStatus);
    // Close the job object so any grandchild processes (cmd.exe, conhost.exe)
    // spawned by EfzRevival.exe are also terminated.  A fresh job will be
    // created for the next StartSession call.
    CloseChildJobObject();
    RestoreDllExitProcessPatches();
    ReinitLocalPlay();

    // ---- Additional cleanup (Issues 1, 4, 5 in CONNECTION_INTERRUPTION doc) ----
    // During normal operation (not process shutdown), reinitialise the DLL
    // session to local-play immediately.  This replaces the dead/neutralised
    // online session object with a live one so the game loop always has a
    // valid vtable for frame dispatch.  During DLL_PROCESS_DETACH we skip
    // this because calling back into EfzRevival.dll under the loader lock
    // can deadlock or crash.
    const bool isShutdown = (reason != nullptr &&
        (std::strcmp(reason, "shutdown") == 0 ||
         std::strcmp(reason, "emergency") == 0));

    if (!isShutdown)
    {
        // If we're inside the per-frame tick (sub_1006E570 -> vtable[2] ->
        // RollbackLoopTick), ForceLocalPlayInit MUST NOT run now because it
        // would destroy the session that RollbackLoopTick is actively using
        // as 'this' (use-after-free -> crash in SetEvent(this[2])).
        // Defer the cleanup to OurPerFrameTickHook which will execute it
        // after the original sub_1006E570 returns safely.
        if (IsInsideFrameTick())
        {
            mod::Log(
                "Takeover: cancel cleanup — DEFERRED (inside frame tick, "
                "ForceLocalPlayInit would destroy active session)");
            RequestDeferredCancelCleanup();
        }
        else
        {
            const bool initOk = ForceLocalPlayInit();
            mod::Log(
                "Takeover: cancel cleanup — ForceLocalPlayInit result=%d",
                initOk ? 1 : 0);

            const bool clearOk = ClearRevivalText();
            mod::Log(
                "Takeover: cancel cleanup — ClearRevivalText result=%d",
                clearOk ? 1 : 0);

            const bool textOk = DisableRevivalTextRendering();
            mod::Log(
                "Takeover: cancel cleanup — DisableRevivalTextRendering result=%d",
                textOk ? 1 : 0);

            mod::ResetCrashRecoveryState();
            mod::Log("Takeover: cancel cleanup — crash recovery state reset");

            ResetGameModeValidation();
            mod::Log("Takeover: cancel cleanup — game mode validation reset");
        }
    }
    else
    {
        mod::Log(
            "Takeover: cancel cleanup — skipped DLL re-init (shutdown path reason='%s')",
            reason != nullptr ? reason : "");
        // Still reset the netplay role even on shutdown so stale state
        // doesn't leak to a future session (belt-and-suspenders).
        g_netplayRole = kNetplayRoleNone;
    }
    // ---- End additional cleanup ------------------------------------------------

    // Clear per-session IAT hook tracking vectors to prevent unbounded
    // growth across sessions (GAP 4 in cleanup audit).
    {
        std::lock_guard<std::mutex> ftLock(g_fakeThreadMutex);
        const size_t oldFakeCount = g_fakeThreads.size();
        // Unlock before calling ClearFakeThreads which takes the same lock,
        // so log the count first then clear outside the lock.
        mod::Log("Takeover: cancel cleanup — clearing g_fakeThreads (count=%zu)",
                 oldFakeCount);
    }
    ClearFakeThreads();
    {
        std::lock_guard<std::mutex> raLock(g_redirectAllocMutex);
        mod::Log("Takeover: cancel cleanup — clearing g_redirectAllocations (count=%zu)",
                 g_redirectAllocations.size());
    }
    ClearRedirectAllocations();

    g_localInitAppliedForSession = false;
    InterlockedExchange(&g_injectedDelayPromptSerial, 0);
    InterlockedExchange(&g_injectedDelayPromptServedSerial, 0);
    InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, 0);
    g_injectedDelayPromptWaitStartTick = 0;
    InterlockedExchange(&g_injectedSpectateConfirmPromptSerial, 0);
    InterlockedExchange(&g_injectedSpectateConfirmPromptServedSerial, 0);
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
    g_delayPromptMetrics = {};

    // Clear stale serials in the shared memory block so that
    // ReadDelayPromptSignal / ReadSpectateConfirmPromptSignal won't
    // re-read the old session's values after the next RefreshRuntimeStatus.
    if (g_hostBlock != nullptr)
    {
        InterlockedExchange(&g_hostBlock->delayPromptSerial, 0);
        InterlockedExchange(&g_hostBlock->delayPromptServedSerial, 0);
        InterlockedExchange(&g_hostBlock->delayMetricsSerial, 0);
        InterlockedExchange(&g_hostBlock->delayInputSerial, 0);
        InterlockedExchange(&g_hostBlock->delayInputServedSerial, 0);
        g_hostBlock->delayInputValue = -1;
        g_hostBlock->delayAveragePingMs = -1;
        g_hostBlock->delayMinPingMs = -1;
        g_hostBlock->delayMaxPingMs = -1;
        g_hostBlock->delayRecommended = -1;
        g_hostBlock->delayRangeMin = 0;
        g_hostBlock->delayRangeMax = 20;
        InterlockedExchange(&g_hostBlock->spectateConfirmPromptSerial, 0);
        InterlockedExchange(&g_hostBlock->spectateConfirmPromptServedSerial, 0);
        g_hostBlock->spectateConfirmPromptKind = 0;
        InterlockedExchange(&g_hostBlock->spectateConfirmInputSerial, 0);
        InterlockedExchange(&g_hostBlock->spectateConfirmInputServedSerial, 0);
        g_hostBlock->spectateConfirmInputValue = 0;
        InterlockedExchange(&g_hostBlock->consoleErrorSerial, 0);
        g_hostBlock->consoleErrorText[0] = '\0';
    }
    ResetNativeWorkflowFlags();
    g_lastConnectingDiagnosticTick = 0;
    g_lastSpectateConsoleSnapshotTick = 0;
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
    CloseMirrorLogFiles();

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

    // --- Diagnostic dump after teardown (2nd-session crash investigation) ---
    LogSessionDiagnosticState("CancelSession_exit");
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

    LogSessionDiagnosticState("ConsumeExitInterception_entry");

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

    // ---- Reverse-init teardown sequence ------------------------------------
    // This mirrors the forward init steps in reverse order:
    //
    //   FORWARD INIT                        REVERSE EXIT (this function)
    //   ──────────────────────────────────  ────────────────────────────────────
    //   a) EfzRevival.exe spawned        →  step 1: TerminateProcess
    //   b) DLL ExitProcess Jcc patched   →  step 1: RestoreDllExitProcessPatches
    //   c) EXE patches (tournament only) →  step 2: RestoreTournamentExePatches
    //   d) Online session object created →  step 3: ForceLocalPlayInit (new local)
    //   e) Text rendering active         →  step 4: ClearRevivalText / Disable
    //                                               (tournament: both;
    //                                                online/spectate: Disable only)
    // -----------------------------------------------------------------------

    mod::Log(
        "Takeover: consuming exit interception mode=%d "
        "(step 1: cancel session / terminate peer / restore DLL patches)",
        mode);
    CancelSessionUnlocked("exit_intercepted", ioStatus);

    // step 2: revert EXE patches applied by the session constructor.
    // Tournament: 4 inline EXE hooks (0x763F04, 0x763E50, 0x754C1A, 0x7599ED).
    // Online/spectate: no additional EXE patches (sub_1006E590 re-applies its
    // frame-by-frame patches every tick and they are self-healing after step 3).
    if (mode == kLocalRoleTournament)
    {
        mod::Log("Takeover: exit interception step 2 — RestoreTournamentExePatches");
        RestoreTournamentExePatches();
    }
    else
    {
        mod::Log(
            "Takeover: exit interception step 2 — no EXE patch restore "
            "needed for mode=%d (online/spectate)",
            mode);
    }

    // step 3: reinstate a live local-play session.
    // Both OurFrameDispatch (frame-hook longjmp recovery) and
    // CancelSessionUnlocked (step 1 above) now call ForceLocalPlayInit
    // eagerly.  This third call is a defence-in-depth guarantee: even if
    // the earlier calls were bypassed (e.g. VEH TOCTOU path or a code
    // path that doesn't go through OurFrameDispatch), the session is
    // always replaced here.  Repeated calls are harmless.
    mod::Log("Takeover: exit interception step 3 — ForceLocalPlayInit (defence-in-depth)");
    ForceLocalPlayInit();

    // step 4: clear any DLL-side text overlay state left by the session.
    // Tournament writes win counters and nicknames to the EfzRender text
    // buffer.  init(2,102) zeroes dword_100A0778 so we must RestoreRenderContext
    // before the clear; ClearRevivalText does this internally.
    //
    // Online/spectate do not write to the EfzRender buffer (they use ImGui
    // overlays), but we call DisableRevivalTextRendering as a defensive
    // clean-up in case the session left the DLL text-draw hook active.
    mod::Log("Takeover: exit interception step 4 — clear text / disable renderer");
    // Both tournament and online/spectate paths share the same cleanup now.
    // SaveRenderContext() is called in StartSession for all session types,
    // so RestoreRenderContext inside ClearRevivalText works for all modes.
    ClearRevivalText();
    DisableRevivalTextRendering();

    mod::Log("Takeover: exit interception fully consumed mode=%d", mode);
    LogSessionDiagnosticState("ConsumeExitInterception_exit");
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

bool NotifyTitleScreenActive(NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_localRoleFlag != kLocalRoleTournament)
    {
        return false;
    }

    // The Jcc patches prevent ExitProcess from firing, so NeutralizeExitProcess
    // (and ConsumeRevivalExitInterception) never triggers on the normal path when
    // the tournament match ends and the game returns to mode 0 (title screen).
    // Poll the game mode directly: if we're still flagged as tournament but the
    // game is back on the title screen, run the same cleanup chain proactively.
    int gameMode = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(g_activeRevival->addrGameModeCurrentIndex), &gameMode)
        || gameMode != 0)
    {
        return false;
    }

    mod::Log("Takeover: tournament returned to title screen (mode 0) — cleaning up proactively");

    RestoreDllExitProcessPatches();
    RestoreTournamentExePatches();
    ForceLocalPlayInit();
    ClearRevivalText();
    DisableRevivalTextRendering();
    g_localRoleFlag = kLocalRoleLocalPlay;

    RefreshRuntimeStatus(ioStatus);
    return true;
}

bool IsPeerProcessAlive()
{
    // Advisory: no mutex needed, single-pointer read is atomic on x86.
    // caller must handle the TOCTOU window between this check and acting on it.
    const HANDLE h = g_revivalProcess;
    if (h == nullptr)
    {
        return false;
    }
    DWORD exitCode = STILL_ACTIVE;
    if (GetExitCodeProcess(h, &exitCode) == FALSE)
    {
        return false;
    }
    return exitCode == STILL_ACTIVE;
}

bool IsNetplayExitInterceptionPending()
{
    return InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0) != 0;
}

} // namespace netplay::bridge::takeover


















