#!/usr/bin/env python3
"""Static safety contracts for direct- and Revival-first startup."""

from __future__ import annotations

import re
import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"Revival launcher adoption contract: {message}", file=sys.stderr)
    raise SystemExit(1)


root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()


def read(relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8", errors="strict")


def function_slice(source: str, signature: str) -> str:
    search_at = 0
    while True:
        start = source.find(signature, search_at)
        if start < 0:
            fail(f"could not locate function: {signature}")
        opening = source.find("{", start)
        semicolon = source.find(";", start)
        if opening >= 0 and (semicolon < 0 or opening < semicolon):
            break
        search_at = semicolon + 1
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    fail(f"unterminated function: {signature}")
    return ""


addresses = read("include/netplay/bridge/revival_addresses.h")
expected_identity = {
    "1.02e": (
        "44CCD28BCFDBD2F407A5DAA03EBB16824D332660CDC07D0A27E69DC4BEB5B19E",
        "0A30E2309D36C323E8085886E4C661684F495DA515F2CE1FE67FB70432F0F515",
        "0x00038E88u",
    ),
    "1.02f": (
        "B40323D0472A771E9D176F321E8EA47F7BFA1E6A5AD8C38B99649FD4BEA15019",
        "B9D21C86F1403EEC9C6262414F4ABDBB9BC2A7ED05FC90222363C2D15802DC86",
        "0x00038EC8u",
    ),
    "1.02f-framestepping": (
        "B40323D0472A771E9D176F321E8EA47F7BFA1E6A5AD8C38B99649FD4BEA15019",
        "D249BFFC0F879601FFF613C8D6D921FE7109DC617FF2CFB7A7E8C539068C3AA5",
        "0x00038EC8u",
    ),
    "1.02g": (
        "949C9AAA28F1DDDE89931DC5303500777939A64396BA9D79486E21DF7D303872",
        "8A5A268DDC419E38D0E5BD6E3974732D2C9C4A17A62FDDE6E2724B8D80D250E7",
        "0x00038FE8u",
    ),
    "1.02h": (
        "E2C8B136A36F829F4178DAF96655EE952FFA465776AE204219CAFC24B74B488F",
        "0687E74BB40A8AADC91B591284BC962A320078394BDF7D23B9741D54E937F8AA",
        "0x00038F78u",
    ),
    "1.02i": (
        "13389473CC9D1B1C829AA6252AAD3DF654194ABFA37CB572AB6FA514F971A9E0",
        "18F91D5CDD8F17FB6CDE90E1AAA3C58A374B4AA80E8E906FDD88CB7FEB9D62D0",
        "0x0003A068u",
    ),
    "1.02j": (
        "194710A1C3DFEA3C50AC7CC5779794B986F6DF0F7009FFD5696552467F9CC027",
        "CEDD606869EE6CFA227B385AF27DC92ABC13FE9C2D432935BCBE3B789468A169",
        "0x00089A11u",
    ),
}
for profile, values in expected_identity.items():
    for value in values:
        if value not in addresses:
            fail(f"exact catalog identity missing for {profile}: {value}")

for required_field in (
    "canonicalRoleFlagOffset",
    "canonicalSessionPtrOffset",
    "onlineSessionVtableRva",
    "spectatorSessionVtableRva",
    "practiceSessionVtableRva",
    "tournamentSessionVtableRva",
    "spectatorSessionOffsetHelperHandle",
    "spectatorSessionOffsetHelperPid",
    "onlineSessionReadinessOffset",
    "onlineSessionReadinessCheck",
    "spectatorSessionReadinessOffset",
    "spectatorSessionReadinessCheck",
    "tournamentExeHookHandlerRva",
    "launcherCleanupTerminateCallRva",
):
    if required_field not in addresses:
        fail(f"profile lost launcher admission field: {required_field}")

policy = read("include/netplay/bridge/revival_launch_policy.h")
for required in (
    "AttachExistingPractice",
    "AdoptExternalOnline",
    "AdoptExternalSpectator",
    "AttachExistingTournament",
    "PassiveFailClosed",
    "!evidence.externalParentPresent && !evidence.revivalDllPresent",
    "HasExactParentBinding(evidence)",
):
    if required not in policy:
        fail(f"launcher policy lost invariant: {required}")

probe = read("src/netplay/bridge/revival_launcher_probe.cpp")
for required in (
    "ReadFileSha256",
    "ProfileMatchesLauncherFile",
    "ProfileMatchesDllFile",
    "canonicalRoleFlagOffset",
    "canonicalSessionPtrOffset",
    "ClassifyObject",
    "ValidateParentBinding",
    "ReadSessionReadiness",
    "RevivalSessionReadinessSatisfied",
    "RetainAdmittedLauncherSnapshot",
    "MatchesAdmittedLauncherSnapshot",
    "ProbePreloadedExistingRevival",
    "IsNativeFrameBootstrapInstalled",
    "0x00401582u",
):
    if required not in probe:
        fail(f"launcher probe lost exact/fail-closed evidence: {required}")
if "g_activeRevival != probe.profile" in probe:
    fail("launcher probe compares cross-translation-unit profile addresses")
parent_profile_check = function_slice(probe, "bool ParentAcceptsProfile(")
for required in (
    "candidate->peTimestamp == profile->peTimestamp",
    "candidate->launcherExeTimestamp",
    "std::strcmp",
):
    if required not in parent_profile_check:
        fail(f"launcher parent profile identity is not value-based: {required}")
if "dllBase + profile.initOnceGuardOffset" in probe:
    fail("launcher probe again treats 1.02j RVA zero/DOS bytes as an init guard")

probe_header = read("include/netplay/bridge/revival_launcher_probe.h")
for required in (
    "revivalDllBase",
    "sessionVtable",
    "helperHandleValue",
    "helperPid",
    "readinessValue",
    "readinessSatisfied",
):
    if required not in probe_header:
        fail(f"admitted launcher snapshot lost field: {required}")

revalidate = function_slice(
    probe, "static bool RevalidateRevivalLauncherStartupImpl(")
for required in (
    "SameLauncherSessionSnapshot(first, second)",
    "MatchesAdmittedLauncherSnapshot(probe, first)",
    "AttachExistingTournament",
    "!first.readinessSatisfied",
):
    if required not in revalidate:
        fail(f"post-guard revalidation lost exact snapshot check: {required}")

startup_probe = function_slice(probe, "RevivalLauncherProbe ProbeRevivalLauncherStartup(")
for required in (
    "roleRequiresReadiness && !second.readinessSatisfied",
    "Sleep(10)",
    "continue",
    "RetainAdmittedLauncherSnapshot(&result, second)",
):
    if required not in startup_probe:
        fail(f"launcher readiness is no longer retryable/admitted exactly: {required}")

ipc = read("src/netplay/bridge/ipc_shared.cpp")
ensure_local = function_slice(ipc, "bool EnsureLocalRevivalLoaded(")
if ensure_local.count("g_localInitFn(localParams)") != 1:
    fail("direct startup must retain exactly one exported init call")
for required in (
    "ProbeRevivalLauncherStartup",
    "initCalls=0",
    "InstallExternalLauncherGuard",
    "PeerProcessOwnership::ExternalLauncherParent",
    "SaveRenderContext",
    "ResetHostSharedBlockForSession",
    "BeginManagedSessionBoundary",
    "InstallNetplayFrameHook",
    "ArmExternalLauncherExitRecovery",
    "PatchRevivalDllExitProcess",
    "RevalidateRevivalLauncherStartup",
    "HasAnyNetplayFrameHookInstalled",
    "HasNetplayPerFrameTickHookInstalled",
    "requestManagedAttachRecovery",
    "WaitForExternalLauncherAttachBoundary",
    "RevalidateRevivalLauncherSessionSnapshot",
):
    if required not in ensure_local:
        fail(f"launcher adoption lost integration step: {required}")

guard_install_at = ensure_local.find("InstallExternalLauncherGuard(")
preflight_at = ensure_local.find(
    "renderContextSaved = SaveRenderContext()", guard_install_at)
online_stage_role_at = ensure_local.find(
    "g_localRoleFlag = probe.role;", guard_install_at)
online_pending_at = ensure_local.find(
    "SetExternalLauncherAttachPending(true);", online_stage_role_at)
online_arm_at = ensure_local.find(
    "recoveryArmed = ArmExternalLauncherExitRecovery();", online_pending_at)
online_frame_hook_at = ensure_local.find(
    "frameHookReady =", online_arm_at)
online_boundary_at = ensure_local.find(
    "WaitForExternalLauncherAttachBoundary(5000u)", online_frame_hook_at)
online_iat_at = ensure_local.find(
    "&& PatchRevivalDllExitProcess();", online_boundary_at)
post_revalidate_at = ensure_local.rfind(
    "if (!RevalidateRevivalLauncherSessionSnapshot(probe))")
parent_duplicate_at = ensure_local.find(
    "DuplicateHandle(", guard_install_at, post_revalidate_at)
if not (
    0 <= guard_install_at < preflight_at < online_stage_role_at
    < online_pending_at < online_arm_at < online_frame_hook_at
    < online_boundary_at < online_iat_at < post_revalidate_at
):
    fail("Online/Spectator attach ordering no longer stages native evidence safely")
if parent_duplicate_at < 0 \
        or "g_revivalProcess = sessionParentProcess;" not in ensure_local[
            parent_duplicate_at:post_revalidate_at
        ]:
    fail("session watcher no longer receives a duplicate exact-parent handle")
if "probe.parentProcess = nullptr;" in ensure_local[
        guard_install_at:post_revalidate_at
    ]:
    fail("final launcher revalidation again loses its probe-owned parent handle")

tournament_block_at = ensure_local.find(
    "Role 3 may begin with only one queued title input")
tournament_stage_at = ensure_local.find(
    "g_localRoleFlag = probe.role;", tournament_block_at)
tournament_pending_at = ensure_local.find(
    "SetExternalLauncherAttachPending(true);", tournament_stage_at)
tournament_frame_at = ensure_local.find(
    "frameHookReady = InstallNetplayFrameHook();", tournament_pending_at)
tournament_boundary_at = ensure_local.find(
    "WaitForExternalLauncherAttachBoundary(5000u)", tournament_frame_at)
tournament_guard_at = ensure_local.find(
    "SaveAndApplyExternalTournamentExitGuard()", tournament_boundary_at)
tournament_iat_at = ensure_local.find(
    "PatchRevivalDllExitProcess()", tournament_guard_at)
tournament_revalidate_at = ensure_local.find(
    "if (!RevalidateRevivalLauncherSessionSnapshot(probe))",
    tournament_iat_at,
)
if not (
    0 <= tournament_block_at < tournament_stage_at < tournament_pending_at
    < tournament_frame_at < tournament_boundary_at < tournament_guard_at
    < tournament_iat_at < tournament_revalidate_at < preflight_at
):
    fail("Tournament survival boundary no longer precedes renderer/IPC setup")
if "Practice process converted to managed direct-session baseline initCalls=0" not in ensure_local:
    fail("launcher Practice process can no longer transition safely into a later managed session")
for forbidden in (
    "RemoveNetplayFrameHook()",
    "RestoreRevivalDllExitProcessIat()",
):
    if forbidden in ensure_local:
        fail(f"live external adoption regained unsafe post-publication rollback: {forbidden}")
if "return true;" not in ensure_local[online_frame_hook_at:post_revalidate_at]:
    fail("post-publication failure no longer stays managed for title recovery")

guard = read("src/netplay/bridge/external_launcher_guard.cpp")
for forbidden in (
    "BuildPatchMap(",
    "PatchIatModule(",
    "nb_stub_CreateProcessA",
    "InitializeInjected",
    "StartConsoleCaptureWorker",
):
    if forbidden in guard:
        fail(f"narrow parent guard expanded into helper takeover: {forbidden}")
for required in (
    '"TerminateProcess"',
    "expectedCleanupReturnRva",
    "targetPid != g_guardMarker->childProcessId",
    "caller - g_guardExeBase",
    "terminateBlockedSerial",
):
    if required not in guard:
        fail(f"parent kill guard lost exact constraint: {required}")

iat = read("src/netplay/bridge/iat_stubs.cpp")
exported_terminate = function_slice(iat, "nb_stub_TerminateProcess(")
if "_ReturnAddress()" not in exported_terminate:
    fail("TerminateProcess IAT target does not capture the launcher call site")
stub_terminate = function_slice(iat, "BOOL StubTerminateProcess(")
if "_ReturnAddress()" in stub_terminate:
    fail("TerminateProcess implementation captures its wrapper instead of launcher caller")
if "EvaluateExternalLauncherTerminate(hProcess, callerReturnAddress)" not in stub_terminate:
    fail("launcher return address is not passed into the exact guard")

patch_exit = function_slice(iat, "bool PatchRevivalDllExitProcess(")
for required in (
    "current == NeutralizeExitProcess",
    "g_realExitProcess != NeutralizeExitProcess",
    "g_realExitProcess != current",
    "nativeExitProcess",
    "current != nativeExitProcess",
):
    if required not in patch_exit:
        fail(f"child ExitProcess patch is not idempotent: {required}")

probe = read("src/netplay/bridge/revival_launcher_probe.cpp")
native_bootstrap = function_slice(
    probe, "bool IsNativeFrameBootstrapInstalled(")
for required in (
    "const RevivalAddressProfile& profile",
    "profile.frameHookRva",
    "kExeFrameHookAddress = 0x00401582u",
    "kStolenBytes = 10u",
    "kTrampolinePrefixBytes = 9u",
    "kStockDisplaced",
    "0xC7, 0x05, 0x4C, 0x01, 0x79",
    "for (size_t i = 5u; i < kStolenBytes; ++i)",
    "site[i] != 0x90",
    "ReadModuleImageRange(",
    "IsExecutableCommittedRange(trampoline, kTrampolineSize)",
    "trampolineBytes[0] != 0x60",
    "trampolineBytes[1] != 0x9C",
    "trampolineBytes[2] != 0xE8",
    "trampolineBytes[7] != 0x9D",
    "trampolineBytes[8] != 0x61",
    "std::memcmp(",
    "callback == expectedCallback",
    "returnTarget == kExeFrameHookAddress + kStolenBytes",
):
    if required not in native_bootstrap:
        fail(f"native frame bootstrap lost exact trampoline binding: {required}")
if "IsNativeFrameBootstrapInstalled(\n        profile, dllBase)" not in probe:
    fail("launcher snapshot does not bind bootstrap validation to its exact profile/base")

neutralize_exit = function_slice(iat, "static VOID WINAPI NeutralizeExitProcess(")
for required in (
    "g_netplayFrameJmpOwnerThreadId == currentThreadId",
    "g_netplayUiJmpOwnerThreadId == currentThreadId",
    "refusing cross-thread longjmp",
):
    if required not in neutralize_exit:
        fail(f"ExitProcess recovery lost thread-affine longjmp guard: {required}")

runtime = "\n".join(
    path.read_text(encoding="utf-8", errors="strict")
    for tree in (root / "include", root / "src")
    for path in tree.rglob("*")
    if path.suffix in {".h", ".cpp"}
)
ownership_terminator = function_slice(ipc, "BOOL TerminatePeerProcessIfOwned(")
runtime_without_owner = runtime.replace(ownership_terminator, "", 1)
if re.search(
    r"TerminateProcess\s*\(\s*(?:takeover::)?g_revivalProcess",
    runtime_without_owner,
):
    fail("peer teardown bypasses explicit spawned-vs-external ownership")
for required in (
    "PeerProcessOwnership::ExternalLauncherParent",
    "HasActiveExactExternalLauncherParent()",
    "RetireExternalLauncherGuardSignalDelivery()",
    "GetProcessId(g_revivalProcess)",
    "WaitForSingleObject(g_revivalProcess",
    "ERROR_TIMEOUT",
    "LaunchDisposition::DirectGameHost",
):
    if required not in ownership_terminator:
        fail(f"exact external launcher termination lost lifecycle guard: {required}")
if "PROCESS_TERMINATE" not in probe:
    fail("exact launcher parent handle cannot terminate the admitted generation")
release_after_termination = function_slice(
    ipc,
    "bool ReleasePeerProcessAfterTerminationAttempt(",
)
for required in (
    "ExternalLauncherParent",
    "!terminationSucceeded",
    "retaining exact external Revival parent",
    "CloseProcessHandle(status, waitForPeerWatcher)",
):
    if required not in release_after_termination:
        fail(f"failed exact-parent termination can discard its reusable-session guard: {required}")
process_alive = function_slice(ipc, "bool ProcessAlive(")
for required in (
    "PeerProcessOwnership::ExternalLauncherParent",
    "!exitCodeRead && externalLauncherParent",
    "WaitForSingleObject(g_revivalProcess, 0)",
    "waitResult != WAIT_OBJECT_0",
    "LaunchDisposition::DirectGameHost",
    "CloseProcessHandle(status)",
):
    if required not in process_alive:
        fail(f"late exact-parent exit does not normalize launcher state: {required}")
emergency_shutdown = function_slice(
    read("src/netplay/bridge/revival_takeover.cpp"),
    "void EmergencyShutdownHost()",
)
if 'TerminatePeerProcessIfOwned(\n            0, "emergency_shutdown_host", false)' not in emergency_shutdown:
    fail("loader-lock emergency shutdown can wait on external launcher exit")

dllmain = read("src/dllmain.cpp")
guard_at = dllmain.find("TryStartExternalLauncherGuardProcess()")
helper_at = dllmain.find("IsCurrentProcessRevival()")
if guard_at < 0 or helper_at < 0 or guard_at > helper_at:
    fail("narrow launcher-parent mode is not selected before helper takeover")
for required in (
    "g_passiveInitialization",
    "UninstallCrashHandlers",
    "ShutdownLogger",
):
    if required not in dllmain:
        fail(f"passive admission leaves a persistent mod side effect: {required}")
preimage_at = dllmain.find("CaptureTournamentExePreimageAtProcessAttach()")
game_thread_at = dllmain.find("CreateThread(nullptr, 0, InitializeModThread")
ui_finalize_at = dllmain.find("CompleteLauncherUiAttachment(hooksInstalled)")
if not (0 <= preimage_at < game_thread_at):
    fail("Tournament EFZ preimage is not captured before the mod worker can race native init")
if ui_finalize_at < 0:
    fail("launcher Tournament parked boundary is not completed after UI hook installation")

revival = read("src/netplay/bridge/revival_memory.cpp")
parity_gate = function_slice(revival, "static bool NeedsPostNativeControlPath(")
if "HasExternalLauncherTerminateBlockedSignal()" not in parity_gate:
    fail("blocked launcher kill cannot wake event-driven title recovery")
if "ConsumeExternalLauncherTerminateBlockedSignal()" not in revival:
    fail("blocked launcher kill signal is never consumed")

tick_hook = function_slice(revival, "static int __fastcall OurPerFrameTickHook(")
attach_pending_at = tick_hook.find(
    "externalAttachState == kExternalAttachPending")
attach_fatal_at = tick_hook.find("const bool fatalConsoleError", attach_pending_at)
if attach_pending_at < 0 or attach_fatal_at < 0:
    fail("single-boundary external attach state machine is missing")
if "return 0;" in tick_hook[attach_pending_at:attach_fatal_at]:
    fail("external attach again drops native tick invocations while pending")
for required in (
    "kExternalAttachCommit",
    "g_externalLauncherAttachObserved",
):
    if required not in tick_hook[attach_pending_at:attach_fatal_at]:
        fail(f"external attach boundary lost state: {required}")
if "kExternalAttachQuarantine" not in tick_hook:
    fail("external attach boundary lost quarantine state")
for required in (
    "ArmTournamentReturnCleanup(\"external_attach_quarantine\")",
    "InvokeOriginalPerFrameTickPreservingNonvolatile",
):
    if required not in tick_hook and required not in revival:
        fail(f"Tournament tick boundary lost critical recovery/ABI invariant: {required}")

recovery = read("src/netplay/bridge/gameplay_exit_recovery.cpp")
if "RetireExternalLauncherGuardSignalDelivery()" not in recovery:
    fail("external launcher signal generation is not retired at recovery")
if "gameplay_exit_recovery_external_launcher" not in recovery:
    fail("external recovery does not terminate the singleton-owning launcher generation")
if "ReleasePeerProcessAfterTerminationAttempt(" not in recovery:
    fail("external recovery does not release the ordinary peer slot")

takeover = read("src/netplay/bridge/revival_takeover.cpp")
start_session = function_slice(takeover, "bool StartSession(")
reap_at = start_session.find("ReapExternalLauncherGuardIfParentExited()")
retained_parent_at = start_session.find(
    "PeerProcessOwnership::ExternalLauncherParent",
    reap_at,
)
process_alive_at = start_session.find("ProcessAlive(ioStatus)", retained_parent_at)
boundary_at = start_session.find("BeginManagedSessionBoundary(")
ensure_local_at = start_session.find("EnsureLocalRevivalLoaded(true)")
if not (
    0 <= reap_at < retained_parent_at < process_alive_at < boundary_at < ensure_local_at
):
    fail("retained external launcher is not resolved before direct-session preflight")
initialize_host = function_slice(takeover, "bool InitializeHost()")
launcher_resolve_at = initialize_host.find("EnsureLocalRevivalLoaded()")
for side_effect in (
    "RecoverTemporaryHostProtocolOverride(",
    "CleanupNativeHostShadowLogDirectory(",
    "PrimeManagedLogEfzHistory()",
    "EnsureHostIpc()",
):
    side_effect_at = initialize_host.find(side_effect)
    if launcher_resolve_at < 0 or side_effect_at <= launcher_resolve_at:
        fail(
            "passive launcher ownership must be resolved before startup side effect: "
            + side_effect
        )

tournament_cleanup = function_slice(
    takeover, "bool CompletePendingTournamentReturnCleanup(")
for required in (
    "RestoreTournamentExePatches()",
    "ForceLocalPlayInit()",
    "RestoreDllExitProcessPatches()",
    "g_tournamentReturnCleanupStage",
    "retainTournamentQuarantine",
    "g_tournamentReturnCleanupPending = false",
):
    if required not in tournament_cleanup:
        fail(f"Tournament return transaction lost stage/fail-stop invariant: {required}")
if not (
    tournament_cleanup.find("RestoreTournamentExePatches()")
    < tournament_cleanup.find("ForceLocalPlayInit()")
    < tournament_cleanup.find("RestoreDllExitProcessPatches()")
):
    fail("Tournament return transaction order is no longer EXE -> object/init -> DLL")

title_selection = function_slice(takeover, "void OnTitleSelectionConfirmed(")
for required in (
    "exeJournalReady",
    "exitGuardsReady",
    "exactTournamentObject",
    "queueNeutralized",
    "tournamentSessionVtableRva",
    "direct_tournament_commit_validation_failure",
    "ArmTournamentReturnCleanup(",
):
    if required not in title_selection:
        fail(f"direct-game Tournament lost fail-closed start contract: {required}")
if not (
    title_selection.find("SaveTournamentExePatches()")
    < title_selection.find("SaveAndApplyDllExitProcessPatches()")
    < title_selection.find("SetLocalRoleFlag(")
    < title_selection.find("NeutralizeTournamentAutoNav()")
):
    fail("direct-game Tournament start order is no longer journal -> guard -> init -> queue")
for ignored in (
    "(void)SaveTournamentExePatches()",
    "(void)SaveAndApplyDllExitProcessPatches()",
    "(void)SetLocalRoleFlag(kLocalRoleTournament",
    "(void)NeutralizeTournamentAutoNav()",
):
    if ignored in title_selection:
        fail(f"direct-game Tournament again ignores a commit result: {ignored}")

ui_finalize = function_slice(
    ipc, "bool CompleteExternalLauncherUiAttachment(")
for required in (
    "AdoptExistingTournamentExePatchState()",
    "IsExternalTournamentExitGuardOwned()",
    "IsRevivalDllExitProcessIatPatched()",
    "onlineSessionVtableRva",
    "spectatorSessionVtableRva",
    "simulationHandoffReady",
    "HasNetplayPerFrameTickHookInstalled()",
    "IsExternalLauncherAttachBoundaryPending()",
    "CompleteExternalLauncherAttachBoundary(true)",
    "CompleteExternalLauncherAttachBoundary(false)",
):
    if required not in ui_finalize:
        fail(f"launcher UI commit lost parked-boundary validation: {required}")

# Online/Spectator must remain parked through StateExport initialization and
# InstallHooks.  Releasing inside EnsureLocalRevivalLoaded races live title
# code and leaves recurring export work active during native simulation.
if "holding native tick until title/UI installation and simulation handoff complete" \
        not in ensure_local:
    fail("external Online/Spectator no longer hold the adopted tick through UI setup")
if "CompleteExternalLauncherAttachBoundary(" in ensure_local:
    fail("external attach boundary is released before post-InstallHooks finalization")

session_bridge = read("src/netplay/bridge/session_bridge.cpp")
complete_ui = function_slice(
    session_bridge, "bool CompleteLauncherUiAttachment(")
needs_at = complete_ui.find("NeedsExternalLauncherSimulationHandoff()")
hooks_gate_at = complete_ui.find("hooksInstalled", needs_at)
prepare_at = complete_ui.find("PrepareExternalLauncherSimulationHandoff()")
session_lock_at = complete_ui.find("std::lock_guard<std::mutex> lock(g_mutex)")
finalize_at = complete_ui.find("CompleteExternalLauncherUiAttachment(")
if not (0 <= needs_at < hooks_gate_at < prepare_at < session_lock_at < finalize_at):
    fail("launcher simulation handoff must run outside the SessionBridge mutex before commit")
if complete_ui.count("PrepareExternalLauncherSimulationHandoff()") != 1 \
        or complete_ui.count("CompleteExternalLauncherUiAttachment(") != 1:
    fail("launcher UI handoff/finalizer is no longer a single publication path")

dllmain_init = function_slice(dllmain, "DWORD WINAPI InitializeModThread(")
try_at = dllmain_init.find("__try")
install_ui_at = dllmain_init.find("netplay::InstallHooks()")
complete_ui_at = dllmain_init.find("CompleteLauncherUiAttachment(hooksInstalled)")
completed_at = dllmain_init.find("startupCompleted = true", complete_ui_at)
finally_at = dllmain_init.find("__finally", completed_at)
incomplete_guard_at = dllmain_init.find("if (!startupCompleted)", finally_at)
emergency_at = dllmain_init.find(
    "EmergencyQuarantineLauncherUiAttachment()", incomplete_guard_at)
if not (
    0 <= try_at < install_ui_at < complete_ui_at < completed_at
    < finally_at < incomplete_guard_at < emergency_at
):
    fail("launcher attach boundary can be released before title/UI hooks finish installing")
for required in (
    "__try",
    "__finally",
    "startupCompleted",
    "EmergencyQuarantineLauncherUiAttachment()",
):
    if required not in dllmain_init:
        fail(f"startup SEH can strand a parked launcher tick: {required}")

emergency_quarantine = function_slice(
    revival, "void EmergencyQuarantineExternalLauncherAttachBoundary(")
for required in (
    "IsExternalLauncherAttachBoundaryPending()",
    "g_revivalExitMode",
    "g_revivalExitIntercepted",
    "MemoryBarrier()",
    "kExternalAttachQuarantine",
    "kExternalAttachPending",
):
    if required not in emergency_quarantine:
        fail(f"emergency attach quarantine lost no-lock publication: {required}")
mode_at = emergency_quarantine.find("&g_revivalExitMode")
intercepted_at = emergency_quarantine.find("&g_revivalExitIntercepted")
barrier_at = emergency_quarantine.find("MemoryBarrier()")
quarantine_at = emergency_quarantine.find("kExternalAttachQuarantine")
pending_at = emergency_quarantine.find("kExternalAttachPending", quarantine_at)
if not (0 <= mode_at < intercepted_at < barrier_at < quarantine_at < pending_at):
    fail("emergency quarantine no longer publishes recovery before boundary release")

runtime_sources = "\n".join(
    path.read_text(encoding="utf-8", errors="strict")
    for path in (root / "src").rglob("*.cpp")
)
if runtime_sources.count("CompleteExternalLauncherAttachBoundary(true)") != 1 \
        or runtime_sources.count("CompleteExternalLauncherAttachBoundary(false)") != 1:
    fail("post-InstallHooks finalizer is no longer the sole normal boundary release authority")
if runtime_sources.count("SetExternalLauncherAttachPending(false)") != 2 \
        or ensure_local.count("SetExternalLauncherAttachPending(false)") != 2:
    fail("attach-pending reset escaped the two proven pre-observation rollback branches")

title_flow = read("src/netplay/hooks/title_flow.cpp")
external_handoff = function_slice(
    title_flow, "bool PrepareExternalLauncherSimulationHandoffImpl(")
for required in (
    "screenTable[0]",
    "SuspendUiHooksForOnlineSimulation(",
    '"external_launcher_adoption"',
):
    if required not in external_handoff:
        fail(f"external launcher lost hooks-layer simulation handoff: {required}")

suspend_ui = function_slice(
    title_flow, "static bool SuspendUiHooksForOnlineSimulation(")
if "g_onlineSimulationUiSuspended.store(" not in suspend_ui:
    fail("successful UI suspension no longer latches the simulation generation")
enter_menu = function_slice(title_flow, "void EnterNetplayMenu(")
resume_at = enter_menu.find("state_export::ResumeControlPlaneUpdates()")
reset_latch_at = enter_menu.find("g_onlineSimulationUiSuspended.store(false")
active_menu_guard_at = enter_menu.find("if (g_netplayMenuState.active)")
if not (0 <= reset_latch_at < resume_at < active_menu_guard_at):
    fail("menu recovery does not reset UI-suspension ownership before resuming export")

menu_hooks = read("src/netplay/hooks/menu_hooks.cpp")
title_update = function_slice(menu_hooks, "static char HookedTitleUpdateImplBody(")
overlay_at = title_update.find("EnsureGameplayOverlayHook()")
simulation_latch_at = title_update.find("g_onlineSimulationUiSuspended.load(")
if not (0 <= simulation_latch_at < overlay_at):
    fail("title update can reinstall EndScene after online simulation handoff")

destroy = function_slice(revival, "bool DestroyCurrentSession(")
for required in (
    "strictExternalTournament",
    "g_lastValidatedSessionPtr",
    "ReadRoleFlagFromRevival()",
    "sessionPtr != admittedSession",
    "tournamentSessionVtableRva",
    "0x0002D3F0u",
    "0x0002D630u",
    "0x0002D8F0u",
    "0x00048550u",
    "refusing init(2,102)",
):
    if required not in destroy and required not in revival:
        fail(f"external Tournament destructor lost exact/fail-stop contract: {required}")
force_local = function_slice(revival, "bool ForceLocalPlayInit(")
if "strictExternalTournament" not in force_local \
        or "ReadSessionPointerFromRevivalLoose()" not in force_local:
    fail("external Tournament cleanup can overwrite its admitted session generation")

external_ipc_at = ensure_local.find("EnsureHostIpc()", guard_install_at)
reset_ipc_at = ensure_local.find(
    "ResetHostSharedBlockForSession(", guard_install_at)
if not (guard_install_at < external_ipc_at < reset_ipc_at):
    fail("external adoption no longer creates IPC lazily after exact guard admission")

if '"1.02f-framestepping"' not in function_slice(
    takeover, "static uintptr_t ResolveHelperSendQuitAllRva("
):
    fail("frame-stepping profile lost the shared 1.02f quit broadcaster")
if "g_externalLauncherGuardRemoteBase" not in function_slice(
    takeover, "static void CancelSessionUnlocked("
):
    fail("external cancel path lost its narrow remote peer-quit base")

run_broadcast = function_slice(takeover, "DWORD RunInjectedPeerQuitBroadcast(")
for required in (
    "GetExternalLauncherGuardChildProcessId(",
    "hostPid == 0",
    "kInjectedPeerQuitBroadcastResultInvalidGuardContext",
):
    if required not in run_broadcast:
        fail(f"narrow guard peer-quit path is not fail-closed: {required}")

print("Revival launcher adoption contract: PASS")
