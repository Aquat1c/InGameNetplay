#pragma once
// Internal shared header; not part of the public API.

#include "netplay/bridge/revival_addresses.h"
#include "netplay/bridge/external_launcher_guard.h"
#include "netplay/bridge/revival_launch_policy.h"
#include "netplay/bridge/session_bridge.h"
#include "logger.h"

#include <csetjmp>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <windows.h>

namespace netplay::bridge::takeover
{

constexpr uint32_t kIpcMagic = 0x4E425247;
constexpr uint32_t kIpcVersion = 6;
constexpr char kSharedBlockName[] = "EFZNetbridge_Shared";
constexpr char kInitReadyEventName[] = "EFZNetbridge_InitReady";
constexpr char kConsoleReadyEventName[] = "EFZNetbridge_ConsoleReady";
constexpr DWORD kStartTimeoutMs = 15000;
constexpr DWORD kInjectedCaptureReadyTimeoutMs = 3000;
constexpr DWORD kLatePatchRetryLogIntervalMs = 3000;
constexpr DWORD kPromptSpectateConfirmWaitTimeoutMs = 30000;

// Version-specific Revival addresses live in revival_addresses.h; selected at runtime via g_activeRevival.

// Revival init() roleFlag semantics (from decompiled EfzRevival.dll):
// 0 = Online session (host AND join)
// 1 = Spectator session
// 2 = Local play (offline practice)
// 3 = Offline tournament
constexpr int kLocalRoleOnline = 0;
constexpr int kLocalRoleSpectate = 1;
constexpr int kLocalRoleLocalPlay = 2;
constexpr int kLocalRoleTournament = 3;

constexpr int kNetplayRoleNone      = 0;  // not in a netplay session
constexpr int kNetplayRoleHost      = 1;  // hosting (P1 side)
constexpr int kNetplayRoleClient    = 2;  // joined (P2 side - inputs swapped)
constexpr int kNetplayRoleSpectator = 3;  // spectating

enum class PeerProcessOwnership : uint8_t
{
    None = 0,
    SpawnedHelper,
    ExternalLauncherParent,
};

#pragma pack(push, 1)
struct SharedBlock
{
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t hostPid = 0;
    uint32_t hostRevivalBase = 0;
    uint32_t hostRevivalTimestamp = 0;
    volatile LONG initSerial = 0;
    int initParams[2] = {0, 0};
    volatile LONG consoleSerial = 0;
    char consoleInput[64] = {};
    volatile LONG consoleAuxSerial = 0;
    char consoleInputAux[128] = {};
    volatile LONG delayPromptSerial = 0;
    volatile LONG delayPromptServedSerial = 0;
    volatile LONG delayMetricsSerial = 0;
    int delayAveragePingMs = -1;
    int delayMinPingMs = -1;
    int delayMaxPingMs = -1;
    int delayRecommended = -1;
    int delayRangeMin = 0;
    int delayRangeMax = 20;
    volatile LONG delayInputSerial = 0;
    volatile LONG delayInputServedSerial = 0;
    int delayInputValue = -1;
    volatile LONG spectateConfirmPromptSerial = 0;
    volatile LONG spectateConfirmPromptServedSerial = 0;
    int spectateConfirmPromptKind = 0;
    volatile LONG spectateConfirmInputSerial = 0;
    volatile LONG spectateConfirmInputServedSerial = 0;
    int spectateConfirmInputValue = 0; // 0 = not set, otherwise raw Revival menu choice
    volatile LONG dbgReadConsoleHits = 0;
    volatile LONG dbgReadConsoleAutoHits = 0;
    volatile LONG dbgCreateProcessHits = 0;
    volatile LONG dbgWriteProcessHits = 0;
    volatile LONG dbgCreateRemoteThreadHits = 0;
    volatile LONG consoleErrorSerial = 0;
    char consoleErrorText[128] = {};
    volatile LONG peerQuitDiagnosticSerial = 0;
    char peerQuitDiagnosticText[8192] = {};
    // Listener acknowledgement seqlock (odd = writer active, even = complete);
    // the PID rejects stale lines from a retiring helper.
    volatile LONG hostListenerSerial = 0;
    volatile LONG hostListenerFamily = 0; // NetworkFamily numeric value (4/6)
    volatile LONG hostListenerPort = 0;
    volatile LONG hostListenerProcessId = 0;
    volatile LONG hostExpectedListenerPort = 0;
    volatile LONG isHostSession = 0;
    // Native Host reader cancellation. EfzRevival writes one exact
    // VK_RETURN/BEL INPUT_RECORD before joining the outgoing console reader.
    volatile LONG consoleControlWakeRequestSerial = 0;
    volatile LONG consoleControlWakeServedSerial = 0;
    // The exact BEL generation that replaces the generic Host reader with the
    // real delay reader. The prompt text is written before that replacement.
    volatile LONG delayPromptTransitionWakeSerial = 0;
    volatile LONG nativeDelayTimeoutSerial = 0;
    // The timeout transition's exact BEL generation. Recovery cannot publish
    // until this generation has actually been returned by ReadConsole.
    volatile LONG nativeDelayTimeoutRequiredWakeSerial = 0;
    volatile LONG nativeDelayTimeoutHandledSerial = 0;
    // Helper-local raw console consumer is running. Host waits for this before
    // resuming the helper main thread, so startup prompts cannot outrun it.
    volatile LONG helperCaptureReady = 0;
};
#pragma pack(pop)

using RevivalInitFn = int(__cdecl*)(int*);

struct FakeThreadInfo
{
    HANDLE handle = nullptr;
    DWORD exitCode = 0;
};

struct RedirectAllocationInfo
{
    uintptr_t base = 0;
    SIZE_T size = 0;
};

struct RevivalSyncFlags
{
    int gameMode = -1;
    int mode0Flag1084 = -1;
    int sessionByte = -1;
    int globalFlag4964 = -1;
    int globalFlag4965 = -1;
    bool inRollbackSyncState = false;
    bool inRollbackActiveState = false;
};

struct TempIpcContext
{
    HANDLE mapHandle = nullptr;
    SharedBlock* block = nullptr;
    HANDLE initEvent = nullptr;
    HANDLE consoleEvent = nullptr;
};

struct RuntimeReadyProbe
{
    bool nativeSyncReady = false;
    bool localInitApplied = false;
    bool delayPromptSeen = false;
    bool delayInputApplied = false;
    bool sessionPointerValid = false;
    bool helperPidMatches = false;
    bool helperHandleMatches = false;
    bool helperBindingReady = false;
    bool ready = false;
    const char* source = "none";
};

extern std::mutex g_mutex;
extern const RevivalAddressProfile* g_activeRevival;

void DetectRevivalVersion();
void EnsureActiveRevivalProfile();
bool ActiveRevivalProfileSupportsSessionStart();

// Resolve a legacy channel name to the active binary's wire name (1.02j appends "_Spec").
const char* RevivalWireName(const char* legacyName);

extern HMODULE g_localRevivalModule;
extern RevivalInitFn g_localInitFn;
extern HANDLE g_revivalProcess;
extern DWORD g_revivalProcessId;
extern PeerProcessOwnership g_peerProcessOwnership;
extern revival_launch::LaunchDisposition g_launchDisposition;
extern int g_localRoleFlag;
extern int g_netplayRole;
extern uintptr_t g_hostRevivalBase;

extern HANDLE g_hostMapHandle;
extern SharedBlock* g_hostBlock;
extern HANDLE g_hostInitEvent;
extern HANDLE g_hostConsoleEvent;

extern bool g_injectedReady;
extern HANDLE g_injectedMapHandle;
extern SharedBlock* g_injectedBlock;
extern HANDLE g_injectedInitEvent;
extern HANDLE g_injectedConsoleEvent;
extern volatile LONG g_injectedLastConsoleSerialServed;
extern volatile LONG g_injectedLastConsoleAuxSerialServed;
extern volatile LONG g_injectedActiveConsoleAuxSerial;
extern volatile LONG g_injectedConsoleAuxScriptOffset;
extern volatile LONG g_injectedAutoConsoleFallbackCount;
extern volatile LONG g_injectedConsoleOutputHits;
extern volatile LONG g_injectedTerminateUnknownPidHits;
extern volatile LONG g_injectedDelayPromptSerial;
extern volatile LONG g_injectedDelayPromptServedSerial;
extern volatile LONG g_injectedConnectedFromDelayPromptSerial;
extern volatile LONG g_injectedSpectateConfirmPromptSerial;
extern volatile LONG g_injectedSpectateConfirmPromptServedSerial;
extern DWORD g_injectedSpectateConfirmPromptWaitStartTick;
extern volatile LONG g_injectedFingerprintReadHits;
extern uintptr_t g_injectedInitAddress;
extern bool g_injectedLazyBound;
extern volatile LONG g_injectedLazyBootstrapState;
extern volatile LONG g_remoteThreadCallIndex;
extern volatile LONG g_startAbortRequested;
extern HANDLE g_fakeProcessThreadHandle;
extern bool g_initCapturedFromWrite;
extern DWORD g_lastConnectingDiagnosticTick;
extern uintptr_t g_lastSessionPtrOffset;
extern uintptr_t g_lastValidatedSessionPtr;
extern DWORD g_lastSessionPointerMismatchTick;
extern DWORD g_lastRuntimeReadyProbeLogTick;
extern uint32_t g_lastRuntimeReadyProbeMask;
extern bool g_lastRuntimeReadyProbeMaskValid;
extern bool g_localInitAppliedForSession;
extern bool g_spectatorPostInitAttemptedForSession;
extern bool g_spectatorPostInitSucceededForSession;
extern volatile LONG g_deferredLifecycleWorkRequested;
extern volatile LONG g_deferredTitleSelection;
extern bool g_tournamentReturnCleanupPending;
extern volatile LONG g_externalTournamentInitialTitleLeft;
void ArmTournamentReturnCleanup(const char* reason);
extern uintptr_t g_remoteInjectedSelfBase;
extern uintptr_t g_externalLauncherGuardRemoteBase;
extern DWORD g_lastLatePatchRetryTick;
extern DWORD g_lastLatePatchRetryLogTick;
extern DWORD g_latePatchRetryAttempts;
extern DWORD g_latePatchRetrySuccesses;
extern bool g_lastLatePatchRetryResultValid;
extern bool g_lastLatePatchRetryResult;
extern bool g_observedTakeoverCreatePath;
extern std::mutex g_fakeThreadMutex;
extern std::vector<FakeThreadInfo> g_fakeThreads;
extern std::mutex g_redirectAllocMutex;
extern std::vector<RedirectAllocationInfo> g_redirectAllocations;
extern volatile LONG g_redirectWriteBlockedHits;
extern volatile LONG g_sessionHistoryRepairHits;
extern std::mutex g_consoleLogMutex;
extern std::string g_consolePendingWriteFile;
extern std::string g_consolePendingWriteFileDisk;
extern std::string g_consolePendingWriteConsoleA;
extern std::string g_consolePendingWriteConsoleW;
extern std::string g_consolePendingWriteConsoleOutputCharacterA;
extern std::string g_consolePendingWriteConsoleOutputCharacterW;
extern std::string g_consolePendingOutputDebugStringA;
extern std::string g_consolePendingOutputDebugStringW;
extern bool g_captureRevivalNativeLogsConfigured;
extern bool g_captureRevivalNativeLogs;
extern bool g_revivalErrorCodeNullGuardPatched;
extern uintptr_t g_revivalErrorCodeNullGuardPatchedBase;
extern void* g_revivalErrorCodeNullGuardStub;
extern DelayPromptMetrics g_delayPromptMetrics;
extern volatile LONG g_revivalExitIntercepted;
extern volatile LONG g_revivalExitMode;
extern bool g_nativeWorkflowLoadedSeen;
extern bool g_nativeWorkflowMatchLoopSeen;
extern bool g_nativeWorkflowTournamentSeen;
extern bool g_nativeWorkflowPeerDiedSeen;
extern bool g_nativeWorkflowHolePunchDiedSeen;
extern bool g_holePunchServerConfigLoaded;
extern std::string g_configuredHolePunchServer;

void CopyString(char* dst, size_t dstSize, const char* src);
bool ExtractConsoleScriptLine(const char* script, LONG* inOutOffset, char* outLine, size_t outLineSize, bool* outHasMore);
bool IsLikelyTextChunk(const char* text, size_t length);
std::string TrimAscii(const std::string& text);
std::string ToLowerAscii(std::string text);
bool TryExtractDiedEndpoint(const std::string& text, std::string* outEndpoint);
std::string LoadConfiguredHolePunchServer();
bool ContainsCaseInsensitive(const std::string& text, const char* needle);
bool ParseIntAt(const std::string& text, size_t start, int* outValue, size_t* outEnd);
bool ExtractIntAfterToken(const std::string& text, const char* token, int* outValue);
bool ExtractDelayRange(const std::string& text, int* outMin, int* outMax);
void EnsureHostLogEfzIatPatched(bool verboseLogs);
DelayPromptMetrics ParseDelayPromptMetricsFromText(const std::string& text, bool* outHasMetrics);
void PublishDelayPromptMetrics(const DelayPromptMetrics& metrics, LONG serial);
void PrimeManagedLogEfzHistory();
bool StartConsoleCaptureWorker(bool enableRawIngress);
void BeginManagedLogEfzWrite();
void EndManagedLogEfzWrite();
bool IsManagedLogEfzWriteActive();
void ResetNativeWorkflowFlags();
void NoteConsolePromptLine(
    const std::string& text,
    LONG controlWakeRequestSerialSnapshot = -1);
std::string* SelectPendingConsoleLine(const char* sourceTag);
bool IsLikelyRevivalDiskLogPath(const std::string& path);
void RegisterConsoleCaptureFileHandle(HANDLE hFile, bool captureAsTextLog);
void LogConsoleTextChunk(const char* sourceTag, const char* text, size_t length);
void FlushPendingConsoleOutput(const char* reason);
void MaybeLogConsoleOutputChunk(HANDLE hFile, LPCVOID lpBuffer, DWORD nBytes);
void CloseMirrorLogFiles();
void StopManagedLogEfzWorker(bool waitForDrain);
void MaybeLogConsoleWriteAChunk(const VOID* lpBuffer, DWORD nChars);
void MaybeLogConsoleWriteWChunk(const VOID* lpBuffer, DWORD nChars);
void MaybeLogConsoleOutputCharacterAChunk(const VOID* lpBuffer, DWORD nChars, COORD writeCoord);
void MaybeLogConsoleOutputCharacterWChunk(const VOID* lpBuffer, DWORD nChars, COORD writeCoord);
void MaybeLogOutputDebugStringA(LPCSTR lpOutputString);
void MaybeLogOutputDebugStringW(LPCWSTR lpOutputString);
std::string ErrorString(DWORD code);
std::string BaseLower(const std::string& path);

bool IsReadableRange(const void* address, size_t size);
bool IsWritableRange(void* address, size_t size);
bool SafeReadInt(const void* address, int* outValue);
bool SafeReadPtr(const void* address, uintptr_t* outValue);
bool SafeReadByte(const void* address, uint8_t* outValue);
bool ReadModuleImageRange(HMODULE module, uintptr_t* outBase, uintptr_t* outEnd);
int ReadRoleFlagFromRevival();
bool IsSessionPointerByVtable(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd);
bool IsLikelySessionPointer(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd);
uintptr_t ReadSessionPointerFromRevival();
uintptr_t ReadSessionPointerFromRevivalLoose();
uintptr_t ReadSessionPointerForMutation(bool* outUsedCached);
bool ReadRevivalSyncFlags(RevivalSyncFlags* outFlags);
void RefreshRuntimeStatus(NetbridgeStatus* ioStatus);
bool SetLocalRoleFlag(int roleFlag, const char* reason);
bool SetRoleFlagDirect(int roleFlag, const char* reason);
void StartSpectatorEscWatcher();
void StopSpectatorEscWatcher();
bool NeutralizeTournamentAutoNav();
bool SaveTournamentExePatches();
bool AdoptExistingTournamentExePatchState();
bool RestoreTournamentExePatches();
bool SaveAndApplyDllExitProcessPatches();
bool SaveAndApplyExternalTournamentExitGuard();
bool IsExternalTournamentExitGuardOwned();
bool ArmExternalLauncherExitRecovery();
void PrimeGracefulQuitRingForSession();
bool RestoreDllExitProcessPatches();
bool AreDllExitPatchesSaved();
bool DestroyCurrentSession(const char* caller);
bool ForceLocalPlayInit();
bool InvokeSessionVtableInit(const char* caller);
bool IsRevival102jSpectatorPostInitReady(const char* caller);
bool InvokeRevival102jSpectatorPostInit(const char* caller);
bool SaveRenderContext();
bool RestoreRenderContext();
bool ClearRevivalText();
bool SetRevivalTextRenderingEnabled(bool enable, const char* reason);
bool DisableRevivalTextRendering();
bool ResetRevivalTextRenderingAfterCleanup(const char* reason);
bool ShouldRepeatPostExitTextCleanup();
bool RestoreRenderContextForGameplayExitCleanup();
void MarkRenderContextConsumedForGameplayExitCleanup();
bool ClearRevivalTextWithCurrentRenderContext();
bool SetRevivalTextRenderingEnabledWithCurrentRenderContext(bool enable, const char* reason);
bool DisableRevivalTextRenderingWithCurrentRenderContext();
bool ResetRevivalTextRenderingAfterCleanupWithCurrentRenderContext(const char* reason);
int GetRevivalGraphicsPatchState();
bool EnsureRevivalGraphicsPatchSetEnabled(const char* reason);

// Reverse the P1/P2 input-config swap that Revival applied when we joined
// as client (P2).  No-op unless g_netplayRole == kNetplayRoleClient.
bool ReverseInputSwapIfClient();
void SetClientInputSwapApplied(bool applied);
bool IsClientInputSwapApplied();

void ResetDebugCounters(SharedBlock* block);
bool InvokeStartInitPlayer(int initMode);
void StabilizeOnlineSessionBindingAfterInit(int initMode);
void RepairRollbackHistoryBindingsIfNeeded();
void MarkRevivalSyncDiagnosticsSessionStart(const char* context);
bool InstallNetplayFrameHook();
bool RemoveNetplayFrameHook();
bool HasAnyNetplayFrameHookInstalled();
bool HasNetplayPerFrameTickHookInstalled();
void SetExternalLauncherAttachPending(bool pending);
bool WaitForExternalLauncherAttachBoundary(DWORD timeoutMs);
bool IsExternalLauncherAttachBoundaryPending();
bool CompleteExternalLauncherAttachBoundary(bool commit);
void EmergencyQuarantineExternalLauncherAttachBoundary();
// Force the game mode index to 0 (title screen); safe from the crash handler VEH.
bool ForceGameModeToTitle();

bool RestoreNeutralizedSessionVtableForCleanup(
    uintptr_t sessionPtr,
    uintptr_t* outOriginalVtable);

void SaveExeFrameHookBytes();
void RestoreExeFrameHookBytes();

void SaveExeDispatchHookBytes();
void RestoreExeDispatchHookBytes();
void RestoreExeDispatchHookBytesAfterSessionInit(int initMode);
bool RestoreExeDispatchHookForTitle(const char* caller);

void SaveModeCtorHookBytes();
void RestoreModeCtorHookBytes();
void DiscardModeCtorHookBytes();
void RestoreModeCtorOriginalBytes();

// Must run after every init(): unfixed trampoline E8/E9/0F 8x displacements crash with wild EIP.
void FixupModeConstructorTrampolines(const char* caller);

void ResetModeConstructorTrampolineCache();

void LogSessionDiagnosticState(const char* context);
void LogSessionDiagnosticStateForced(const char* context);
void LogInitWriteSnapshot(const char* context);
bool IsRevival102jDeepDiagnosticsEnabled();
void LogRevival102jDeepStep(
    const char* context,
    const NetbridgeStatus* status = nullptr);
void LogRevival102jDeepSnapshot(
    const char* context,
    const NetbridgeStatus* status = nullptr);
void LogRevival102jDeepBytes(
    const char* context,
    const char* label,
    uintptr_t address,
    size_t size);

void IncrementForceLocalPlayInitCount();
int GetForceLocalPlayInitCount();
void ResetForceLocalPlayInitCount();

void ResetGameModeValidation();
uint32_t GetGameplayExitRecoveryFrameTick();
bool IsGameplayExitRecoveryInsideFrameTick();
bool ClearDeferredCancelCleanupForRecovery(const char* reason = nullptr);
bool SuppressDeferredCancelCleanupAfterGameplayRecovery(const char* origin);
bool IsDeferredCancelCleanupPending();
bool IsDeferredCancelCleanupGameplaySource();
const char* CurrentDeferredCancelCleanupReason();
uint8_t CurrentDeferredCancelCleanupSourceScreen();
void NotifyLocalProcessCloseForGameplayStall();
void ClearLocalProcessCloseForGameplayStall();
bool IsLocalProcessCloseForGameplayStallActive();

// Destructive cleanup must be deferred while the per-frame tick hook runs (mid-tick would use freed state).
bool IsInsideFrameTick();

void RequestDeferredCancelCleanup(const char* reason = nullptr);

void ArmOnlineMatchEscGracefulQuit();
bool ConsumeOnlineMatchEscGracefulQuit();
void ResetOnlineMatchEscGracefulQuit();

bool RequestInjectedPeerQuitBroadcast(const char* reason, DWORD waitMs);
void ResetInjectedPeerQuitBroadcastState();

DWORD RunInjectedPeerQuitBroadcast();

bool TryClassifyRevivalQuitEndpoint(
    const char* endpointText,
    bool* outIsActivePeer,
    bool* outIsSpectator);

bool IsPeerProcessAlive();
bool StartPeerProcessExitWatch();
void StopPeerProcessExitWatch(bool waitForExit = true);
bool HasPeerProcessExitSignal();

// Defined in revival_memory.cpp; read by iat_stubs.cpp.
extern jmp_buf       g_netplayFrameJmpBuf;
extern volatile bool g_netplayFrameJmpActive;
extern volatile DWORD g_netplayFrameJmpOwnerThreadId;

extern jmp_buf       g_netplayUiJmpBuf;
extern volatile bool g_netplayUiJmpActive;
extern volatile DWORD g_netplayUiJmpOwnerThreadId;

void* EnsureRevivalErrorCodeNullGuardStub();
bool OpenTempIpcContext(TempIpcContext* ctx, bool needInitEvent, bool needConsoleEvent);
void CloseTempIpcContext(TempIpcContext* ctx);
void ClearDelayPromptState(const char* reason);
void PublishDelayPromptSerial(
    LONG serial,
    LONG controlWakeRequestSerialSnapshot);
void ReadDelayPromptSignal(LONG* outPromptSerial, LONG* outPromptServedSerial);
void PublishSpectateConfirmPromptSerial(LONG serial, int promptKind);
void ReadSpectateConfirmPromptSignal(LONG* outPromptSerial, LONG* outPromptServedSerial, int* outPromptKind);
void PublishConsoleError(const char* errorText);
bool TryPublishHeldHostDelayTimeout(
    LONG controlWakeRequestSerialSnapshot = -1);
bool HasPendingHeldHostDelayTimeout();
void ReadConsoleError(LONG* outSerial, char* outText, int outTextSize);
void PublishHostListenerObservation(
    network::NetworkFamily family,
    uint16_t port);
void ClearHostListenerObservation();
bool ReadHostListenerObservation(
    LONG* outSerial,
    network::NetworkFamily* outFamily,
    uint16_t* outPort,
    DWORD* outProcessId);
void HandleTemporaryHostProtocolListenerAck();
void HandleTemporaryHostProtocolListenerAck(
    DWORD expectedProcessId,
    uint16_t expectedPort);
void ConfirmTemporaryHostProtocolListenerAckAtHandoff(
    DWORD expectedProcessId,
    uint16_t expectedPort);
bool RecoverTemporaryHostProtocolOverride(const char* reason);
bool PrepareTemporaryHostProtocolOverride(
    network::NetworkFamily effectiveFamily);
void RestoreTemporaryHostProtocolOverride(const char* reason);
bool GetHostProtocolOverrideState(
    HostProtocolOverrideState* outState);
bool BeginOptionsIniAccess(
    bool writeAccess,
    HostProtocolOverrideState* outState);
void EndOptionsIniAccess();
void ClearPeerQuitDiagnostic();
void AppendPeerQuitDiagnostic(const char* text);
void ReadPeerQuitDiagnostic(LONG* outSerial, char* outText, int outTextSize);
HMODULE SelfModule();
std::string ModulePath(HMODULE module);
void CleanupNativeHostShadowLogDirectory(const char* reason);
bool TryReadCaptureRevivalNativeLogsConfig(bool* outEnabled, std::string* outSourceTag);
bool CaptureRevivalNativeLogsEnabled();
std::string GameDirectory();
std::wstring GameDirectoryWide();
bool TryWriteClipboardAscii(const char* text);
void SetPhase(NetbridgeStatus* status, NetbridgePhase phase, const char* error);
void CloseProcessHandle(
    NetbridgeStatus* status,
    bool waitForPeerWatcher = true);
bool ProcessAlive(NetbridgeStatus* status);
bool CanTerminatePeerProcess();
bool IsExternalLauncherPeerProcess();
BOOL TerminatePeerProcessIfOwned(
    UINT exitCode,
    const char* context,
    bool waitForExit = true);
bool ReleasePeerProcessAfterTerminationAttempt(
    BOOL terminationSucceeded,
    NetbridgeStatus* status,
    const char* context,
    bool waitForPeerWatcher = true);
bool IsSyncReadyForVsHuman(const NetbridgeStatus* status);
bool RequiresNativeVsHumanSyncForHandoff(const NetbridgeStatus* status);
RuntimeReadyProbe EvaluateRuntimeReadyProbe(const NetbridgeStatus* status);
uint32_t BuildRuntimeReadyProbeMask(const RuntimeReadyProbe& probe);
bool HasRuntimeReadySignal(const NetbridgeStatus* status);
bool EnsureHostIpc();
void CloseHostIpc();
bool ResetHostSharedBlockForSession(
    bool isHostSession,
    uint16_t expectedListenerPort,
    const char* reason);
uint32_t BeginManagedSessionBoundary(const char* reason);
bool EnsureLocalRevivalLoaded(bool prepareManagedSession = false);
bool NeedsExternalLauncherSimulationHandoff();
bool CompleteExternalLauncherUiAttachment(
    bool hooksInstalled,
    bool simulationHandoffReady);
void ReinitLocalPlay();
static constexpr DWORD kExitProcessInterceptedException = 0xE0EF0001u;

bool PatchRevivalDllExitProcess();
bool IsRevivalDllExitProcessIatPatched();
bool RestoreRevivalDllExitProcessIat();
bool SignalGracefulQuitRing(const char* contextTag, uintptr_t callerRva);
void NeutralizeRevivalSessionVtable();
uintptr_t ResolveHostRevivalBase();
bool PatchRevivalErrorCodeNullGuard();
void PublishHostRevivalBase();
uintptr_t ResolveInjectedExpectedRevivalBase();
bool WriteIni(
    int role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni,
    network::NetworkFamily sessionFamily,
    bool writeHostProtocol);
bool IsCurrentProcessRevival();
bool IsRunningUnderWine();
void InitializeInjected();
void ShutdownInjected();

struct RemoteModuleRecord
{
    uintptr_t base = 0;
    std::string moduleLower;
};

std::vector<RemoteModuleRecord> EnumerateRemoteModules(DWORD processId);
bool ReadRemoteString(HANDLE process, uintptr_t address, char* out, size_t outSize);
bool InjectSelf(HANDLE process, uintptr_t* outRemoteBase);
bool WaitForPreloadedSelf(DWORD processId, uintptr_t* outRemoteBase, DWORD timeoutMs);
bool PatchIatModule(
    HANDLE process,
    uintptr_t moduleBase,
    const char* moduleName,
    const std::unordered_map<std::string, uint32_t>& patchMap,
    int* outPatched,
    bool verboseLogs);
std::unordered_map<std::string, uint32_t> BuildPatchMap(uintptr_t remoteBase);
bool PatchIat(HANDLE process, DWORD processId, const std::unordered_map<std::string, uint32_t>& patchMap, bool verboseLogs);
// In-process IAT patching for Wine; safe to call from DllMain.
int SelfPatchIat();
HANDLE CreateFakeThread(DWORD exitCode);
bool LookupFakeThread(HANDLE handle, DWORD* outExitCode);
void ClearFakeThreads();
void RegisterRedirectAllocation(void* base, SIZE_T size);
void ForgetRedirectAllocation(void* base);
bool IsWithinRedirectAllocation(const void* address, SIZE_T size);
void ClearRedirectAllocations();
bool HasInjectedContext();
void TryLazyBootstrapInjected();
bool EnsureInjectedContextFast();

BOOL StubCreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation);
BOOL StubReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead);
LPVOID StubVirtualAllocEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
BOOL StubVirtualFreeEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
BOOL StubWriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesWritten);
HANDLE StubCreateRemoteThread(HANDLE hProcess, LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId);
BOOL StubTerminateProcess(
    HANDLE hProcess,
    UINT uExitCode,
    const void* callerReturnAddress);
HANDLE StubOpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId);
BOOL StubReadConsoleA(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl);
BOOL StubReadConsoleW(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl);
BOOL StubWriteConsoleInputA(HANDLE hConsoleInput, const INPUT_RECORD* lpBuffer, DWORD nLength, LPDWORD lpNumberOfEventsWritten);
BOOL StubWriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
BOOL StubWriteConsoleA(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved);
BOOL StubWriteConsoleW(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved);
BOOL StubWriteConsoleOutputCharacterA(HANDLE hConsoleOutput, LPCSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten);
BOOL StubWriteConsoleOutputCharacterW(HANDLE hConsoleOutput, LPCWSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten);
VOID StubOutputDebugStringA(LPCSTR lpOutputString);
VOID StubOutputDebugStringW(LPCWSTR lpOutputString);
DWORD StubWaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
BOOL StubGetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
DWORD StubResumeThread(HANDLE hThread);

} // namespace netplay::bridge::takeover
