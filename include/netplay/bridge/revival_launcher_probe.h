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

// Pure policy half of the Tournament queue witness.  The native queue is
// consumed from the front, so any remaining entries must be an exact suffix
// of the constructor's 22-input sequence.  An empty queue is safe only after
// EFZ has left game mode 0; at the title it is the native ExitProcess trigger
// and must remain a retryable (not admitted) state.
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

// Immutable result of the one-time launcher/startup admission probe.  The
// parentProcess handle is owned by the result and must be closed by
// ReleaseRevivalLauncherProbe unless ownership is explicitly transferred.
struct RevivalLauncherProbe
{
    revival_launch::LaunchDisposition disposition =
        revival_launch::LaunchDisposition::PassiveFailClosed;
    const RevivalAddressProfile* profile = nullptr;
    HANDLE parentProcess = nullptr;
    DWORD parentPid = 0;
    FILETIME parentCreationTime = {};
    // Exact mutable sample admitted before the parent guard is installed.
    // Revalidation requires byte-for-byte/value-for-value equality with this
    // sample; a merely equivalent replacement object is not accepted.
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

// Resolves direct-game startup versus a session created by an exact supported
// EfzRevival.exe parent.  The bounded wait runs only on the mod initialization
// worker, never on EFZ's loader or simulation thread.
RevivalLauncherProbe ProbeRevivalLauncherStartup(DWORD timeoutMs);

// Re-observes every mutable admission field immediately before an existing
// Revival object is adopted.  This closes the gap between the initial probe
// and the external-parent guard handshake: a session that disconnected,
// changed role, or was replaced while the guard was being installed is never
// dereferenced or hooked using the stale probe result.
bool RevalidateRevivalLauncherStartup(const RevivalLauncherProbe& probe);
// Final in-memory validation used while the game thread is parked at the
// attach boundary. Exact DLL/launcher file hashes were already verified by
// RevalidateRevivalLauncherStartup before publication; this variant performs
// no disk I/O.
bool RevalidateRevivalLauncherSessionSnapshot(
    const RevivalLauncherProbe& probe);
void ReleaseRevivalLauncherProbe(RevivalLauncherProbe* probe);
}
