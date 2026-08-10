#include "netplay/bridge/session_lifecycle.h"

#include "netplay/bridge/frontend_return.h"

#include "crash_handler.h"
#include "logger.h"

#include <windows.h>

namespace netplay::bridge::session_lifecycle
{
namespace
{
volatile LONG g_sessionEpoch = 0;
volatile LONG g_cleanupInvocation = 0;
volatile LONG g_lastTornDownEpoch = 0;
} // namespace

uint32_t CurrentSessionEpoch()
{
    return static_cast<uint32_t>(InterlockedCompareExchange(&g_sessionEpoch, 0, 0));
}

uint32_t NextCleanupInvocation()
{
    return static_cast<uint32_t>(InterlockedIncrement(&g_cleanupInvocation));
}

uint32_t LastTornDownEpoch()
{
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_lastTornDownEpoch, 0, 0));
}

void NoteTeardownComplete(uint32_t epoch)
{
    InterlockedExchange(&g_lastTornDownEpoch, static_cast<LONG>(epoch));
}

uint32_t BeginSessionBoundary(const char* reason)
{
    const uint32_t epoch =
        static_cast<uint32_t>(InterlockedIncrement(&g_sessionEpoch));
    const char* const why = (reason != nullptr) ? reason : "unknown";
    (void)why;  // referenced only by the (release-compiled-out) trace macro

    MOD_LIFECYCLE_TRACE(
        "LIFECYCLE ep=%u stage=session_boundary action=begin reason=%s",
        epoch, why);

    // --- Centralized reset of cross-session mod-owned latches. ---
    // The individual resets are idempotent, but this function is not: every
    // call advances the epoch. The managed-session authority must invoke it
    // exactly once per committed boundary.

    // Rank 1: legacy gameplay-exit cleanup suppression latch.  Left armed, it
    // suppresses teardown for the whole next session on whichever peer returned
    // to the netplay menu - an asymmetry that survives into match N+1.
    frontend_return::ResetConsumedContinuationLatch();
    MOD_LIFECYCLE_TRACE(
        "LIFECYCLE ep=%u stage=reset_continuation_latch action=complete", epoch);

    // Re-arm the once-per-process crash-artifact latch so a crash/desync in
    // THIS (later) session still writes a minidump - the 2nd/3rd-session failure
    // being investigated would otherwise leave no artifact.
    mod::RearmCrashArtifacts();
    MOD_LIFECYCLE_TRACE(
        "LIFECYCLE ep=%u stage=rearm_crash_artifacts action=complete", epoch);

    MOD_LIFECYCLE_TRACE(
        "LIFECYCLE ep=%u stage=session_boundary action=complete reason=%s",
        epoch, why);
    return epoch;
}
} // namespace netplay::bridge::session_lifecycle
