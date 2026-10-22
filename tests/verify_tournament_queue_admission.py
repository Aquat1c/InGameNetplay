#!/usr/bin/env python3
"""Focused static contracts for external Tournament auto-nav admission."""

from __future__ import annotations

import re
import sys
from pathlib import Path


def fail(message: str) -> None:
    print(f"Tournament queue admission contract: {message}", file=sys.stderr)
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


header = read("include/netplay/bridge/revival_launcher_probe.h")
canonical_match = re.search(
    r"kCanonicalInputs\s*\[[^]]+\]\s*=\s*\{(?P<body>.*?)\};",
    header,
    flags=re.DOTALL,
)
if canonical_match is None:
    fail("canonical 22-input sequence is missing")
canonical = [
    int(value)
    for value in re.findall(r"\b(\d+)u", canonical_match.group("body"))
]
expected = [
    0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0,
    4, 0, 0, 0, 0, 16, 0, 0, 0, 0, 0,
]
if canonical != expected:
    fail(f"canonical input sequence changed: {canonical!r}")

classifier = function_slice(header, "ClassifyTournamentAutoNavSuffix(")
for required in (
    "count > kTournamentAutoNavInputCount",
    "count != 0u && values == nullptr",
    "gameMode == 0",
    "TournamentAutoNavSuffixState::Retry",
    "kTournamentAutoNavInputCount - count",
    "values[i] != kCanonicalInputs[suffixStart + i]",
):
    if required not in classifier:
        fail(f"suffix classifier lost policy: {required}")

probe = read("src/netplay/bridge/revival_launcher_probe.cpp")
msvc = function_slice(probe, "bool ReadMsvcTournamentQueue(")
for required in (
    "ReadMsvcTournamentDequeState(dequeAddress, &before)",
    "kElementsPerBlock = 8u",
    "before.mapSlots & (before.mapSlots - 1u)",
    "SameMsvcTournamentDequeState(before, after)",
):
    if required not in msvc:
        fail(f"legacy MSVC deque reader lost constraint: {required}")

msvc_state = function_slice(probe, "bool ReadMsvcTournamentDequeState(")
for offset in ("+ 4u", "+ 8u", "+ 12u", "+ 16u"):
    if offset not in msvc_state:
        fail(f"legacy MSVC deque layout lost offset: {offset}")

libstdcpp = function_slice(probe, "bool ReadLibstdcppTournamentQueue(")
for required in (
    "kElementsPerBlock = 256u",
    "before.startNode",
    "before.finishNode",
    "SameLibstdcppTournamentDequeState(before, after)",
    "node != before.finishNode || cur != before.finishCur",
):
    if required not in libstdcpp:
        fail(f"1.02j libstdc++ deque reader lost constraint: {required}")

libstdcpp_state = function_slice(
    probe, "bool ReadLibstdcppTournamentDequeState(")
for offset in (
    "+ 0x00u", "+ 0x04u", "+ 0x08u", "+ 0x0Cu", "+ 0x10u",
    "+ 0x14u", "+ 0x18u", "+ 0x1Cu", "+ 0x20u", "+ 0x24u",
):
    if offset not in libstdcpp_state:
        fail(f"1.02j libstdc++ deque layout lost offset: {offset}")

layout = function_slice(probe, "TournamentQueueLayout ResolveTournamentQueueLayout(")
for tag, offset in (
    ("1.02e", "740u"),
    ("1.02f", "740u"),
    ("1.02f-framestepping", "740u"),
    ("1.02g", "740u"),
    ("1.02h", "740u"),
    ("1.02i", "748u"),
    ("1.02j", "0x340u"),
):
    if f'"{tag}"' not in layout or offset not in layout:
        fail(f"queue layout routing missing {tag} at {offset}")

live_read = function_slice(probe, "ReadTournamentAutoNavSuffixState(")
for required in (
    "profile.addrGameModeCurrentIndex",
    "modeBefore",
    "modeAfter",
    "modeBefore != modeAfter",
    "ReadLibstdcppTournamentQueue",
    "ReadMsvcTournamentQueue",
    "ClassifyTournamentAutoNavSuffix",
):
    if required not in live_read:
        fail(f"live queue witness lost constraint: {required}")

readiness = function_slice(probe, "bool ReadSessionReadiness(")
for required in (
    "ReadTournamentAutoNavSuffixState(profile, sessionPtr)",
    "TournamentAutoNavSuffixState::Invalid",
    "TournamentAutoNavSuffixState::Ready",
):
    if required not in readiness:
        fail(f"Tournament readiness lost live queue validation: {required}")

snapshot = probe[probe.find("struct LauncherSessionSnapshot"):]
snapshot = snapshot[:snapshot.find("};")]
for forbidden in ("queueCount", "queueHash", "autoNavCount", "autoNavHash"):
    if forbidden in snapshot:
        fail(f"dynamic queue evidence became an immutable snapshot field: {forbidden}")

revalidate = function_slice(
    probe, "static bool RevalidateRevivalLauncherStartupImpl(")
if revalidate.count("ReadLauncherSessionSnapshot(probe, revival") != 2:
    fail("parked revalidation no longer takes two fresh live session samples")

print("Tournament queue admission contract: PASS")
