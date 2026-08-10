#!/usr/bin/env python3
"""Static guardrails for the active rollback simulation boundary."""

from __future__ import annotations

import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"simulation parity contract: {message}", file=sys.stderr)
    raise SystemExit(1)


root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
revival_path = root / "src/netplay/bridge/revival_memory.cpp"
console_path = root / "src/netplay/bridge/console_capture.cpp"
takeover_path = root / "src/netplay/bridge/revival_takeover.cpp"
ipc_path = root / "src/netplay/bridge/ipc_shared.cpp"
process_inject_path = root / "src/netplay/bridge/process_inject.cpp"
session_bridge_path = root / "src/netplay/bridge/session_bridge.cpp"
title_flow_path = root / "src/netplay/hooks/title_flow.cpp"
player_rooms_path = root / "src/netplay/hooks/player_rooms_menu.cpp"
frontend_return_path = root / "src/netplay/bridge/frontend_return.cpp"
async_host_path = root / "src/netplay/bridge/async_hosting.cpp"
logger_path = root / "src/logger.cpp"
lobby_path = root / "src/netplay/core/lobby_client.cpp"


def function_slice(source: str, signature: str) -> str:
    """Return a simple C++ function definition, skipping declarations."""
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
                return source[start : index + 1]
    fail(f"unterminated function: {signature}")
    return ""  # unreachable

for retired_path in (
    root / "include/netplay/bridge/batch_stabilizer.h",
    root / "src/netplay/bridge/batch_stabilizer.cpp",
):
    if retired_path.exists():
        fail(f"retired rollback-batch detour returned: {retired_path}")

runtime_text = "\n".join(
    path.read_text(encoding="utf-8", errors="strict")
    for tree in (root / "include", root / "src")
    for path in tree.rglob("*")
    if path.suffix in {".h", ".cpp"}
)
for retired_symbol in (
    "batch_stabilizer",
    "BatchStabilizerWorkKB",
    "BatchStabilizerWorkKb",
    "DeferredConsoleParse",
    "Revival102jSafeReadInputHook",
    "InstallRevival102jSafeInputReadPatch",
    "consoleDesyncWarn",
    "PublishConsoleDesyncWarning",
    "ConsumeLocalBattleEscQuitRingIgnore",
    "EnsureCharSelectEntryHoldHook",
    "ArmCharSelectEntryHold",
    "HookedCharSelectUpdate",
    "g_charSelectEntryHold",
    "g_charSelectUpdateSlotAddress",
    "EnsureReplayScreenHook",
    "ArmSpectateReplayBypass",
    "HookedReplayScreenUpdate",
    "g_spectateReplayBypass",
    "g_replayUpdateSlotAddress",
):
    if retired_symbol in runtime_text:
        fail(f"retired simulation hook/settings symbol returned: {retired_symbol}")

revival = revival_path.read_text(encoding="utf-8", errors="strict")
hook_start = revival.find("static int __fastcall OurPerFrameTickHook")
hook_end = revival.find("\nbool IsInsideFrameTick()", hook_start)
if hook_start < 0 or hook_end < 0:
    fail("could not locate OurPerFrameTickHook")
hook = revival[hook_start:hook_end]
dispatch = "result = RunPerFrameTickDispatch(exeThis);"
if hook.count(dispatch) != 1:
    fail("native Revival tick must be dispatched exactly once")
dispatch_at = hook.find(dispatch)
for pre_native_forbidden in (
    "QueryPerformanceCounter",
    "MonitorScreenIndexChange",
    "mod::Log",
    "ReadSessionPtrRaw",
    "CaptureRevivalRemoteInputDiag",
    "IsEagerZeroFrameGraphicsRestoreEnabled",
    "GetTickCount",
    "GetStatus",
    "OpenFileMapping",
    "MapViewOfFile",
):
    token_at = hook.find(pre_native_forbidden)
    if token_at >= 0 and token_at < dispatch_at:
        fail(f"{pre_native_forbidden} moved before the native tick")
if "TickExportOnly(" in hook:
    fail("rollback hook must not poll shared-state export")

parity_guard = "if (!NeedsPostNativeControlPath(nativeTickSkippedForFatalError))"
guard_at = hook.find(parity_guard, dispatch_at)
heavy_path_at = hook.find("// From this point onward all work is mod-side/post-native.")
if guard_at < 0 or heavy_path_at < 0 or guard_at > heavy_path_at:
    fail("bounded post-native parity return is missing")
guard_region = hook[dispatch_at:heavy_path_at]
if "return result;" not in guard_region:
    fail("ordinary battle path does not return before control-plane work")

