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

#if defined(EFZ_LIFECYCLE_TRACE)
// Runtime toggle for the compiled-in lifecycle/investigation tracing.
// Defaults to DISABLED even in trace builds; an investigation enables it
// explicitly via [Others] LifecycleTrace=1 (read in mod_settings::Reload).
bool IsLifecycleTraceEnabled();
void SetLifecycleTraceEnabled(bool enabled);
#endif
}

// Lifecycle/investigation trace sink. In EFZ_LIFECYCLE_TRACE builds
// (xp-trace preset) this forwards to mod::Log when the runtime toggle is on;
// in end-user builds the macro compiles to nothing - no format strings in the
// binary, no argument evaluation, zero cost. Route ALL detailed diagnostic
// logging (per-frame state, pointers/addresses, sync counters, memory dumps)
// through this macro, never through mod::Log directly.
#if defined(EFZ_LIFECYCLE_TRACE)
#define MOD_LIFECYCLE_TRACE(...) \
    do { if (::mod::IsLifecycleTraceEnabled()) ::mod::Log(__VA_ARGS__); } while (0)
#define MOD_LIFECYCLE_TRACE_COMPILED 1
#else
#define MOD_LIFECYCLE_TRACE(...) ((void)0)
#define MOD_LIFECYCLE_TRACE_COMPILED 0
#endif
