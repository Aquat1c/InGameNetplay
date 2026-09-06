#!/usr/bin/env python3
"""Regression contract for managed graceful quit while EFZ is on charselect."""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path


def fail(message: str) -> None:
    print(f"Managed charselect quit contract: {message}", file=sys.stderr)
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


@dataclass
class ManagedCharselectQuitModel:
    exit_intercepted_latch: int = 1
    charselect_exit_flag: int = 0
    current_screen: int = 1
    native_tick_calls: int = 0

    def current_recovery_invocation(self) -> int:
        # The fatal latch suppresses native dispatch in this invocation. Managed
        # recovery takes ownership, clears the latch, and requests charselect's
        # native +45 exit for the following Revival tick.
        if self.exit_intercepted_latch != 1 or self.current_screen != 1:
            fail("model recovery did not start from latch=1 on charselect")
        self.exit_intercepted_latch = 0
        self.charselect_exit_flag = 1
        return 0  # session_frame_tick callback EAX is discarded by the EXE hook

    def next_revival_tick(self) -> None:
        if self.exit_intercepted_latch != 0:
            fail("next tick remained blocked by the old interception latch")
        self.native_tick_calls += 1
        if self.current_screen != 1 or self.charselect_exit_flag != 1:
            fail("native charselect update did not receive screen=1/+45=1")
        self.charselect_exit_flag = 0
        self.current_screen = 0


model = ManagedCharselectQuitModel()
ignored_callback_result = model.current_recovery_invocation()
if ignored_callback_result != 0:
    fail("model no longer reflects the conventional ignored callback result")
if model.current_screen != 1 or model.charselect_exit_flag != 1:
    fail("current recovery invocation must leave +45 for the next native tick")
if model.native_tick_calls != 0:
    fail("recovery invocation called native a second time")
model.next_revival_tick()
if (model.current_screen, model.charselect_exit_flag, model.native_tick_calls) != (0, 0, 1):
    fail("next Revival tick did not perform exactly one native 1 -> 0 transition")


constants = read("include/netplay/core/constants.h")
if not re.search(r"kOffsetScreenExitState\s*=\s*45\s*;", constants):
    fail("screen exit-state constant is no longer charselect byte +45")

gameplay_exit = read("src/netplay/bridge/gameplay_exit_recovery.cpp")
begin = function_slice(
    gameplay_exit, "bool BeginGameplayExitRecovery(const char* origin)")
if "InterlockedExchange(&takeover::g_revivalExitIntercepted, 0)" not in begin:
    fail("managed Begin no longer clears the old exit-interception latch")

frontend_return = read("src/netplay/bridge/frontend_return.cpp")
native_exit_request = function_slice(frontend_return, "bool RequestNativeExit(")
if "targetContext + netplay::constants::kOffsetScreenExitState, 1" not in native_exit_request:
    fail("frontend recovery no longer writes the native screen +45 exit request")

revival_memory = read("src/netplay/bridge/revival_memory.cpp")
finalize = function_slice(
    revival_memory, "static char FinalizeGracefulQuitTeardown(")
for required in (
    "IsManagedFrontendRecoveryScreen(teardownScreen)",
    "BeginGameplayExitRecovery(",
    "GameplayExitOrigin::QuitRing",
):
    if required not in finalize:
        fail(f"managed graceful-quit route lost contract: {required}")
managed_start = finalize.find("if (IsManagedFrontendRecoveryScreen(teardownScreen))")
legacy_start = finalize.find("NeutralizeRevivalSessionVtable();", managed_start)
if managed_start < 0 or legacy_start < 0:
    fail("could not isolate managed graceful-quit branch")
if "ForceGameModeToTitle(" in finalize[managed_start:legacy_start]:
    fail("graceful quit again bypasses the native charselect +45 exit")

tick_hook = function_slice(
    revival_memory, "static int __fastcall OurPerFrameTickHook(")
if tick_hook.count("RunPerFrameTickDispatch(") != 1:
    fail("per-frame hook no longer has exactly one lexical native dispatch")
if "g_revivalExitIntercepted" not in tick_hook:
    fail("fatal interception latch no longer suppresses the recovery invocation")

for required in (
    "kExeDispatchHookAddr = 0x401642u",
    "0xFF, 0x52, 0x04, 0xA2, 0x48, 0x01, 0x79, 0x00",
):
    if required not in revival_memory:
        fail(f"EXE dispatch ABI evidence changed: {required}")

print("Managed charselect quit contract: PASS")
