#pragma once

#include "netplay/bridge/revival_addresses.h"

#include <windows.h>

namespace netplay::bridge::takeover
{
// Installs the narrow EfzRevival-parent survival guard used by an externally
// launched, mod-owned session.  The caller retains ownership of parentProcess.
// The process identity, launcher image, and one exact cleanup call site are
// revalidated on both sides before the parent's sole IAT patch is activated.
bool InstallExternalLauncherGuard(
    HANDLE parentProcess,
    DWORD parentProcessId,
    const FILETIME& parentCreationTime,
    const RevivalAddressProfile& profile,
    uintptr_t* outRemoteBase = nullptr);

// Read-only child-side liveness query.  True means the marker is still ACTIVE
// and its exact parent PID generation is alive.  It never changes guard state
// and does not take ownership of a process handle.
bool HasActiveExactExternalLauncherParent();

// Ends delivery for the current managed-session generation without weakening
// the parent-side TerminateProcess protection.  Late blocked-call serials stay
// suppressed until a future guard installation creates a new generation.
void RetireExternalLauncherGuardSignalDelivery();

// Title/control-plane only (never the rollback/per-frame path). Releases the
// retired guard only after its retained exact parent process has exited or no
// longer matches the admitted PID generation. Returns true when child-side
// guard state was reaped.
bool ReapExternalLauncherGuardIfParentExited();

// A blocked cleanup TerminateProcess call is published by the launcher into
// the shared marker.  Has is read-only; Consume atomically advances the local
// observed generation and returns true exactly once per newly observed state.
bool HasExternalLauncherTerminateBlockedSignal();
bool ConsumeExternalLauncherTerminateBlockedSignal();

// Releases the child-side marker/event views and retained exact-parent handle.
// This never terminates or otherwise mutates the external launcher process.
void CleanupExternalLauncherGuard();

// DllMain-only external-launcher path.  A true result means the current
// EfzRevival process was injected for the narrow guard and must not enter the
// ordinary injected-helper bootstrap, even if validation later fails.
bool TryStartExternalLauncherGuardProcess();
bool IsExternalLauncherGuardProcess();
bool GetExternalLauncherGuardChildProcessId(
    DWORD* outChildProcessId,
    uint32_t* outRevivalDllTimestamp = nullptr);
void ShutdownExternalLauncherGuardProcess();

enum class ExternalLauncherTerminateDecision
{
    NotGuardProcess,
    PassThrough,
    Block,
};

// Evaluated only at the very beginning of nb_stub_TerminateProcess.  The
// return address must be captured by that stub itself (not by a helper).
ExternalLauncherTerminateDecision EvaluateExternalLauncherTerminate(
    HANDLE process,
    const void* callerReturnAddress);
} // namespace netplay::bridge::takeover
