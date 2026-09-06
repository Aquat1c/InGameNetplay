#!/usr/bin/env python3
"""Contract for publishing a clean status snapshot on consecutive starts."""

from __future__ import annotations

import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"Rehost status reset contract: {message}", file=sys.stderr)
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


header = read("include/netplay/bridge/session_bridge.h")
for default in (
    "int role = -1;",
    "int roleFlag = -1;",
    "int syncGameMode = -1;",
    "int delayPromptServedSerial = 0;",
    "int delaySetupReady = 0;",
    "int vsHumanSyncReady = 0;",
    "uint32_t processId = 0;",
):
    if default not in header:
        fail(f"NetbridgeStatus lost clean-session default: {default}")

source = read("src/netplay/bridge/session_bridge.cpp")
start = function_slice(source, "static bool StartSessionInternal(")
reset_at = start.find("g_status = {};")
role_at = start.find("g_status.role = static_cast<int>(role);", reset_at)
port_at = start.find("g_status.port = port;", role_at)
address_at = start.find("g_status.address", port_at)
connecting_at = start.find("SetPhase(NetbridgePhase::Connecting", address_at)
worker_at = start.find("g_startWorker = std::thread", connecting_at)
if not (
    0 <= reset_at < role_at < port_at < address_at < connecting_at < worker_at
):
    fail("accepted request is not reset/populated/published before worker start")

# The reset must happen only after endpoint/family validation accepted the
# request. Otherwise an invalid second attempt would erase the visible error or
# current-session status before StartSession returns false.
validation_at = start.find("const bool remoteRole")
host_validation_at = start.find("else if (role == NetbridgeRole::Host")
if not (0 <= validation_at < host_validation_at < reset_at):
    fail("status reset moved ahead of synchronous endpoint/family validation")

queued_window = start[reset_at:worker_at]
for stale_field in (
    "delaySetupReady",
    "delayPromptServedSerial",
    "vsHumanSyncReady",
    "processId",
    "consoleErrorSerial",
):
    if f"g_status.{stale_field} =" in queued_window:
        fail(f"queued snapshot repopulates stale session field: {stale_field}")

print("Rehost status reset contract: PASS")
