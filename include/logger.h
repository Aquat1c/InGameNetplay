#pragma once

#include <windows.h>

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle, bool spawnConsole = false);
void ShutdownLogger();
void SetConsoleVisible(bool visible);
void Log(const char* fmt, ...);
}
