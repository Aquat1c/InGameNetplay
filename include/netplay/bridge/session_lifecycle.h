#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// Centralized netplay session lifecycle authority.
//
// The mod's post-match / post-session cleanup was historically scattered across
// many exit paths (normal match end, rematch, char-select return, menu return,
// ESC quit, disconnect recovery, WM_CLOSE, tournament return), several of which
// skipped, reordered, or double-ran individual resets.  Cross-session latches
// that leaked on one peer but not the other are a prime driver of the
// session-2/session-3 desync.
//
// This module provides the boundary authority used by direct sessions and
// exact launcher-first adoption:
//   * a monotonic session EPOCH, bumped exactly once per session start at the
//     corresponding managed-session commit point, used both as a
//     stable identifier for aligning the two peers' lifecycle traces and as the
//     idempotency key that collapses repeated cleanup calls within one boundary;
//   * BeginSessionBoundary(), the centralized reset of the mod's
//     cross-session latches, so match N+1 never inherits match N's state
//     regardless of how messy match N's exit path was.
//
// Resetting at the START of the next session (rather than at each exit path) is
// deliberate: the exit paths are asymmetric and some are suppressed, which is
// the bug.  The single unconditional entry point guarantees a clean slate.
// ---------------------------------------------------------------------------

namespace netplay::bridge::session_lifecycle
{
// The current session-boundary epoch.  Starts at 0 (no session yet); the first
// BeginSessionBoundary() returns 1.  Monotonic for the process lifetime.
uint32_t CurrentSessionEpoch();

// Runs exactly once at the start/commit of every managed session, under the
// takeover mutex. Bumps the epoch and resets the mod's cross-session latches:
//   * frontend_return consumed-continuation suppression latch (rank 1);
//   * once-per-process crash-artifact latch (so later-session crashes dump);
//   * (extend here as further cross-session latches are centralized).
// Returns the new epoch. Calling it more than once creates a new epoch and is
// therefore not safe within one session boundary.
uint32_t BeginSessionBoundary(const char* reason);

// Monotonic cleanup-invocation counter, for the "repeated cleanup calls" proof
// in lifecycle traces: each teardown entry stamps a fresh id so a boundary that
// runs cleanup twice is visible as two invocations against one epoch.
uint32_t NextCleanupInvocation();

// The epoch whose teardown has already fully run, recorded by the teardown
// path so a second cancel within the same boundary can be recognized as a
// repeat (idempotency).  0 until the first teardown completes.
uint32_t LastTornDownEpoch();
void NoteTeardownComplete(uint32_t epoch);
}
