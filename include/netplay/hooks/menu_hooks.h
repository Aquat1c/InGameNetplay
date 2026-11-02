#pragma once

#include <windows.h>

namespace netplay
{
bool InstallHooks();
void RemoveHooks();
bool AreHooksInstalled();
void PrepareForProcessExit(bool emergency);

bool PrepareExternalLauncherSimulationHandoff();

void ShowInProgressMessage(HWND owner);
}
