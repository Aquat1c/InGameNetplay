#pragma once

#include "netplay/bridge/revival_addresses.h"

#include <windows.h>

namespace netplay::bridge::takeover
{
bool InstallExternalLauncherGuard(
    HANDLE parentProcess,
    DWORD parentProcessId,
    const FILETIME& parentCreationTime,
    const RevivalAddressProfile& profile,
    uintptr_t* outRemoteBase = nullptr);

bool HasActiveExactExternalLauncherParent();

void RetireExternalLauncherGuardSignalDelivery();

bool ReapExternalLauncherGuardIfParentExited();

bool HasExternalLauncherTerminateBlockedSignal();
bool ConsumeExternalLauncherTerminateBlockedSignal();

void CleanupExternalLauncherGuard();

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

// Evaluated only at the very beginning of nb_stub_TerminateProcess; the return address must be captured by that stub itself (not by a helper).
ExternalLauncherTerminateDecision EvaluateExternalLauncherTerminate(
    HANDLE process,
    const void* callerReturnAddress);
} // namespace netplay::bridge::takeover
