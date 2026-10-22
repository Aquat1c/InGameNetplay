#pragma once

#include <cstdint>

namespace netplay::bridge::session_lifecycle
{
// 0 until the first BeginSessionBoundary(); monotonic for the process lifetime.
uint32_t CurrentSessionEpoch();

// Runs once at the start of every managed session, under the takeover mutex;
// bumps the epoch and resets the cross-session latches. A second call within
// one boundary is unsafe.
uint32_t BeginSessionBoundary(const char* reason);

// Monotonic counter making a double-cleanup boundary visible in lifecycle traces.
uint32_t NextCleanupInvocation();

// Epoch whose teardown fully ran; 0 until the first teardown. Repeat cancels
// within the same boundary are idempotent.
uint32_t LastTornDownEpoch();
void NoteTeardownComplete(uint32_t epoch);
}