parity_decision = function_slice(revival, "static bool NeedsPostNativeControlPath(")
for forbidden in (
    "QueryPerformanceCounter",
    "QueryPerformanceFrequency",
    "GetTickCount",
    "GetStatus",
    "IsPeerProcessAlive",
    "OpenFileMapping",
    "MapViewOfFile",
    "mod::Log",
    "std::",
    "IsPeerProcessAlive",
):
    if forbidden in parity_decision:
        fail(f"parity decision regained variable work: {forbidden}")
if "HasPeerProcessExitSignal()" not in parity_decision:
    fail("parity decision lost the event-driven helper-exit signal")

dispatch_wrapper = function_slice(revival, "static int RunPerFrameTickDispatch(")
if dispatch_wrapper.count("g_origPerFrameTick(fixedThis)") != 1:
    fail("setjmp wrapper must call the native tick exactly once")

console = console_path.read_text(encoding="utf-8", errors="strict")
producer = function_slice(console, "void LogConsoleTextChunk(")
for forbidden in (
    "ProcessConsoleTextChunk",
    "IsLikelyTextChunk",
    "std::string",
    "std::lock_guard",
    ".notify_",
):
    if forbidden in producer:
        fail(f"raw console producer regained variable work: {forbidden}")

enqueue = function_slice(console, "static bool EnqueueConsoleParseChunk(")
for forbidden in (
    "std::string",
    "std::mutex",
    "std::lock_guard",
    ".notify_",
    "ProcessConsoleTextChunk",
    "mod::Log",
):
    if forbidden in enqueue:
        fail(f"console ingress regained blocking/allocating work: {forbidden}")

for required in (
    "g_consoleIngressEnqueuePosition.compare_exchange_strong",
    "g_consoleIngressLossGeneration.fetch_add",
    "slot.sequence.store",
):
    if required not in enqueue:
        fail(f"console ingress lost required bounded-ring primitive: {required}")

write_file_capture = function_slice(console, "void MaybeLogConsoleOutputChunk(")
for forbidden in (
    "IsLikelyTextChunk",
    "GetFileType",
    "TryGetDiskFilePathFromHandle",
    "std::string",
    "std::lock_guard",
    "mod::Log",
):
    if forbidden in write_file_capture:
        fail(f"WriteFile capture regained producer-side classification: {forbidden}")

takeover = takeover_path.read_text(encoding="utf-8", errors="strict")
host_log_hook = function_slice(takeover, "void EnsureHostLogEfzIatPatched(")
for forbidden in ("PatchIat", "nb_stub_WriteFile", "nb_stub_CreateFile"):
    if forbidden in host_log_hook:
        fail(f"host simulation log IAT capture returned: {forbidden}")

ipc = ipc_path.read_text(encoding="utf-8", errors="strict")
injected_init = function_slice(ipc, "void InitializeInjected(")
if "StartConsoleCaptureWorker(true)" not in injected_init:
    fail("injected helper capture ring has no local consumer worker")
if "helperCaptureReady" not in injected_init:
    fail("injected helper does not acknowledge capture-worker readiness")
injected_shutdown = function_slice(ipc, "void ShutdownInjected(")
if "StopManagedLogEfzWorker(false)" not in injected_shutdown:
    fail("injected helper capture worker is not stopped on process detach")

start_watch = function_slice(ipc, "bool StartPeerProcessExitWatch(")
clear_at = start_watch.find(
    "InterlockedExchange(&g_peerProcessExitSignaled, 0)")
thread_at = start_watch.find("g_peerProcessExitWatchThread = std::thread(")
if thread_at < 0:
    thread_at = start_watch.find("watcherThread = CreateThread(")
if clear_at < 0 or thread_at < 0 or clear_at > thread_at:
    fail("helper-exit signal must be cleared before watcher launch")
if "if (watcherThread == nullptr)" not in start_watch:
    fail("helper-exit watcher creation does not fail closed")

start_session = function_slice(takeover, "bool StartSession(")
if "if (!StartPeerProcessExitWatch())" not in start_session:
    fail("session start does not fail closed when helper-exit watch is unavailable")
if "g_hostBlock->helperCaptureReady" not in start_session:
    fail("host resumes helper without capture-worker readiness barrier")
if "resumeResult == static_cast<DWORD>(-1)" not in start_session:
    fail("helper ResumeThread failure is not rejected")
emergency_shutdown = function_slice(takeover, "void EmergencyShutdownHost(")
if "CloseProcessHandle(nullptr, false)" not in emergency_shutdown:
    fail("loader-lock shutdown can block joining the helper-exit watcher")

