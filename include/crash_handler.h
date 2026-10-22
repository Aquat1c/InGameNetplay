#pragma once

#include <windows.h>

namespace mod
{
void InstallCrashHandlers(HMODULE moduleHandle, bool injectedTakeoverMode);
void UninstallCrashHandlers();
void ResetCrashRecoveryState();
void RearmCrashArtifacts();
} // namespace mod

