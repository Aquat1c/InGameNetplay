#pragma once

#include <windows.h>

namespace netplay::bridge::takeover
{
bool InstallHelperDelayDisconnectHandler(HMODULE helperModule, const char* versionTag);
}
