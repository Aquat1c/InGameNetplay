#pragma once

#include <windows.h>

namespace netplay
{
bool InstallHooks();
void RemoveHooks();
bool AreHooksInstalled();
void PrepareForProcessExit(bool emergency);

// Launcher-first Online/Spectator sessions are already native-live when the
// mod attaches.  Called after title hooks are installed while the adopted
// frame boundary is still parked, so recurring UI/export work is quiesced
// before the first native title update is released.
bool PrepareExternalLauncherSimulationHandoff();

void ShowInProgressMessage(HWND owner);
}
