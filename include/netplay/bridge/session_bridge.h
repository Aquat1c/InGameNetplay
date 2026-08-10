#pragma once

#include "netplay/core/network_endpoint.h"

#include <cstddef>
#include <cstdint>
#include <string>

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

// Immutable network selection for one explicit Host attempt. The title/lobby
// preflight owns public-address discovery; the bridge only validates and
// snapshots its result so helper startup and async rehosts cannot drift if the
// INI/options change while the Revival netplay session is active.
struct HostSessionNetworkConfig
{
    network::NetworkFamily preferredFamily = network::NetworkFamily::IPv4;
    network::NetworkFamily effectiveFamily = network::NetworkFamily::IPv4;
    std::string publicAddress;
    bool automaticFamilyRetryAttempted = false;
};

// Snapshot of the short-lived EfzRevival.ini Protocol override used while a
// Host helper starts. originalFamily is the user's next-session preference
// (invalid/missing values normalize to IPv4); effectiveFamily is the family
// temporarily written for the active helper attempt.
struct HostProtocolOverrideState
{
    bool active = false;
    bool originalValueExisted = false;
    network::NetworkFamily originalFamily =
        network::NetworkFamily::IPv4;
    network::NetworkFamily effectiveFamily =
        network::NetworkFamily::IPv4;
    std::string originalValue;
};

// Synchronous result classification for the explicit Host preflight. A
// queued worker reports later native failures through NetbridgeStatus; only
// FamilyUnavailable is safe for the frontend to retry on the other family.
enum class HostStartFailure : uint8_t
{
    None = 0,
    InvalidRequest,
    FamilyUnavailable,
    StartAlreadyInProgress,
};

// Native EfzRevival.exe acknowledgement parsed from:
//   Hosting using UDP IPv4|IPv6 on port N
// This is an internal C++ status seam, deliberately separate from
// NetbridgeStatus so the existing exported by-value ABI remains unchanged.
struct HostListenerObservation
{
    bool available = false;
    bool expectedFamilyKnown = false;
    bool familyMatches = false;
    bool portMatches = false;
    bool expectedProcessKnown = false;
    bool processMatches = false;
    uint32_t serial = 0;
    uint32_t processId = 0;
    network::NetworkFamily family = network::NetworkFamily::IPv4;
    uint16_t port = 0;
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
    int sessionScoresValid = 0; // both win counters were read from this role's verified layout
    int sessionNamesValid = 0;  // both names were read and validated from this role's verified layout
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
// Publish a runtime snapshot without calling takeover::Tick(). Transition
// owners use force=true; this must not be polled by the rollback-frame hook.
void TickExportOnly(bool force);
bool StartSession(
    NetbridgeRole role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni = true);
// Explicit Host entry point used after family-specific public-address
// preflight. Unlike legacy StartSession(Host, ...), this temporarily writes the
// effective Protocol for the helper, then restores the user's preferred INI
// value after the native listener acknowledgement (or on failure/cancel).
bool StartHostSession(
    uint16_t port,
    const char* nickname,
    const HostSessionNetworkConfig& networkConfig,
    bool writeNicknameToIni = true,
    HostStartFailure* outFailure = nullptr);
bool GetActiveHostSessionNetworkConfig(HostSessionNetworkConfig* outConfig);
bool GetHostListenerObservation(HostListenerObservation* outObservation);
bool IsSessionStartInProgress();
bool GetHostProtocolOverrideState(HostProtocolOverrideState* outState);
// Serializes the Options reader/writer with bridge-owned EfzRevival.ini
// updates. A successful call keeps an internal recursive lock held until the
// matching EndOptionsIniAccess(). Write access is rejected (with no lock held)
// while a temporary Host Protocol override is active.
bool BeginOptionsIniAccess(
    bool writeAccess,
    HostProtocolOverrideState* outState);
void EndOptionsIniAccess();
bool ApplyInputDelay(int delayFrames);
bool AnswerSpectatePromptChoice(int choice);
bool PrepareVsHumanHandoff();
bool RequiresNativeVsHumanSyncForHandoff();
// Send Revival's native MessageQuit before a local UI/process exit tears down
// the helper. Returns true when the helper completed the native broadcast.
// Safe to call from normal game/window callbacks; do not call under loader lock.
bool RequestPeerQuitBeforeLocalExit(const char* reason);
// A synchronous validation/family rejection is acknowledged here without
// invoking Revival teardown because no start worker or helper was created.
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
// Restore the Revival-owned EFZ title dispatch hook when recovery disturbed
// it. Returns false for builds where no restore is needed.
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
