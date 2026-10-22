#!/usr/bin/env python3
"""Static contracts separating the two exact EfzRevival 1.02j compiles."""

from __future__ import annotations

import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"Revival compile-profile contract: {message}", file=sys.stderr)
    raise SystemExit(1)


root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()


def read(relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8", errors="strict")


def function_slice(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        fail(f"could not locate function: {signature}")
    opening = source.find("{", start)
    if opening < 0:
        fail(f"could not locate function body: {signature}")
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
for required in (
    "constexpr RevivalAddressProfile kRevival_1_02j = {",
    "constexpr RevivalAddressProfile kRevival_1_02j_20260802 = {",
    "&kRevival_1_02j,",
    "&kRevival_1_02j_20260802,",
    "uintptr_t initExportRva;",
    "uint32_t revivalDllSizeOfImage;",
    "uint32_t revivalDllEntryPointRva;",
    "uintptr_t replaySessionVtableRva;",
    "uintptr_t tournamentExitGuardRva;",
    "uintptr_t practicePostInitRva;",
    "uintptr_t spectatorPostInitRva;",
    "uintptr_t memorialPauseIntegrationRva;",
    "uintptr_t rollbackReadInputRva;",
    "uintptr_t tournamentDeletingDtorRva;",
    "uintptr_t onlineDeletingDtorRva;",
    "uintptr_t spectatorDeletingDtorRva;",
    "uintptr_t replayDeletingDtorRva;",
    "uintptr_t practiceDeletingDtorRva;",
    "bool usesProceduralDdrawTextApi;",
    "uint32_t companionDdrawFileSize;",
    "const char* companionDdrawSha256;",
):
    if required not in addresses:
        fail(f"compile-profile schema/table lost: {required}")


takeover = read("src/netplay/bridge/revival_takeover.cpp")
old_verifier = function_slice(
    takeover, "static bool VerifyRevival102jBinary(")
new_verifier = function_slice(
    takeover, "static bool VerifyRevival102j20260802Binary(")
selector = function_slice(
    takeover, "static const RevivalAddressProfile* FindLoadedRevivalProfile(")

if "kRevival_1_02j_20260802" in old_verifier:
    fail("the original 1.02j verifier is coupled to the new compile")
if "const RevivalAddressProfile& profile = kRevival_1_02j_20260802;" \
        not in new_verifier:
    fail("the new compile does not have its own strict verifier")
for required in (
    "profile.revivalDllSizeOfImage",
    "profile.revivalDllEntryPointRva",
    "profile.initExportRva",
    "profile.frameHookRva",
    "profile.perFrameTickRva",
    "profile.inputSwapPairRva",
    "profile.startInitPlayerRva",
    "profile.clearTextRva",
    "profile.setTextEnabledRva",
    "profile.roleFlagOffsets[0]",
    "profile.sessionPtrOffsets[0]",
    "profile.globalStatePtrOffset",
    "profile.timerPtrOffset",
    "profile.renderContextBaseOffset",
    "profile.tournamentSessionVtableRva",
    "profile.onlineSessionVtableRva",
    "profile.spectatorSessionVtableRva",
    "profile.replaySessionVtableRva",
    "profile.practiceSessionVtableRva",
    "profile.tournamentExitGuardRva",
    "profile.spectatorPostInitRva",
    "profile.tournamentInputQueueOffset",
):
    if required not in new_verifier:
        fail(f"new strict verifier lost exact profile anchor: {required}")
for required in (
    "ModuleBytesMatchOrInstalledJmp",
    "ModuleBytesMatchMasked",
    "ModuleDirectCallCountEquals",
    "ModuleVtableSlotsMatch",
    "ModuleWindowContainsU32",
    "kNativeGuard",
    "kOwnedGuard",
    "0xC2, 0x04, 0x00",
):
    if required not in new_verifier:
        fail(f"new strict verifier lost byte/ownership proof: {required}")
if 'profile.setTextEnabledRva, 22)' not in new_verifier:
    fail("new strict verifier no longer checks the complete stdcall ret 4 wrapper")

old_branch = selector.find("timestamp == kRevival_1_02j.peTimestamp")
old_verify = selector.find("VerifyRevival102jBinary(module)", old_branch)
new_branch = selector.find(
    "timestamp == kRevival_1_02j_20260802.peTimestamp", old_verify)
new_verify = selector.find(
    "VerifyRevival102j20260802Binary(module)", new_branch)
new_profile = selector.find("&kRevival_1_02j_20260802", new_verify)
generic_fallback = selector.find(
    "return FindRevivalProfileByTimestamp(timestamp);", new_profile)
if not (
    0 <= old_branch < old_verify < new_branch < new_verify
    < new_profile < generic_fallback
):
    fail("loaded-module detection no longer routes each 1.02j timestamp through its own verifier")


probe = read("src/netplay/bridge/revival_launcher_probe.cpp")
dll_match = function_slice(probe, "bool ProfileMatchesDllFile(")
for required in (
    "profile.revivalDllFileSize",
    "profile.revivalDllSha256",
    "profile.companionDdrawFileSize",
    "profile.companionDdrawSha256",
    'GetModuleHandleA("Ddraw.dll")',
    "GetModuleFileNameA(ddraw, ddrawPath, MAX_PATH)",
    "ReadFileSha256(ddrawPath, &ddrawFileSize, &ddrawSha256)",
):
    if required not in dll_match:
        fail(f"DLL admission lost exact Ddraw pairing step: {required}")
revival_hash_at = dll_match.find("const bool revivalMatches")
revival_reject_at = dll_match.find("if (!revivalMatches)", revival_hash_at)
optional_pair_at = dll_match.find(
    "profile.companionDdrawFileSize == 0", revival_reject_at)
ddraw_load_at = dll_match.find('GetModuleHandleA("Ddraw.dll")', optional_pair_at)
ddraw_hash_at = dll_match.find("ReadFileSha256(ddrawPath", ddraw_load_at)
if not (
    0 <= revival_hash_at < revival_reject_at < optional_pair_at
    < ddraw_load_at < ddraw_hash_at
):
    fail("Ddraw is not checked only after the exact Revival DLL and optional-pair gate")


revival = read("src/netplay/bridge/revival_memory.cpp")
likely_session = function_slice(revival, "bool IsLikelySessionPointer(")
for field in (
    "tournamentSessionVtableRva",
    "onlineSessionVtableRva",
    "spectatorSessionVtableRva",
    "replaySessionVtableRva",
    "practiceSessionVtableRva",
):
    if f"g_activeRevival->{field}" not in likely_session:
        fail(f"1.02j object validation still lacks profile field: {field}")

for signature in (
    "static bool SaveAndApplyRevival102jExitProcessPatches(",
    "bool SaveAndApplyExternalTournamentExitGuard(",
    "bool IsExternalTournamentExitGuardOwned(",
    "bool RestoreDllExitProcessPatches(",
):
    body = function_slice(revival, signature)
    if "g_activeRevival->tournamentExitGuardRva" not in body:
        fail(f"Tournament exit guard is not profile-driven in {signature}")

destroy = function_slice(revival, "bool DestroyCurrentSession(")
for field in (
    "tournamentDeletingDtorRva",
    "onlineDeletingDtorRva",
    "spectatorDeletingDtorRva",
    "replayDeletingDtorRva",
    "practiceDeletingDtorRva",
):
    if f"g_activeRevival->{field}" not in destroy:
        fail(f"session destruction is not profile-driven for {field}")
if "0x00048550u" in destroy or "0x00049490u" in destroy:
    fail("1.02j Tournament destruction regressed to a compile-specific literal")

practice_post_init = function_slice(
    revival, "static bool InvokeRevival102jLocalPostInit(")
for field in ("practiceSessionVtableRva", "practicePostInitRva"):
    if f"g_activeRevival->{field}" not in practice_post_init:
        fail(f"Practice post-init is not profile-driven for {field}")
spectator_ready = function_slice(
    revival, "bool IsRevival102jSpectatorPostInitReady(")
if "g_activeRevival->spectatorSessionVtableRva" not in spectator_ready:
    fail("Spectator readiness retains the original compile's vtable")
spectator_post_init = function_slice(
    revival, "bool InvokeRevival102jSpectatorPostInit(")
for field in ("spectatorSessionVtableRva", "spectatorPostInitRva"):
    if f"g_activeRevival->{field}" not in spectator_post_init:
        fail(f"Spectator post-init is not profile-driven for {field}")
graphics_restore = function_slice(
    revival, "static bool ApplyRevival102jGraphicsPatchSetEnabled(")
if "g_activeRevival->memorialPauseIntegrationRva" not in graphics_restore:
    fail("1.02j graphics restore retains the original compile's pause RVA")


save_render = function_slice(revival, "bool SaveRenderContext(")
procedural_at = save_render.find(
    "g_activeRevival->usesProceduralDdrawTextApi")
module_at = save_render.find('GetModuleHandleA("EfzRevival.dll")')
global_at = save_render.find("renderContextGlobalOffset")
for required in (
    "g_savedRenderContext = 0;",
    "g_renderContextSaved = false;",
    "return true;",
):
    if required not in save_render[procedural_at:module_at]:
        fail(f"procedural SaveRenderContext is not a no-save success: {required}")
if not (0 <= procedural_at < global_at < module_at):
    fail("procedural SaveRenderContext can touch the removed renderer global")

restore_render = function_slice(
    revival, "static bool RestoreRenderContextInternal(")
procedural_at = restore_render.find(
    "g_activeRevival->usesProceduralDdrawTextApi")
saved_at = restore_render.find("g_savedRenderContext == 0")
if not (0 <= procedural_at < saved_at) \
        or "return true;" not in restore_render[procedural_at:saved_at]:
    fail("procedural RestoreRenderContext is not an early no-write success")

clear_text = function_slice(revival, "bool ClearRevivalText(")
if clear_text.count("!g_activeRevival->usesProceduralDdrawTextApi") < 2:
    fail("procedural ClearRevivalText can still save or restore an EfzRender pointer")
clear_current = function_slice(
    revival, "bool ClearRevivalTextWithCurrentRenderContext(")
for required in (
    "g_activeRevival->usesProceduralDdrawTextApi",
    "ClearProceduralTextFn",
    "reinterpret_cast<ClearProceduralTextFn>(fnAddr)();",
):
    if required not in clear_current:
        fail(f"procedural clear-text ABI lost: {required}")

set_current = function_slice(
    revival,
    "static bool SetRevivalTextRenderingEnabledWithCurrentRenderContextInternal(",
)
for required in (
    "g_activeRevival->usesProceduralDdrawTextApi",
    "SetProceduralTextEnabledFn",
    "reinterpret_cast<SetProceduralTextEnabledFn>(fnAddr)(enable ? 1 : 0);",
):
    if required not in set_current:
        fail(f"procedural text-enable ABI lost: {required}")
readback_at = set_current.find("if (IsRevival102jProfile()")
if readback_at >= 0:
    readback_condition_end = set_current.find(")", readback_at)
    readback_prefix = set_current[readback_at:readback_condition_end + 100]
    if "!g_activeRevival->usesProceduralDdrawTextApi" not in readback_prefix:
        fail("procedural text-enable result is rejected by the legacy EfzRender readback")


adoption_test = read("tests/verify_revival_launcher_adoption.py")
if '"0x00048550u"' in adoption_test:
    fail("launcher-adoption test still pins the original 1.02j destructor literal")
if '"tournamentDeletingDtorRva"' not in adoption_test:
    fail("launcher-adoption test no longer requires a profile-selected Tournament destructor")

print("Revival compile-profile contract: PASS")
