#pragma once

#include <cstddef>
#include <cstdint>

namespace netplay::bridge
{
enum class NetbridgeRole : int
{
    Host = 0,
    Join = 1,
    Spectate = 2,
    JoinSpectate = 3,   // Join (choice 3) with auto-accept spectate redirect
};

enum class NetbridgeSpectatePromptKind : int
{
    None = 0,
    HostAlreadyPlaying = 1,
    HostNotYetPlaying = 2,
};

enum class NetbridgePhase : int
{
    Idle = 0,
    Connecting = 1,
    DelaySetup = 2,
    Connected = 3,
    Failed = 4,
    SessionEnded = 5,
};

struct NetbridgeStatus
{
    int phase = static_cast<int>(NetbridgePhase::Idle);
    int role = -1;
    int roleFlag = -1;
    int syncGameMode = -1;
    int syncMode0Flag1084 = -1;
    int syncSessionByte = -1;
    int syncGlobalFlag4964 = -1;
    int syncGlobalFlag4965 = -1;
    int pingMs = -1;
    int rollbackFrames = -1;
    int delayPromptSerial = 0;
    int delayPromptServedSerial = 0;
    int spectateConfirmPromptSerial = 0;
    int spectateConfirmPromptServedSerial = 0;
    int spectateConfirmPromptKind = static_cast<int>(NetbridgeSpectatePromptKind::None);
    int localInitApplied = 0;
    int delaySetupReady = 0;
    int vsHumanSyncReady = 0;
    uint16_t port = 0;
    char address[64] = {};
    char nickname[64] = {};
    char p1Name[64] = {};
    char p2Name[64] = {};
    char errorMsg[128] = {};
    uint32_t phaseTick = 0;
    uint32_t processId = 0;
    int consoleErrorSerial = 0;
    char consoleErrorText[128] = {};

    // Revival session-object fields - populated when an online session is
    // active and the session pointer has been validated.
    int activePlayer = -1;      // 0 = P1 (host), 1 = P2 (client), -1 = unknown
    int sessionP1Wins = 0;      // P1 win count from Revival session object
    int sessionP2Wins = 0;      // P2 win count from Revival session object
};

struct DelayPromptMetrics
{
    int serial = 0;
    int averagePingMs = -1;
    int minPingMs = -1;
    int maxPingMs = -1;
    int recommendedDelay = -1;
    int minDelay = 0;
    int maxDelay = 20;
    int inputSerial = 0;
    int inputValue = -1;
};

bool IsCurrentProcessRevival();
bool IsRunningUnderWine();
// In-process IAT patching for Wine - safe to call from DllMain.
int SelfPatchIat();
void Initialize();
void Shutdown();
void EmergencyShutdown();
void InitializeInjectedProcess();
void ShutdownInjectedProcess();
void Tick();
// Lightweight per-frame export pulse - refreshes shared-memory state from
// the current g_status snapshot without calling takeover::Tick().  Safe to
// call from any game thread context (loading screen, battle, frame hook).
void TickExportOnly();
bool StartSession(
    NetbridgeRole role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni = true);
bool ApplyInputDelay(int delayFrames);
bool AnswerSpectatePromptChoice(int choice);
bool PrepareVsHumanHandoff();
bool RequiresNativeVsHumanSyncForHandoff();
void CancelSession(const char* reason);
bool ConsumeRevivalExitInterception(int* outMode);
void CompleteGameplayExitRecovery(int mode, const char* origin);
bool NotifyTitleScreenActive();
bool CompletePendingTournamentReturnCleanup();
// Returns true if the EfzRevival.exe peer process is still running.
// Advisory check (TOCTOU): the process may exit immediately after this call.
// Used to abort the state-1 handoff before returning a global-state-transition
// value to EFZ.exe - preventing ExitProcess from firing on the main thread
// in the absence of a setjmp recovery point.
bool IsPeerProcessAlive();
// Returns true if NeutralizeExitProcess has fired and the exit interception
// flag is pending consumption.  Used by the crash handler to detect the TOCTOU
// window between IsPeerProcessAlive() returning true and the peer actually dying.
bool IsNetplayExitInterceptionPending();
// Unconditionally reinitialise the Revival DLL session to local-play mode.
// Safe to call from the crash handler's VEH after TOCTOU netplay recovery so
// EFZ.exe gets a valid session tick on the next frame instead of the dead
// neutralised vtable.
bool ForceLocalPlayInit();
// Force the game mode index to 0 (title screen) so that the title-screen
// hook runs on the next main-loop iteration.  Returns true on success.
// Safe to call from the VEH crash handler.
bool ForceGameModeToTitle();
// Restore the EFZ title dispatch site for Revival builds that need it during
// recovery.  Returns false for builds where no restore is needed.
bool RestoreRevivalTitleDispatchForRecovery(const char* caller);
// Crash-handler diagnostic accessors - return active Revival profile offsets.
// Returns 0 if no profile is active yet.
uintptr_t GetRevivalRenderContextOffset();
uintptr_t GetRevivalSessionPtrOffset();
void OnTitleSelectionConfirmed(int selection);
NetbridgeStatus GetStatus();
DelayPromptMetrics GetDelayPromptMetrics();
const char* PhaseToString(NetbridgePhase phase);
void BuildStatusLine(const NetbridgeStatus& status, char* buffer, size_t bufferSize);
}
