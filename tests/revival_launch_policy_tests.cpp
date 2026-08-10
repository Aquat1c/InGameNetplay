#include "netplay/bridge/revival_launch_policy.h"
#include "netplay/bridge/revival_addresses.h"
#include "netplay/bridge/revival_launcher_probe.h"

#include <array>
#include <cstring>
#include <iostream>

namespace
{
using netplay::bridge::revival_launch::ClassifyLaunch;
using netplay::bridge::revival_launch::LaunchDisposition;
using netplay::bridge::revival_launch::LaunchEvidence;
using netplay::bridge::revival_launch::SessionObjectKind;
using netplay::bridge::revival_launch::SessionRole;

struct TestCase
{
    const char* name;
    LaunchEvidence evidence;
    LaunchDisposition expected;
};

constexpr LaunchEvidence VerifiedParent()
{
    LaunchEvidence evidence = {};
    evidence.externalParentPresent = true;
    evidence.exactParentExecutable = true;
    evidence.revivalDllPresent = true;
    evidence.exactRevivalDll = true;
    evidence.supportedProfile = true;
    return evidence;
}

constexpr LaunchEvidence Session(
    SessionObjectKind objectKind,
    SessionRole role)
{
    LaunchEvidence evidence = VerifiedParent();
    evidence.externalBootstrapComplete = true;
    evidence.sessionPresent = true;
    evidence.objectKind = objectKind;
    evidence.role = role;
    return evidence;
}

constexpr LaunchEvidence BootstrappedWithoutSession(SessionRole role)
{
    LaunchEvidence evidence = VerifiedParent();
    evidence.externalBootstrapComplete = true;
    evidence.role = role;
    return evidence;
}

constexpr LaunchEvidence BoundSession(
    SessionObjectKind objectKind,
    SessionRole role)
{
    LaunchEvidence evidence = Session(objectKind, role);
    evidence.parentProcessHandlePresent = true;
    evidence.sessionPidMatchesParent = true;
    return evidence;
}

bool RunCase(const TestCase& testCase)
{
    const LaunchDisposition actual = ClassifyLaunch(testCase.evidence);
    if (actual == testCase.expected)
    {
        return true;
    }

    std::cerr << "FAIL: " << testCase.name
              << " expected disposition "
              << static_cast<int>(testCase.expected)
              << ", got " << static_cast<int>(actual) << '\n';
    return false;
}

void Expect(
    bool condition,
    const char* message,
    int& failures)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
} // namespace

