#pragma once

#include <windows.h>

namespace mod
{
void InstallCrashHandlers(HMODULE moduleHandle, bool injectedTakeoverMode);
void UninstallCrashHandlers();
// Reset the one-shot TOCTOU recovery guard so a subsequent session
// can be recovered if needed.  Call this after successfully consuming
// a netplay exit interception (i.e. after ConsumeRevivalExitInterception).
void ResetCrashRecoveryState();
} // namespace mod