# The late helper-IAT safety retry runs on the title thread every 500 ms until
# the create path is observed. Already-patched slots must remain read-only;
# changing page protection for all of them caused measured title-frame hitches.
process_inject = process_inject_path.read_text(
    encoding="utf-8", errors="strict")
patch_iat_module = function_slice(process_inject, "bool PatchIatModule(")
slot_compare_at = patch_iat_module.find("if (oldAddress != newAddress)")
protect_at = patch_iat_module.find("VirtualProtectEx")
if slot_compare_at < 0 or protect_at < 0 or protect_at < slot_compare_at:
    fail("late IAT retry can change protection on an already-patched slot")
read_remote_string = function_slice(process_inject, "bool ReadRemoteString(")
bulk_read_at = read_remote_string.find("bulkSize")
fallback_loop_at = read_remote_string.find("while (index + 1 < outSize)")
if bulk_read_at < 0 or fallback_loop_at < 0 or bulk_read_at > fallback_loop_at:
    fail("remote import-name reads lost their bounded bulk fast path/fallback")

if "g_remoteInjectedPatchMap = std::move(patches)" not in takeover:
    fail("session startup no longer retains its immutable remote IAT patch map")
late_patch_rebuild = (
    "const std::unordered_map<std::string, uint32_t> patches = "
    "BuildPatchMap(g_remoteInjectedSelfBase)"
)
if late_patch_rebuild in takeover:
    fail("late IAT retry rebuilds the complete patch map every 500 ms")

# QueryPerformanceFrequency is invariant for the lifetime of Windows. Keep it
# in bridge initialization rather than paying for it in every title/menu tick.
session_bridge = session_bridge_path.read_text(
    encoding="utf-8", errors="strict")
session_tick = function_slice(session_bridge, "void Tick(")
if "QueryPerformanceFrequency" in session_tick:
    fail("session bridge re-queries the QPC frequency every title frame")

for writer_signature in (
    "BOOL StubWriteFile(",
    "BOOL StubWriteConsoleA(",
    "BOOL StubWriteConsoleW(",
    "BOOL StubWriteConsoleOutputCharacterA(",
    "BOOL StubWriteConsoleOutputCharacterW(",
    "VOID StubOutputDebugStringA(",
    "VOID StubOutputDebugStringW(",
):
    writer = function_slice(
        (root / "src/netplay/bridge/iat_stubs.cpp").read_text(
            encoding="utf-8", errors="strict"),
        writer_signature,
    )
    if "EnsureInjectedContextFast()" not in writer:
        fail(f"helper write can outrun capture-worker readiness: {writer_signature}")

state_export_source = (
    root / "src/netplay/bridge/netplay_state_export.cpp"
).read_text(encoding="utf-8", errors="strict")
queue_update = function_slice(state_export_source, "void QueueUpdate(")
pending_at = queue_update.find("InterlockedExchange(&g_updateRequestPending, 1)")
unlock_at = queue_update.find("LeaveCriticalSection(&g_updateRequestLock)")
if pending_at < 0 or unlock_at < 0 or pending_at > unlock_at:
    fail("state-export request is published outside its suspension barrier lock")
suspend_export = function_slice(
    state_export_source, "bool SuspendForOnlineSimulation(")
for required in (
    "g_updateInFlight",
    "g_updateRequestPending",
):
    if required not in suspend_export:
        fail(f"state-export suspension does not drain: {required}")
if suspend_export.count("g_updateInFlight") < 2:
    fail("state-export suspension lacks a stable double-read quiescence check")

title_flow = title_flow_path.read_text(encoding="utf-8", errors="strict")
for signature in (
    "void HandoffSpectateSession(",
    "void HandoffConnectedSessionToVsHumanState(",
):
    handoff = function_slice(title_flow, signature)
    suspend_at = handoff.find("if (!SuspendUiHooksForOnlineSimulation(")
    first_mutation = min(
        position for position in (
            handoff.find("RunTransitionFadeOut("),
            handoff.find("PrepareVsHumanGameState("),
        ) if position >= 0
    )
    if suspend_at < 0 or suspend_at > first_mutation:
        fail(f"{signature} does not verify UI teardown before state mutation")

suspend_ui = function_slice(
    title_flow, "static bool SuspendUiHooksForOnlineSimulation(")
