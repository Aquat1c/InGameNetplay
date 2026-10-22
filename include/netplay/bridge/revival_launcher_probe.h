#pragma once

#include "netplay/bridge/revival_launch_policy.h"

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace netplay::bridge::takeover
{
struct RevivalAddressProfile;

namespace launcher_probe_detail
{
constexpr size_t kTournamentAutoNavInputCount = 22u;

enum class TournamentAutoNavSuffixState
{
    Invalid,
    Retry,
    Ready,
};

// An empty queue at game mode 0 (title) is the native ExitProcess trigger: it must remain Retry, never Ready.
inline TournamentAutoNavSuffixState ClassifyTournamentAutoNavSuffix(
    const uint16_t* values,
    size_t count,
    int gameMode)
{
    static constexpr uint16_t kCanonicalInputs[
        kTournamentAutoNavInputCount] = {
        0u, 0u, 0u, 0u, 0u, 0u, 4u, 0u, 0u, 0u, 0u,
        4u, 0u, 0u, 0u, 0u, 16u, 0u, 0u, 0u, 0u, 0u,
    };

    if (count > kTournamentAutoNavInputCount
        || (count != 0u && values == nullptr))
    {
        return TournamentAutoNavSuffixState::Invalid;
    }
    if (count == 0u)
    {
        return gameMode == 0
            ? TournamentAutoNavSuffixState::Retry
            : TournamentAutoNavSuffixState::Ready;
    }

    const size_t suffixStart = kTournamentAutoNavInputCount - count;
    for (size_t i = 0; i < count; ++i)
    {
        if (values[i] != kCanonicalInputs[suffixStart + i])
        {
            return TournamentAutoNavSuffixState::Invalid;
        }
    }
    return TournamentAutoNavSuffixState::Ready;
}
}

// parentProcess is owned by the probe result; close it via ReleaseRevivalLauncherProbe unless ownership is explicitly transferred.
struct RevivalLauncherProbe
{
    revival_launch::LaunchDisposition disposition =
        revival_launch::LaunchDisposition::PassiveFailClosed;
    const RevivalAddressProfile* profile = nullptr;
    HANDLE parentProcess = nullptr;
    DWORD parentPid = 0;
    FILETIME parentCreationTime = {};
    // Exact sample taken before the parent guard; revalidation requires value-for-value equality, not equivalence.
    uintptr_t revivalDllBase = 0;
    uintptr_t sessionPtr = 0;
    uintptr_t sessionVtable = 0;
    uintptr_t helperHandleValue = 0;
    int helperPid = -1;
    uint32_t readinessValue = 0;
    bool readinessSatisfied = false;
    int role = -1;
    int activePlayer = -1;
    bool exactExternalParent = false;
};

// The bounded wait runs only on the mod initialization worker, never on EFZ's loader or simulation thread.
RevivalLauncherProbe ProbeRevivalLauncherStartup(DWORD timeoutMs);

bool RevalidateRevivalLauncherStartup(const RevivalLauncherProbe& probe);
// Final in-memory validation while the game thread is parked at the attach boundary; performs no disk I/O.
bool RevalidateRevivalLauncherSessionSnapshot(
    const RevivalLauncherProbe& probe);
void ReleaseRevivalLauncherProbe(RevivalLauncherProbe* probe);
}
