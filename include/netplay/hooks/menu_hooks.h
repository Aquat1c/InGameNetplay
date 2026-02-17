#pragma once

#include <windows.h>

namespace netplay
{
bool InstallHooks();
void RemoveHooks();
bool AreHooksInstalled();

void ShowInProgressMessage(HWND owner);
}

