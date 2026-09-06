#!/usr/bin/env python3
"""Contract for one-shot manual spectate confirmation across the ACK gap."""

from __future__ import annotations

import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"Spectate prompt ACK latch contract: {message}", file=sys.stderr)
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


# Model the UI-side ownership rule independently of helper scheduling. Once an
# answer for one exact helper/prompt generation is queued, repeated snapshots
# remain suppressed until the helper acknowledges it. A new generation must
# still be eligible, including the common serial=1 consecutive-session case.
PromptKey = tuple[int, int]


def should_open_prompt(
    pending: bool,
    current: PromptKey,
    submitted: PromptKey | None,
) -> bool:
    return pending and current != submitted


first = (4100, 1)
if not should_open_prompt(True, first, None):
    fail("a new pending manual prompt is not eligible to open")

submitted = first  # successful AnswerSpectatePromptChoice queue
if should_open_prompt(True, first, submitted):
    fail("the same unacknowledged prompt can reopen after successful Yes")
if should_open_prompt(False, first, submitted):
    fail("an acknowledged prompt can open")

second_process = (4200, 1)
if not should_open_prompt(True, second_process, submitted):
    fail("a new process generation reusing prompt serial 1 is suppressed")
second_prompt = (4100, 2)
if not should_open_prompt(True, second_prompt, submitted):
    fail("a new prompt serial in the same process is suppressed")
if not should_open_prompt(True, first, None):
    fail("an explicit new-session reset does not admit a fresh prompt")


title_flow = read("src/netplay/hooks/title_flow.cpp")
console_policy = read("include/netplay/bridge/console_handoff_policy.h")

fingerprint = "g_submittedSpectatePromptFingerprint"
if f"uint64_t {fingerprint} = 0;" not in title_flow:
    fail("manual and automatic answers do not share one submitted fingerprint")
if "g_lastAutoAnsweredSpectatePromptFingerprint" in title_flow:
    fail("the obsolete auto-only prompt fingerprint still exists")

manual = function_slice(title_flow, "bool HandleSpectateConfirmOverlayInput(")
answer_call = "netplay::bridge::AnswerSpectatePromptChoice(choice)"
answer_positions: list[int] = []
search_at = 0
while True:
    position = manual.find(answer_call, search_at)
    if position < 0:
        break
    answer_positions.append(position)
    search_at = position + len(answer_call)
if len(answer_positions) != 2:
    fail("manual Join/Wait and Yes/No prompt paths are no longer both covered")
for position in answer_positions:
    next_answer = manual.find(answer_call, position + len(answer_call))
    branch_end = next_answer if next_answer >= 0 else len(manual)
    failure_guard = manual.find("if (!answered)", position, branch_end)
    failure_return = manual.find("return true;", failure_guard, branch_end)
    assignment = manual.find(
        f"{fingerprint} = promptFingerprint;", position, branch_end
    )
    if not (
        0 <= failure_guard < failure_return < assignment < branch_end
    ):
        fail("a successful manual spectate answer is not fingerprinted")

update = function_slice(title_flow, "char UpdateNetplayMenu(")
fingerprint_builder = function_slice(
    title_flow, "uint64_t SpectatePromptFingerprint("
)
for field in (
    "status.processId",
    "status.spectateConfirmPromptSerial",
):
    if field not in fingerprint_builder:
        fail(f"prompt identity lost exact generation field: {field}")
if "static_cast<uint64_t>(status.processId) << 32" not in fingerprint_builder:
    fail("prompt identity no longer keeps the process generation in its high word")

key_start = update.find(
    "const uint64_t promptFingerprint =\n"
    "        SpectatePromptFingerprint(bridgeStatus);"
)
submitted_state = update.find(
    "const bool spectatePromptAnswerSubmitted =", key_start
)
submitted_compare = update.find(
    f"{fingerprint} == promptFingerprint", submitted_state
)
if not (0 <= key_start < submitted_state < submitted_compare):
    fail("current prompt identity is not compared with the submitted generation")

auto_answer = update.find("netplay::bridge::AnswerSpectatePromptChoice(1)", key_start)
auto_success = update.find("if (answered)", auto_answer)
auto_submit = update.find(f"{fingerprint} = promptFingerprint;", auto_answer)
activate = update.find("ActivateSpectateConfirmOverlay(", auto_answer)
if not (0 <= auto_answer < auto_success < auto_submit < activate):
    fail("automatic Yes does not publish the unified submitted fingerprint")

activation_guard_start = update.rfind("if (", auto_submit, activate)
if activation_guard_start < 0:
    fail("could not locate manual prompt activation guard")
activation_guard = update[activation_guard_start:activate]
if "!spectatePromptAnswerSubmitted" not in activation_guard:
    fail("manual prompt activation does not suppress its submitted generation")

manual_dispatch = update.find("HandleSpectateConfirmOverlayInput(", activate)
manual_key = update.find("promptFingerprint", manual_dispatch)
if not (0 <= activate < manual_dispatch < manual_key):
    fail("manual prompt handler is not bound to the displayed prompt generation")

activate_join = function_slice(title_flow, "void ActivateJoiningOverlay(")
if f"{fingerprint} = 0;" not in activate_join:
    fail("a newly activated Join/Spectate attempt inherits the prior prompt latch")
activate_challenge_join = function_slice(
    title_flow, "void ActivateChallengeJoiningOverlay("
)
if f"{fingerprint} = 0;" not in activate_challenge_join:
    fail("a newly activated challenge Join inherits the prior prompt latch")

# The D shortcut is intentionally independent of ordinary Join's redirect
# confirmation. Keep it routed to direct Spectate (Revival menu choice 4).
d_hotkey = update.find("&& joinWaitToSpectatePressed)")
d_action = update.find("StartJoinSpectateIpAction(screenContext, true)", d_hotkey)
bridge_status = update.find(
    "const netplay::bridge::NetbridgeStatus bridgeStatus", d_hotkey
)
if not (0 <= d_hotkey < d_action < bridge_status):
    fail("Join D no longer routes directly to StartJoinSpectateIpAction")

direct_spectate = function_slice(
    title_flow, "bool TryStartWaitToSpectateFromJoinSettings("
)
start_at = direct_spectate.find("netplay::bridge::StartSession(")
role_at = direct_spectate.find(
    "netplay::bridge::NetbridgeRole::Spectate", start_at
)
if not (0 <= start_at < role_at):
    fail("Join D action no longer starts the direct Spectate role")

spectate_case = console_policy.find("case NetbridgeRole::Spectate:")
spectate_choice = console_policy.find('return {4, "4\\r\\n", ""};', spectate_case)
if not (0 <= spectate_case < spectate_choice):
    fail("direct Spectate no longer selects Revival menu choice 4")

print("Spectate prompt ACK latch contract: PASS")
