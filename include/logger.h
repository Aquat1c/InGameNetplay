#pragma once

#include <windows.h>

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle, bool spawnConsole = false, bool writeLogFile = true);
void ShutdownLogger();
void SetConsoleVisible(bool visible);
void SetFileLoggingEnabled(HMODULE moduleHandle, bool enabled);
void Log(const char* fmt, ...);

void FlushLoggerSync();

#if defined(EFZ_LIFECYCLE_TRACE)
bool IsLifecycleTraceEnabled();
void SetLifecycleTraceEnabled(bool enabled);
#endif
}

#if defined(EFZ_LIFECYCLE_TRACE)
#define MOD_LIFECYCLE_TRACE(...) \
    do { if (::mod::IsLifecycleTraceEnabled()) ::mod::Log(__VA_ARGS__); } while (0)
#define MOD_LIFECYCLE_TRACE_COMPILED 1
#else
#define MOD_LIFECYCLE_TRACE(...) ((void)0)
#define MOD_LIFECYCLE_TRACE_COMPILED 0
#endif
