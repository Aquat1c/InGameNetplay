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

struct HostSessionNetworkConfig
{
    network::NetworkFamily preferredFamily = network::NetworkFamily::IPv4;
    network::NetworkFamily effectiveFamily = network::NetworkFamily::IPv4;
    std::string publicAddress;
    bool automaticFamilyRetryAttempted = false;
};

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

enum class HostStartFailure : uint8_t
{
    None = 0,
    InvalidRequest,
    FamilyUnavailable,
    StartAlreadyInProgress,
};

// Parsed from the Revival console line "Hosting using UDP IPv4|IPv6 on port N".
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
bool Initialize();
bool CompleteLauncherUiAttachment(bool hooksInstalled);
void EmergencyQuarantineLauncherUiAttachment();
void Shutdown();
void EmergencyShutdown();
void InitializeInjectedProcess();
void ShutdownInjectedProcess();
void Tick();
// Must not be polled by the rollback-frame hook.
void TickExportOnly(bool force);
bool StartSession(
    NetbridgeRole role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni = true);
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
// Recursive lock held until EndOptionsIniAccess(); write access is rejected during a Host Protocol override.
bool BeginOptionsIniAccess(
    bool writeAccess,
    HostProtocolOverrideState* outState);
void EndOptionsIniAccess();
bool ApplyInputDelay(int delayFrames);
bool AnswerSpectatePromptChoice(int choice);
bool PrepareVsHumanHandoff();
bool RequiresNativeVsHumanSyncForHandoff();
// Safe from game/window callbacks; never under loader lock.
bool RequestPeerQuitBeforeLocalExit(const char* reason);
void CancelSession(const char* reason);
bool ConsumeRevivalExitInterception(int* outMode);
void CompleteGameplayExitRecovery(int mode, const char* origin);
bool NotifyTitleScreenActive();
bool CompletePendingTournamentReturnCleanup();
// True if the peer EfzRevival.exe process is still running. Advisory (TOCTOU).
bool IsPeerProcessAlive();
bool IsNetplayExitInterceptionPending();
// Callable from the crash handler's VEH.
bool ForceLocalPlayInit();
bool ForceGameModeToTitle();
bool RestoreRevivalTitleDispatchForRecovery(const char* caller);
uintptr_t GetRevivalRenderContextOffset();
uintptr_t GetRevivalSessionPtrOffset();
void OnTitleSelectionConfirmed(int selection);
NetbridgeStatus GetStatus();
DelayPromptMetrics GetDelayPromptMetrics();
const char* PhaseToString(NetbridgePhase phase);
void BuildStatusLine(const NetbridgeStatus& status, char* buffer, size_t bufferSize);
}