int main()
{
    LaunchEvidence badExecutable = VerifiedParent();
    badExecutable.exactParentExecutable = false;

    LaunchEvidence badDll = VerifiedParent();
    badDll.exactRevivalDll = false;

    LaunchEvidence unsupported = VerifiedParent();
    unsupported.supportedProfile = false;

    LaunchEvidence orphanSession = Session(
        SessionObjectKind::Practice,
        SessionRole::Practice);
    orphanSession.externalParentPresent = false;
    orphanSession.exactParentExecutable = false;

    LaunchEvidence orphanTournament = Session(
        SessionObjectKind::Tournament,
        SessionRole::Tournament);
    orphanTournament.externalParentPresent = false;
    orphanTournament.exactParentExecutable = false;

    LaunchEvidence orphanOnline = BoundSession(
        SessionObjectKind::Online,
        SessionRole::Online);
    orphanOnline.externalParentPresent = false;
    orphanOnline.exactParentExecutable = false;
    orphanOnline.parentProcessHandlePresent = false;
    orphanOnline.sessionPidMatchesParent = false;

    LaunchEvidence parentFlagsWithoutParent = {};
    parentFlagsWithoutParent.exactParentExecutable = true;
    parentFlagsWithoutParent.revivalDllPresent = true;
    parentFlagsWithoutParent.exactRevivalDll = true;

    LaunchEvidence kindWithoutSession = VerifiedParent();
    kindWithoutSession.objectKind = SessionObjectKind::Practice;

    LaunchEvidence pidBindingWithoutSession = VerifiedParent();
    pidBindingWithoutSession.sessionPidMatchesParent = true;

    LaunchEvidence handleWithoutSession = VerifiedParent();
    handleWithoutSession.parentProcessHandlePresent = true;

    LaunchEvidence missingObjectKind = VerifiedParent();
    missingObjectKind.sessionPresent = true;
    missingObjectKind.role = SessionRole::Practice;

    LaunchEvidence missingRole = VerifiedParent();
    missingRole.sessionPresent = true;
    missingRole.objectKind = SessionObjectKind::Practice;

    LaunchEvidence onlineMissingHandle = Session(
        SessionObjectKind::Online,
        SessionRole::Online);
    onlineMissingHandle.sessionPidMatchesParent = true;

    LaunchEvidence onlinePidMismatch = Session(
        SessionObjectKind::Online,
        SessionRole::Online);
    onlinePidMismatch.parentProcessHandlePresent = true;

    LaunchEvidence spectatorMissingBinding = Session(
        SessionObjectKind::Spectator,
        SessionRole::Spectator);

    LaunchEvidence objectBeforeBootstrap = Session(
        SessionObjectKind::Practice,
        SessionRole::Practice);
    objectBeforeBootstrap.externalBootstrapComplete = false;

    const TestCase cases[] = {
        {
            "direct efz.exe startup",
            {},
            LaunchDisposition::DirectGameHost,
        },
        {
            "verified launcher is still initializing",
            VerifiedParent(),
            LaunchDisposition::AwaitExternalSession,
        },
        {
            "bootstrapped role-2 title state without a Practice object",
            BootstrappedWithoutSession(SessionRole::Practice),
            LaunchDisposition::AttachExistingPractice,
        },
        {
            "verified allocated Practice session",
            Session(SessionObjectKind::Practice, SessionRole::Practice),
            LaunchDisposition::AttachExistingPractice,
        },
        {
            "verified parent-bound Online session",
            BoundSession(SessionObjectKind::Online, SessionRole::Online),
            LaunchDisposition::AdoptExternalOnline,
        },
        {
            "verified parent-bound Spectator session",
            BoundSession(
                SessionObjectKind::Spectator,
                SessionRole::Spectator),
            LaunchDisposition::AdoptExternalSpectator,
        },
        {
            "verified Tournament session attaches without duplicate init",
            Session(
                SessionObjectKind::Tournament,
                SessionRole::Tournament),
            LaunchDisposition::AttachExistingTournament,
        },
        {
            "kind-role mismatch selects the explicit passive label",
            Session(SessionObjectKind::Practice, SessionRole::Online),
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "parent executable fingerprint mismatch",
            badExecutable,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "Revival DLL fingerprint mismatch",
            badDll,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "unsupported profile",
            unsupported,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "exact orphan Practice session after launcher exits",
            orphanSession,
            LaunchDisposition::AttachExistingPractice,
        },
        {
            "exact orphan Tournament session after launcher exits",
            orphanTournament,
            LaunchDisposition::AttachExistingTournament,
        },
        {
            "orphan Online session is never ownership-adopted",
            orphanOnline,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "exact-parent flags without a parent",
            parentFlagsWithoutParent,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "object kind without a session pointer",
            kindWithoutSession,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "bootstrapped Online role without its required object",
            BootstrappedWithoutSession(SessionRole::Online),
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "bootstrapped Tournament role without its required object",
            BootstrappedWithoutSession(SessionRole::Tournament),
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "PID binding without a session pointer",
            pidBindingWithoutSession,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "embedded process handle without a session pointer",
            handleWithoutSession,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "session pointer without an object kind",
            missingObjectKind,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "session pointer without a role",
            missingRole,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "Online object with Spectator role",
            BoundSession(
                SessionObjectKind::Online,
                SessionRole::Spectator),
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "Spectator object with Online role",
            BoundSession(
                SessionObjectKind::Spectator,
                SessionRole::Online),
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "Tournament object with Practice role",
            Session(
                SessionObjectKind::Tournament,
                SessionRole::Practice),
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "Online PID match without a process handle",
            onlineMissingHandle,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "Online process handle with a PID mismatch",
            onlinePidMismatch,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "Spectator session without exact parent binding",
            spectatorMissingBinding,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "session object visible before external bootstrap completion",
            objectBeforeBootstrap,
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "complete external bootstrap without a canonical role",
            [] {
                LaunchEvidence evidence = VerifiedParent();
                evidence.externalBootstrapComplete = true;
                return evidence;
            }(),
            LaunchDisposition::PassiveFailClosed,
        },
        {
            "unknown session object",
            Session(SessionObjectKind::Unknown, SessionRole::Practice),
            LaunchDisposition::PassiveFailClosed,
        },
    };

    int failures = 0;
    for (const TestCase& testCase : cases)
    {
        if (!RunCase(testCase))
        {
            ++failures;
        }
    }

    using netplay::bridge::revival_launch::HasExactParentBinding;
    using netplay::bridge::revival_launch::HasExactParentPair;
    using netplay::bridge::takeover::RevivalAddressProfile;
    using netplay::bridge::takeover::RevivalSessionReadinessCheck;
    using netplay::bridge::takeover::RevivalSessionReadinessSatisfied;
    Expect(
        HasExactParentPair(VerifiedParent()),
        "verified EXE+DLL pair and supported profile are exact",
        failures);
    Expect(
        !HasExactParentPair(badDll),
        "a DLL mismatch invalidates the exact parent pair",
        failures);
    Expect(
        HasExactParentBinding(BoundSession(
            SessionObjectKind::Online,
            SessionRole::Online)),
        "process handle plus matching PID is an exact binding",
        failures);
    Expect(
        !HasExactParentBinding(onlinePidMismatch),
        "a handle alone is not an exact parent binding",
        failures);

    Expect(
        RevivalSessionReadinessSatisfied(
            RevivalSessionReadinessCheck::None, 0u),
        "roles without a witness retain their existing admission semantics",
        failures);
    Expect(
        RevivalSessionReadinessSatisfied(
            RevivalSessionReadinessCheck::EqualsOne, 1u)
            && !RevivalSessionReadinessSatisfied(
                RevivalSessionReadinessCheck::EqualsOne, 0u)
            && !RevivalSessionReadinessSatisfied(
                RevivalSessionReadinessCheck::EqualsOne, 2u),
        "legacy Online readiness requires initComplete exactly equal to one",
        failures);
    Expect(
        RevivalSessionReadinessSatisfied(
            RevivalSessionReadinessCheck::NonZero, 0x1234u)
            && !RevivalSessionReadinessSatisfied(
                RevivalSessionReadinessCheck::NonZero, 0u),
        "published-control readiness accepts any non-zero witness",
        failures);

    struct ReadinessProfileExpectation
    {
        const RevivalAddressProfile* profile;
        uintptr_t onlineOffset;
        RevivalSessionReadinessCheck onlineCheck;
        uintptr_t spectatorOffset;
    };
    const ReadinessProfileExpectation readinessProfiles[] = {
        {&netplay::bridge::takeover::kRevival_1_02e,
            1220u, RevivalSessionReadinessCheck::EqualsOne, 0x42Cu},
        {&netplay::bridge::takeover::kRevival_1_02f,
            1220u, RevivalSessionReadinessCheck::EqualsOne, 0x42Cu},
        {&netplay::bridge::takeover::kRevival_1_02f_framestepping,
            1220u, RevivalSessionReadinessCheck::EqualsOne, 0x42Cu},
        {&netplay::bridge::takeover::kRevival_1_02g,
            1220u, RevivalSessionReadinessCheck::EqualsOne, 0x42Cu},
        {&netplay::bridge::takeover::kRevival_1_02h,
            1220u, RevivalSessionReadinessCheck::EqualsOne, 0x42Cu},
        {&netplay::bridge::takeover::kRevival_1_02i,
            1228u, RevivalSessionReadinessCheck::EqualsOne, 0x434u},
        {&netplay::bridge::takeover::kRevival_1_02j,
            0x658u, RevivalSessionReadinessCheck::NonZero, 0x588u},
        {&netplay::bridge::takeover::kRevival_1_02j_20260802,
            0x658u, RevivalSessionReadinessCheck::NonZero, 0x588u},
    };
    for (const ReadinessProfileExpectation& expected : readinessProfiles)
    {
        Expect(
            expected.profile->onlineSessionReadinessOffset
                    == expected.onlineOffset
                && expected.profile->onlineSessionReadinessCheck
                    == expected.onlineCheck
                && expected.profile->spectatorSessionReadinessOffset
                    == expected.spectatorOffset
                && expected.profile->spectatorSessionReadinessCheck
                    == RevivalSessionReadinessCheck::NonZero,
            "all admitted profiles retain their exact role-readiness witnesses",
            failures);
    }

    using netplay::bridge::takeover::launcher_probe_detail::
        ClassifyTournamentAutoNavSuffix;
    using netplay::bridge::takeover::launcher_probe_detail::
        TournamentAutoNavSuffixState;
    static constexpr std::array<uint16_t, 22> canonicalTournamentInputs = {
        0u, 0u, 0u, 0u, 0u, 0u, 4u, 0u, 0u, 0u, 0u,
        4u, 0u, 0u, 0u, 0u, 16u, 0u, 0u, 0u, 0u, 0u,
    };
    for (size_t count = 1u; count <= canonicalTournamentInputs.size(); ++count)
    {
        const uint16_t* suffix = canonicalTournamentInputs.data()
            + canonicalTournamentInputs.size() - count;
        Expect(
            ClassifyTournamentAutoNavSuffix(suffix, count, 0)
                == TournamentAutoNavSuffixState::Ready
                && ClassifyTournamentAutoNavSuffix(suffix, count, 3)
                == TournamentAutoNavSuffixState::Ready,
            "every non-empty canonical Tournament queue suffix is admissible",
            failures);
    }
    Expect(
        ClassifyTournamentAutoNavSuffix(nullptr, 0u, 0)
            == TournamentAutoNavSuffixState::Retry,
        "mode-0 Tournament with an empty queue remains retryable",
        failures);
    Expect(
        ClassifyTournamentAutoNavSuffix(nullptr, 0u, 3)
            == TournamentAutoNavSuffixState::Ready,
        "a consumed Tournament queue is valid after leaving mode 0",
        failures);

    std::array<uint16_t, 22> corruptedTournamentInputs =
        canonicalTournamentInputs;
    corruptedTournamentInputs[6] = 8u;
    Expect(
        ClassifyTournamentAutoNavSuffix(
            corruptedTournamentInputs.data(),
            corruptedTournamentInputs.size(),
            0) == TournamentAutoNavSuffixState::Invalid,
        "a non-canonical Tournament input is rejected",
        failures);
    std::array<uint16_t, 23> oversizedTournamentInputs = {};
    Expect(
        ClassifyTournamentAutoNavSuffix(
            oversizedTournamentInputs.data(),
            oversizedTournamentInputs.size(),
            3) == TournamentAutoNavSuffixState::Invalid
            && ClassifyTournamentAutoNavSuffix(nullptr, 1u, 3)
                == TournamentAutoNavSuffixState::Invalid,
        "malformed Tournament queue samples fail closed",
        failures);

    struct TournamentQueueProfileExpectation
    {
        const RevivalAddressProfile* profile;
        uintptr_t queueOffset;
    };
    const TournamentQueueProfileExpectation tournamentQueueProfiles[] = {
        {&netplay::bridge::takeover::kRevival_1_02e, 740u},
        {&netplay::bridge::takeover::kRevival_1_02f, 740u},
        {&netplay::bridge::takeover::kRevival_1_02f_framestepping, 740u},
        {&netplay::bridge::takeover::kRevival_1_02g, 740u},
        {&netplay::bridge::takeover::kRevival_1_02h, 740u},
        {&netplay::bridge::takeover::kRevival_1_02i, 748u},
        {&netplay::bridge::takeover::kRevival_1_02j, 0x340u},
        {&netplay::bridge::takeover::kRevival_1_02j_20260802, 0x340u},
    };
    for (const TournamentQueueProfileExpectation& expected
        : tournamentQueueProfiles)
    {
        Expect(
            expected.profile->tournamentInputQueueOffset
                == expected.queueOffset,
            "every Tournament profile retains its exact deque offset",
            failures);
    }

    const RevivalAddressProfile& newJ =
        netplay::bridge::takeover::kRevival_1_02j_20260802;
    Expect(
        newJ.versionTag != nullptr
            && std::strcmp(newJ.versionTag, "1.02j") == 0
            && newJ.peTimestamp == 0x6A6F0F7Bu
            && newJ.defaultImageBase == 0x70000000u,
        "the 2026-08-02 compile has a distinct exact DLL identity in the 1.02j ABI family",
        failures);
    Expect(
        newJ.launcherExeTimestamp == 0x6A6F0E2Bu
            && newJ.launcherExeSizeOfImage == 0x00294000u
            && newJ.launcherExeEntryPointRva == 0x00001460u
            && newJ.launcherExeFileSize == 2673664u
            && newJ.launcherExeSha256 != nullptr
            && std::strcmp(
                newJ.launcherExeSha256,
                "7C7707322B7CDA0B1479DFE97A7260D9E971BA162DFCE755C894B9626FEB1C42") == 0,
        "the 2026-08-02 launcher fingerprint remains exact",
        failures);
    Expect(
        newJ.revivalDllFileSize == 1911296u
            && newJ.revivalDllSha256 != nullptr
            && std::strcmp(
                newJ.revivalDllSha256,
                "62D9ED74B7931E2BC7EA208109425BC25207C82FACA91A96A55C778F8D568F32") == 0
            && newJ.revivalDllSizeOfImage == 0x001D8000u
            && newJ.revivalDllEntryPointRva == 0x000011F0u
            && newJ.initExportRva == 0x0012DB70u,
        "the 2026-08-02 Revival DLL fingerprint and PE anchors remain exact",
        failures);
    Expect(
        newJ.companionDdrawFileSize == 1314816u
            && newJ.companionDdrawSha256 != nullptr
            && std::strcmp(
                newJ.companionDdrawSha256,
                "0E072342D87D240E18969BC3663032E0E370EABA24D1FBB8E6C1F6216AD9F603") == 0
            && newJ.usesProceduralDdrawTextApi,
        "the renderer-split compile remains paired with its exact procedural Ddraw DLL",
        failures);
    Expect(
        newJ.roleFlagOffsets[0] == 0x0014EC40u
            && newJ.sessionPtrOffsets[0] == 0x0014E980u
            && newJ.globalStatePtrOffset == 0x0014E920u
            && newJ.initFlagOffset == 0x0014EC44u
            && newJ.initByteOffset == 0x0014ED64u
            && newJ.timerPtrOffset == 0x0014E8C4u
            && newJ.renderContextBaseOffset == 0x0014E8C0u
            && newJ.renderContextGlobalOffset == 0u
            && newJ.rngEngineStateOffset == 0x0014E984u,
        "the 2026-08-02 relocated globals remain exact",
        failures);
    Expect(
        newJ.startInitPlayerRva == 0x000542B0u
            && newJ.inputSwapPairRva == 0x000541A0u
            && newJ.frameHookRva == 0x000418E0u
            && newJ.perFrameTickRva == 0x000421D0u
            && newJ.clearTextRva == 0x00078B80u
            && newJ.setTextEnabledRva == 0x000788D0u
            && newJ.tournamentExeHookHandlerRva == 0x0003CEC0u
            && newJ.launcherCleanupTerminateCallRva == 0x00089A11u,
        "the 2026-08-02 relocated hook and renderer RVAs remain exact",
        failures);
    Expect(
        newJ.onlineSessionVtableRva == 0x0016FBD0u
            && newJ.spectatorSessionVtableRva == 0x0016FC00u
            && newJ.replaySessionVtableRva == 0x0016FC30u
            && newJ.practiceSessionVtableRva == 0x0016FC60u
            && newJ.tournamentSessionVtableRva == 0x0016FB90u,
        "the 2026-08-02 session vtables remain exact",
        failures);
    Expect(
        newJ.tournamentExitGuardRva == 0x0004777Fu
            && newJ.practicePostInitRva == 0x0007F080u
            && newJ.spectatorPostInitRva == 0x0005F540u
            && newJ.memorialPauseIntegrationRva == 0x00078620u
            && newJ.rollbackReadInputRva == 0x00053640u,
        "the 2026-08-02 profile-driven runtime and verifier RVAs remain exact",
        failures);
    Expect(
        newJ.tournamentDeletingDtorRva == 0x00049490u
            && newJ.onlineDeletingDtorRva == 0x00059480u
            && newJ.spectatorDeletingDtorRva == 0x00063800u
            && newJ.replayDeletingDtorRva == 0x00076B70u
            && newJ.practiceDeletingDtorRva == 0x00081870u,
        "the 2026-08-02 deleting destructors remain exact",
        failures);
    Expect(
        netplay::bridge::takeover::kRevival_1_02j.peTimestamp
                != newJ.peTimestamp
            && netplay::bridge::takeover::kRevival_1_02j.launcherExeTimestamp
                != newJ.launcherExeTimestamp
            && netplay::bridge::takeover::kRevival_1_02j.revivalDllFileSize
                != newJ.revivalDllFileSize
            && !netplay::bridge::takeover::kRevival_1_02j.usesProceduralDdrawTextApi
            && netplay::bridge::takeover::kRevival_1_02j.companionDdrawFileSize == 0u
            && netplay::bridge::takeover::kRevival_1_02j.companionDdrawSha256 == nullptr,
        "the original and renderer-split 1.02j compiles cannot collapse into one identity",
        failures);

    if (failures != 0)
    {
        std::cerr << failures << " Revival launch policy contract(s) failed\n";
        return 1;
    }

    std::cout << "Revival launch policy contracts passed\n";
    return 0;
}
