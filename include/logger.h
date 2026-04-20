#pragma once

#include <windows.h>

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle, bool spawnConsole = false, bool writeLogFile = true);
void ShutdownLogger();
void SetConsoleVisible(bool visible);
void SetFileLoggingEnabled(HMODULE moduleHandle, bool enabled);
void Log(const char* fmt, ...);

// Synchronously drains the async writer's queue to the log file/console.
// Safe to call from the crash handler before writing a minidump so the
// tail of Log() output lands on disk before the process dies.
void FlushLoggerSync();
}
