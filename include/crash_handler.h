#pragma once

#include <windows.h>

namespace mod
{
void InstallCrashHandlers(HMODULE moduleHandle, bool injectedTakeoverMode);
void UninstallCrashHandlers();
} // namespace mod

