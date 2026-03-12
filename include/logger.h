#pragma once

#include <windows.h>

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle, bool spawnConsole = false, bool writeLogFile = true);
void ShutdownLogger();
void SetConsoleVisible(bool visible);
void SetFileLoggingEnabled(HMODULE moduleHandle, bool enabled);
void Log(const char* fmt, ...);
}
