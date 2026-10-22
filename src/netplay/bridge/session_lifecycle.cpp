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
    (void)why;

    MOD_LIFECYCLE_TRACE(
        "LIFECYCLE ep=%u stage=session_boundary action=begin reason=%s",
        epoch, why);

    // Centralized reset of cross-session mod-owned latches. Each call
    // advances the epoch; invoke exactly once per committed boundary.

    // Gameplay-exit cleanup suppression latch: left armed, it suppresses the
    // whole next session's teardown on whichever peer returned to the menu.
    frontend_return::ResetConsumedContinuationLatch();
    MOD_LIFECYCLE_TRACE(
        "LIFECYCLE ep=%u stage=reset_continuation_latch action=complete", epoch);

    mod::RearmCrashArtifacts();
    MOD_LIFECYCLE_TRACE(
        "LIFECYCLE ep=%u stage=rearm_crash_artifacts action=complete", epoch);

    MOD_LIFECYCLE_TRACE(
        "LIFECYCLE ep=%u stage=session_boundary action=complete reason=%s",
        epoch, why);
    return epoch;
}
} // namespace netplay::bridge::session_lifecycle
