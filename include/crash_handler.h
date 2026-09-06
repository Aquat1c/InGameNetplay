#pragma once

#include <windows.h>

namespace mod
{
void InstallCrashHandlers(HMODULE moduleHandle, bool injectedTakeoverMode);
void UninstallCrashHandlers();
// Reset the one-shot TOCTOU recovery guard so a subsequent session
// can be recovered if needed.  Call this after successfully consuming
// a netplay exit interception (i.e. after ConsumeRevivalExitInterception).
void ResetCrashRecoveryState();
// Re-arm the once-per-process crash-artifact latch so a crash in a LATER
// session (e.g. the 2nd/3rd-session desync under investigation) still writes
// a minidump/text log.  Without this, g_dumpWritten latches on the first
// artifact write and every subsequent session's crash is silent.  Called by
// the centralized session-boundary reset. Idempotent.
void RearmCrashArtifacts();
} // namespace mod