for required in (
    "TickExportOnly(true)",
    "state_export::SuspendForOnlineSimulation()",
    "RemoveNetplayWindowHook()",
    "InstallNetplayWindowHook(screenContext)",
    "debug_overlay::SuspendForOnlineSimulation()",
    "ShutdownRenderOverlay()",
    "SuspendUpdateHooksForOnlineSimulation()",
    "windowOk && imguiOk && renderOk && frontendOk",
):
    if required not in suspend_ui:
        fail(f"online handoff does not verify teardown component: {required}")

frontend_return = frontend_return_path.read_text(encoding="utf-8", errors="strict")
suspend_updates = function_slice(
    frontend_return, "bool SuspendUpdateHooksForOnlineSimulation(")
for required in (
    "FrontendReturnLoadingUpdateThunk",
    "FrontendReturnBattleUpdateThunk",
    "FrontendReturnResultUpdateThunk",
    "RestoreScreenUpdateHook",
):
    if required not in suspend_updates:
        fail(f"online handoff no longer restores screen-update hook: {required}")

# EFZ's native timing thread only signals an auto-reset pacing event; the game
# simulation itself remains normal-priority.  Disk flushes and lobby keepalive
# work that coexist with battle must therefore yield to the game without
# changing their queues or network cadence.
logger = logger_path.read_text(encoding="utf-8", errors="strict")
writer = function_slice(logger, "void WriterThreadEntry(")
for required in ("SetThreadPriority", "THREAD_PRIORITY_BELOW_NORMAL"):
    if required not in writer:
        fail(f"logger writer lost game-thread scheduling isolation: {required}")

lobby = lobby_path.read_text(encoding="utf-8", errors="strict")
poll_worker = function_slice(lobby, "void LobbySession::PollThreadEntry(")
priority_at = poll_worker.find("THREAD_PRIORITY_BELOW_NORMAL")
joined_at = poll_worker.find("LobbySession::PollThread: joined lobby")
loop_at = poll_worker.find("while (!m_shouldStop.load())")
if priority_at < 0 or joined_at < 0 or loop_at < 0:
    fail("could not verify lobby poll scheduling boundary")
if priority_at < joined_at or priority_at > loop_at:
    fail("lobby poll priority must change only after join and before keepalive polling")

public_ip_sources = function_slice(lobby, "bool TryDiscoverPublicIpForFamily(")
if public_ip_sources.count("THREAD_PRIORITY_BELOW_NORMAL") < 3:
    fail("parallel public-IP source workers can contend at game priority")
public_ip_owner = function_slice(
    lobby, "void LobbySession::StartPublicIpDiscoveryAsync(")
if "THREAD_PRIORITY_BELOW_NORMAL" not in public_ip_owner:
    fail("public-IP owner worker can contend at game priority")

for discovery_worker in (
    "DWORD WINAPI HostDiscoveryThreadMain(",
    "DWORD WINAPI LocalNetworkCapabilityThreadMain(",
):
    worker = function_slice(title_flow, discovery_worker)
    if "THREAD_PRIORITY_BELOW_NORMAL" not in worker:
        fail(f"menu discovery worker can contend at game priority: {discovery_worker}")

# Player-room requests used to be detached C++ threads with no module lease.
# A request surviving menu exit could therefore execute unmapped DLL code.
player_rooms = player_rooms_path.read_text(encoding="utf-8", errors="strict")
if ".detach()" in player_rooms or "std::thread" in player_rooms:
    fail("player-room worker regained an unload-unsafe detached thread")
room_worker_launch = function_slice(player_rooms, "bool LaunchRoomWorker(")
for required in (
    "GetModuleHandleExA",
    "GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS",
    "CreateThread",
):
    if required not in room_worker_launch:
        fail(f"player-room worker lost module-safe launch primitive: {required}")
room_worker = function_slice(player_rooms, "DWORD WINAPI RoomWorkerThreadMain(")
if "FreeLibraryAndExitThread" not in room_worker:
    fail("player-room worker does not release its module atomically with exit")

# Listener startup failure is terminal for one hosting attempt.  Re-logging the
# same state on every title frame adds unbounded queue/disk work while the UI is
# already reporting a single failure.
async_host = async_host_path.read_text(encoding="utf-8", errors="strict")
async_tick = function_slice(async_host, "void Tick(")
failure_branch = async_tick.find("ASYNC_HOST_START_FAILED_BEFORE_LISTENER")
if failure_branch < 0:
    fail("could not locate async-host pre-listener failure branch")
failure_guard = async_tick[max(0, failure_branch - 500):failure_branch]
if "!g_listenerStartupFailed" not in failure_guard:
    fail("async-host pre-listener failure can log every title frame")

print("simulation parity contract: PASS")
