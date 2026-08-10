#pragma once

#include <cstdint>

namespace netplay::bridge::revival_launch
{
// This policy intentionally contains no process-inspection code.  Startup
// gathers and validates the evidence; this classifier only decides whether it
// is safe to take ownership of the already-created Revival session.
enum class LaunchDisposition : std::uint8_t
{
    DirectGameHost,
    AwaitExternalSession,
    AttachExistingPractice,
    AdoptExternalOnline,
    AdoptExternalSpectator,
    AttachExistingTournament,
    PassiveFailClosed,
};

enum class SessionObjectKind : std::uint8_t
{
    None,
    Practice,
    Online,
    Spectator,
    Tournament,
    Unknown,
};

enum class SessionRole : std::int8_t
{
    Unknown = -1,
    Online = 0,
    Spectator = 1,
    Practice = 2,
    Tournament = 3,
};

struct LaunchEvidence
{
    bool externalParentPresent = false;
    bool exactParentExecutable = false;
    bool revivalDllPresent = false;
    bool exactRevivalDll = false;
    bool supportedProfile = false;
    bool externalBootstrapComplete = false;

    bool sessionPresent = false;
    SessionObjectKind objectKind = SessionObjectKind::None;
    SessionRole role = SessionRole::Unknown;

    // Online and spectator session objects carry a process handle and PID.
    // Both have to bind back to the verified parent before ownership transfer.
    bool parentProcessHandlePresent = false;
    bool sessionPidMatchesParent = false;
};

inline constexpr bool HasExactParentPair(const LaunchEvidence& evidence)
{
    return evidence.externalParentPresent
        && evidence.exactParentExecutable
        && evidence.exactRevivalDll
        && evidence.supportedProfile;
}

inline constexpr bool HasExactParentBinding(const LaunchEvidence& evidence)
{
    return evidence.parentProcessHandlePresent
        && evidence.sessionPidMatchesParent;
}

inline constexpr bool HasContradictoryEvidence(
    const LaunchEvidence& evidence)
{
    if (!evidence.externalParentPresent
        && (evidence.exactParentExecutable
            || evidence.parentProcessHandlePresent
            || evidence.sessionPidMatchesParent))
    {
        return true;
    }

    if (!evidence.revivalDllPresent
        && (evidence.exactRevivalDll
            || evidence.supportedProfile
            || evidence.externalBootstrapComplete
            || evidence.sessionPresent))
    {
        return true;
    }

    if (!evidence.sessionPresent)
    {
        return evidence.objectKind != SessionObjectKind::None
            || evidence.parentProcessHandlePresent
            || evidence.sessionPidMatchesParent;
    }

    return evidence.objectKind == SessionObjectKind::None
        || evidence.role == SessionRole::Unknown;
}

inline constexpr LaunchDisposition ClassifyLaunch(
    const LaunchEvidence& evidence)
{
    if (HasContradictoryEvidence(evidence))
    {
        return LaunchDisposition::PassiveFailClosed;
    }

    if (!evidence.externalParentPresent && !evidence.revivalDllPresent)
    {
        return LaunchDisposition::DirectGameHost;
    }

    // Local Play and Tournament launchers resume EFZ and then exit.  Their
    // process may be gone before the mod worker is scheduled, but the exact
    // already-initialized DLL and its canonical role/object remain conclusive
    // evidence that a second exported init would be destructive.  Practice
    // can be reused without a second init. Tournament is attached to its
    // already-created native object so the mod can own title/exit recovery.
    if (!evidence.externalParentPresent)
    {
        if (!evidence.exactRevivalDll
            || !evidence.supportedProfile
            || !evidence.externalBootstrapComplete)
        {
            return LaunchDisposition::PassiveFailClosed;
        }

        if (!evidence.sessionPresent)
        {
            if (evidence.role == SessionRole::Practice)
            {
                return LaunchDisposition::AttachExistingPractice;
            }
            return LaunchDisposition::PassiveFailClosed;
        }

        if (evidence.objectKind == SessionObjectKind::Practice
            && evidence.role == SessionRole::Practice)
        {
            return LaunchDisposition::AttachExistingPractice;
        }
        if (evidence.objectKind == SessionObjectKind::Tournament
            && evidence.role == SessionRole::Tournament)
        {
            return LaunchDisposition::AttachExistingTournament;
        }
        return LaunchDisposition::PassiveFailClosed;
    }

    if (!HasExactParentPair(evidence))
    {
        return LaunchDisposition::PassiveFailClosed;
    }

    if (!evidence.externalBootstrapComplete)
    {
        return evidence.sessionPresent
            ? LaunchDisposition::PassiveFailClosed
            : LaunchDisposition::AwaitExternalSession;
    }

    // Revival's local-play init sets canonical role 2 before a Practice object
    // exists.  At the title screen that null session pointer is a stable,
    // fully-bootstrapped state rather than evidence that init is unfinished.
    if (!evidence.sessionPresent)
    {
        return evidence.role == SessionRole::Practice
            ? LaunchDisposition::AttachExistingPractice
            : LaunchDisposition::PassiveFailClosed;
    }

    switch (evidence.objectKind)
    {
    case SessionObjectKind::Practice:
        return evidence.role == SessionRole::Practice
            ? LaunchDisposition::AttachExistingPractice
            : LaunchDisposition::PassiveFailClosed;

    case SessionObjectKind::Online:
        return evidence.role == SessionRole::Online
                && HasExactParentBinding(evidence)
            ? LaunchDisposition::AdoptExternalOnline
            : LaunchDisposition::PassiveFailClosed;

    case SessionObjectKind::Spectator:
        return evidence.role == SessionRole::Spectator
                && HasExactParentBinding(evidence)
            ? LaunchDisposition::AdoptExternalSpectator
            : LaunchDisposition::PassiveFailClosed;

    case SessionObjectKind::Tournament:
        return evidence.role == SessionRole::Tournament
            ? LaunchDisposition::AttachExistingTournament
            : LaunchDisposition::PassiveFailClosed;

    case SessionObjectKind::None:
    case SessionObjectKind::Unknown:
        return LaunchDisposition::PassiveFailClosed;
    }

    return LaunchDisposition::PassiveFailClosed;
}
} // namespace netplay::bridge::revival_launch
