#!/usr/bin/env python3
"""Contract for keeping a lobby room open outside the Lobby screen, and for the
export staying off the simulation thread while it reports online screens."""

from __future__ import annotations

import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"Lobby background contract: {message}", file=sys.stderr)
    raise SystemExit(1)


root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()


def read(relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8", errors="strict")


def function_slice(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        fail(f"could not locate function: {signature}")
    opening = source.find("{", start)
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


title_flow = read("src/netplay/hooks/title_flow.cpp")
async_host = read("src/netplay/bridge/async_hosting.cpp")
export = read("src/netplay/bridge/netplay_state_export.cpp")
revival = read("src/netplay/bridge/revival_memory.cpp")
frontend = read("src/netplay/bridge/frontend_return.cpp")

# Challenge hosts must be async hosts, or minimizing has nothing to keep alive.
if title_flow.count("HostOrigin::LobbyChallenge);") < 2:
    fail("a lobby challenge start path does not register with async_host")

rehost = function_slice(async_host, "void AutoRehost(const char* reason)")
guard_at = rehost.find("g_origin == HostOrigin::LobbyChallenge")
restart_at = rehost.find("StartHostSession(")
if guard_at < 0 or restart_at < 0 or guard_at > restart_at:
    fail("a challenge with a published endpoint can be rehosted silently")

switch_to = function_slice(title_flow, "void SwitchToMenu(uint32_t screenContext")
if "if (g_lobbySession && !g_lobbyBackgrounded)" not in switch_to:
    fail("leaving the Lobby screen closes a backgrounded room")

leave = function_slice(title_flow, "void LeaveNetplayMenu(uint32_t screenContext")
if "!keepHost && !g_lobbyBackgrounded" not in leave:
    fail("leaving the netplay menu closes a backgrounded room")

enter = function_slice(title_flow, "void EnterNetplayMenu(uint32_t screenContext")
challenge_at = enter.find("HostOrigin::LobbyChallenge")
host_screen_at = enter.find("SwitchToMenu(screenContext, NetplayMenuId::Host")
if challenge_at < 0 or host_screen_at < 0 or challenge_at > host_screen_at:
    fail("return from a minimized challenge can reach the Host screen and close the room")

restore = function_slice(title_flow, "static void RestoreHostingOverlayFromAsyncHost()")
if "challengeMode = true" not in restore or "targetName" not in restore:
    fail("restoring a minimized challenge loses its overlay state")

execute = function_slice(title_flow, "void ExecuteNetplayAction(uint32_t screenContext")
open_host_at = execute.find("case NetplayMenuAction::OpenHost:")
confirm_at = execute.find("RequestStopHostingConfirmIfHosting", open_host_at)
next_case_at = execute.find("case NetplayMenuAction::OpenJoin:", open_host_at)
if not (0 <= open_host_at < confirm_at < next_case_at):
    fail("opening Host does not ask to leave a backgrounded room")

update = function_slice(title_flow, "char UpdateNetplayMenu(uint32_t screenContext")
for token in (
    "g_stopHostingConfirm.leaveRoom",
    "g_pendingRoomLeaveAction.active = true",
    "BeginLobbySessionShutdown(false)",
):
    if token not in update:
        fail(f"leave-room confirmation is incomplete: {token}")
if "g_lobbySessionShutdown.load() == LobbyShutdownPhase::Idle" not in update:
    fail("the pending action can run while the room is still closing")

if "TickBackgroundLobby();" not in frontend:
    fail("a backgrounded room is not serviced during gameplay")
tick = function_slice(title_flow, "void TickBackgroundLobby()")
if "g_onlineSimulationUiSuspended" not in tick:
    fail("background room work is not excluded from the online simulation")

# The heartbeat runs beside the live simulation: one byte, no pointer walks.
heartbeat = function_slice(export, "void PublishSuspendedHeartbeat()")
for forbidden in ("UpdateNow(", "ReadScreenObject(", "ReadGameSystemPtr(", "ReadScores(",
                  "ReadCharSelectContext(", "reinterpret_cast<const uint32_t*>"):
    if forbidden in heartbeat:
        fail(f"suspended heartbeat reads more than the screen byte: {forbidden}")
if "ReadScreenIndex()" not in heartbeat:
    fail("suspended heartbeat does not derive its screen from the screen byte")

worker = function_slice(export, "void ExportWorkerMain()")
if "kSuspendedHeartbeatMs" not in worker:
    fail("export worker does not heartbeat while suspended")

# Boundary reads stay on the game thread and out of battle.
hook = function_slice(revival, "static int __fastcall OurPerFrameTickHook(")
guard = hook.find("if (!NeedsPostNativeControlPath(nativeTickSkippedForFatalError))")
boundary = hook.find("PublishOnlineBoundary()")
if guard < 0 or boundary < 0 or boundary < guard:
    fail("online boundary publish can run on the ordinary battle path")

# Revival sets these on screen entry: character-select IDs are stale on entry,
# and the live delay is a ping estimate until battle applies the configured one.
char_select = function_slice(export, "static bool ReadCharSelectContext(")
for required in ("kCharSelectP1State", "kCharSelectGridMap", "kCharSelectP1Timer"):
    if required not in char_select:
        fail(f"character reads ignore the selection state: {required}")
if "p1Timer != 0" in char_select:
    fail("the Random spinner timer is reported as a character lock")
if "OnlineCharSelectChanged()" not in hook[guard:]:
    fail("character-select changes are not published during online sessions")
boundary_publish = function_slice(
    read("src/netplay/bridge/session_bridge.cpp"), "bool PublishOnlineBoundary()")
if "ReadConfiguredInputDelay(" not in boundary_publish:
    fail("a boundary publishes the pre-battle ping estimate as the input delay")
if "s_exportBoundaryPublishes = 2" not in hook:
    fail("a boundary can read before Revival's handler for that screen has run")

# Old PCs: reuse the poll connection, keep Debug logs off XP, bound the append.
lobby = read("src/netplay/core/lobby_client.cpp")
if "shutdownRequest ? nullptr : &m_keepAlive" not in lobby:
    fail("lobby polling does not reuse its TLS connection")
ipc = read("src/netplay/bridge/ipc_shared.cpp")
write_ini = function_slice(ipc, "bool WriteIni(")
if "IsWindowsXpFamily()" not in write_ini:
    fail("XP sessions do not turn off Revival debug logging")
options = read("src/netplay/hooks/options_menu.cpp")
if "IsWindowsXpFamily()" not in options:
    fail("the Debug option is still offered on XP")
stubs = read("src/netplay/bridge/iat_stubs.cpp")
if "kLogNetAppendLimitBytes" not in stubs:
    fail("logNet.txt can grow without limit")

# The return key must not abandon a load.
poll_key = function_slice(async_host, "void PollReturnKey()")
lockout_at = poll_key.find("kReturnKeyLoadLockoutMs")
begin_at = poll_key.find("BeginReturnToFrontend(")
if lockout_at < 0 or begin_at < 0 or lockout_at > begin_at:
    fail("the return key can start a return during or right after loading")

# Character select and other screens without their own tick.
if "!screenHasOwnTick" not in revival:
    fail("background work does not run on character select")
if "SND_ASYNC" not in tick:
    fail("incoming challenges are silent outside the menu")

print("Lobby background contract: PASS")
