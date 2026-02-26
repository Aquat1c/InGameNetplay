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
    int localInitApplied = 0;
    int delaySetupReady = 0;
    int vsHumanSyncReady = 0;
    uint16_t port = 0;
    char address[64] = {};
    char nickname[32] = {};
    char p1Name[64] = {};
    char p2Name[64] = {};
    char errorMsg[128] = {};
    uint32_t phaseTick = 0;
    uint32_t processId = 0;
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
void Initialize();
void Shutdown();
void EmergencyShutdown();
void InitializeInjectedProcess();
void ShutdownInjectedProcess();
void Tick();
bool StartSession(NetbridgeRole role, uint16_t port, const char* address, const char* nickname);
bool ApplyInputDelay(int delayFrames);
bool AnswerSpectateConfirm(bool acceptSpectate);
bool PrepareVsHumanHandoff();
void CancelSession(const char* reason);
bool ConsumeRevivalExitInterception(int* outMode);
bool NotifyTitleScreenActive();
void OnTitleSelectionConfirmed(int selection);
NetbridgeStatus GetStatus();
DelayPromptMetrics GetDelayPromptMetrics();
const char* PhaseToString(NetbridgePhase phase);
void BuildStatusLine(const NetbridgeStatus& status, char* buffer, size_t bufferSize);
}
