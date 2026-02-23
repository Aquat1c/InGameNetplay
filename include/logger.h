#pragma once

#include <windows.h>

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle, bool spawnConsole = true);
void ShutdownLogger();
void Log(const char* fmt, ...);
}
