#pragma once

#include "netplay/bridge/session_bridge.h"

namespace netplay::bridge::takeover
{
bool IsCurrentProcessRevival();
void DetectRevivalVersion();
void InitializeHost();
void ShutdownHost();
void EmergencyShutdownHost();
void InitializeInjected();
void ShutdownInjected();
void RequestAbortStart();
bool ApplyInputDelay(int delayFrames, NetbridgeStatus* ioStatus);
bool AnswerSpectatePromptChoice(int choice, NetbridgeStatus* ioStatus);
bool PrepareVsHumanHandoff(NetbridgeStatus* ioStatus);
void OnTitleSelectionConfirmed(int selection, NetbridgeStatus* ioStatus);
bool StartSession(
    NetbridgeRole role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni,
    NetbridgeStatus* ioStatus,
    uint32_t* outConnectStartTick);
void Tick(NetbridgeStatus* ioStatus, uint32_t* ioConnectStartTick);
void CancelSession(const char* reason, NetbridgeStatus* ioStatus);
bool ConsumeRevivalExitInterception(int* outMode, NetbridgeStatus* ioStatus);
bool NotifyTitleScreenActive(NetbridgeStatus* ioStatus);
bool CompletePendingTournamentReturnCleanup(NetbridgeStatus* ioStatus);
DelayPromptMetrics GetDelayPromptMetrics();
bool IsPeerProcessAlive();
bool IsNetplayExitInterceptionPending();
bool ForceLocalPlayInit();
bool ForceGameModeToTitle();
}
