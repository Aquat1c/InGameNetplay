#pragma once

#include "netplay/bridge/session_bridge.h"

namespace netplay::bridge::takeover
{
bool IsCurrentProcessRevival();
void InitializeHost();
void ShutdownHost();
void EmergencyShutdownHost();
void InitializeInjected();
void ShutdownInjected();
void RequestAbortStart();
bool ApplyInputDelay(int delayFrames, NetbridgeStatus* ioStatus);
bool AnswerSpectateConfirm(bool acceptSpectate, NetbridgeStatus* ioStatus);
bool PrepareVsHumanHandoff(NetbridgeStatus* ioStatus);
void OnTitleSelectionConfirmed(int selection, NetbridgeStatus* ioStatus);
bool StartSession(NetbridgeRole role, uint16_t port, const char* address, const char* nickname, NetbridgeStatus* ioStatus, uint32_t* outConnectStartTick);
void Tick(NetbridgeStatus* ioStatus, uint32_t* ioConnectStartTick);
void CancelSession(const char* reason, NetbridgeStatus* ioStatus);
bool ConsumeRevivalExitInterception(int* outMode, NetbridgeStatus* ioStatus);
DelayPromptMetrics GetDelayPromptMetrics();
}
