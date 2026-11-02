#pragma once

#include <cstdint>

namespace netplay::bridge::session_lifecycle
{
uint32_t CurrentSessionEpoch();

// Exactly once per session, under the takeover mutex; a second call within one boundary is unsafe.
uint32_t BeginSessionBoundary(const char* reason);

uint32_t NextCleanupInvocation();

uint32_t LastTornDownEpoch();
void NoteTeardownComplete(uint32_t epoch);
}
