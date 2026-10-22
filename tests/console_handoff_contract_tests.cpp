#include "netplay/bridge/console_handoff_policy.h"

#include <cstring>
#include <iostream>

namespace
{
int g_failures = 0;

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

INPUT_RECORD MakeNativeBelWake()
{
    INPUT_RECORD record = {};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wRepeatCount = 0;
    record.Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
    record.Event.KeyEvent.wVirtualScanCode = 0;
    record.Event.KeyEvent.uChar.AsciiChar = '\a';
    record.Event.KeyEvent.dwControlKeyState = 0;
    return record;
}

void TestInitialInputPlan()
{
    using namespace netplay::bridge;
    using console_handoff::BuildInitialConsoleInputPlan;

    const auto host = BuildInitialConsoleInputPlan(NetbridgeRole::Host);
    Expect(host.menuChoice == 1, "Host selects Revival menu choice 1");
    Expect(
        std::strcmp(host.primaryInput, "1\r\n") == 0,
        "Host primary input is exactly 1 CRLF");
    Expect(
        host.auxiliaryInput[0] == '\0',
        "Host has no auxiliary blank input");

    const auto join = BuildInitialConsoleInputPlan(NetbridgeRole::Join);
    Expect(join.menuChoice == 3, "Join selects clipboard menu choice 3");
    Expect(
        std::strcmp(join.primaryInput, "3\r\n") == 0,
        "Join primary input remains 3 CRLF");
    Expect(join.auxiliaryInput[0] == '\0', "Join has no auxiliary input");

    const auto joinSpectate =
        BuildInitialConsoleInputPlan(NetbridgeRole::JoinSpectate);
    Expect(
        joinSpectate.menuChoice == 3,
        "JoinSpectate keeps clipboard menu choice 3");
    Expect(
        std::strcmp(joinSpectate.primaryInput, "3\r\n") == 0,
        "JoinSpectate primary input remains 3 CRLF");

    const auto spectate =
        BuildInitialConsoleInputPlan(NetbridgeRole::Spectate);
    Expect(spectate.menuChoice == 4, "Spectate keeps menu choice 4");
    Expect(
        std::strcmp(spectate.primaryInput, "4\r\n") == 0,
        "Spectate primary input remains 4 CRLF");

    const auto invalid =
        BuildInitialConsoleInputPlan(static_cast<NetbridgeRole>(999));
    Expect(
        invalid.menuChoice == 0 && invalid.primaryInput[0] == '\0',
        "invalid roles do not silently become Host");
}

void TestExactBelWakePredicate()
{
    using netplay::bridge::console_handoff::IsExactBelControlWake;

    INPUT_RECORD record = MakeNativeBelWake();
    Expect(
        IsExactBelControlWake(record, 1),
        "exact native VK_RETURN/BEL record is accepted");

    Expect(
        !IsExactBelControlWake(record, 2),
        "multi-record writes are not control wakes");

    record.Event.KeyEvent.bKeyDown = FALSE;
    Expect(
        !IsExactBelControlWake(record, 1),
        "key-up record is rejected");
    record = MakeNativeBelWake();
    record.Event.KeyEvent.bKeyDown = 2;
    Expect(
        !IsExactBelControlWake(record, 1),
        "non-native nonzero key-down value is rejected");
    record = MakeNativeBelWake();
    record.Event.KeyEvent.wRepeatCount = 1;
    Expect(
        !IsExactBelControlWake(record, 1),
        "non-native repeat count is rejected");
    record = MakeNativeBelWake();
    record.Event.KeyEvent.wVirtualKeyCode = 0;
    Expect(
        !IsExactBelControlWake(record, 1),
        "non-Return virtual key is rejected");
    record = MakeNativeBelWake();
    record.Event.KeyEvent.wVirtualScanCode = 1;
    Expect(
        !IsExactBelControlWake(record, 1),
        "nonzero scan code is rejected");
    record = MakeNativeBelWake();
    record.Event.KeyEvent.uChar.AsciiChar = '\r';
    Expect(
        !IsExactBelControlWake(record, 1),
        "CR is not treated as the native BEL wake");
    record = MakeNativeBelWake();
    record.Event.KeyEvent.dwControlKeyState = SHIFT_PRESSED;
    Expect(
        !IsExactBelControlWake(record, 1),
        "modified key record is rejected");
}

void TestReaderAndDelayContracts()
{
    using namespace netplay::bridge::console_handoff;

    Expect(
        !IsHostReader(true, 1, 0),
        "Host reader is not parked before primary input is served");
    Expect(
        IsHostReader(true, 1, 1),
        "Host reader parks after primary input is served");
    Expect(
        !IsHostReader(false, 1, 1),
        "non-Host reader does not inherit Host parking");

    Expect(
        HasPendingExplicitDelayInput(4, 3, 7, 6),
        "new explicit delay serial can be served");
    Expect(
        !HasPendingExplicitDelayInput(4, 4, 7, 6),
        "resolved prompt cannot consume another delay input");
    Expect(
        !HasPendingExplicitDelayInput(4, 3, 7, 7),
        "already-served delay serial is not replayed");

    Expect(
        IsExplicitDelayInputServed(7, 7),
        "matching explicit input serial satisfies readiness");
    Expect(
        !IsExplicitDelayInputServed(0, 4),
        "prompt/control service without input never satisfies readiness");
    Expect(
        !IsExplicitDelayInputServed(7, 6),
        "queued but unserved input does not satisfy readiness");

    Expect(
        IsHeldHostDelayReader(true, 4, 3, 0, 0),
        "unanswered Host delay reader is held");
    Expect(
        !IsHeldHostDelayReader(false, 4, 3, 0, 0),
        "Join delay reader is not classified as held Host");
    Expect(
        !IsHeldHostDelayReader(true, 4, 4, 0, 0),
        "served prompt is no longer held");
    Expect(
        !IsHeldHostDelayReader(true, 4, 3, 7, 7),
        "served explicit input is no longer held");
}

void TestTimeoutCleanupContract()
{
    using namespace netplay::bridge::console_handoff;

    Expect(
        SelectPromptTransitionWakeSerial(0, 0) == 1,
        "prompt raw-write snapshot binds the immediately following BEL");
    Expect(
        SelectPromptTransitionWakeSerial(3, 7) == 4,
        "deferred prompt parsing keeps its raw-write generation");
    Expect(
        SelectTimeoutRequiredWakeSerial(1, 1, 2) == 2,
        "timeout raw-write snapshot binds the timeout-local BEL");
    Expect(
        SelectTimeoutRequiredWakeSerial(1, 1, 1) == 2,
        "synchronous timeout parse waits for the next BEL");
    Expect(
        SelectTimeoutRequiredWakeSerial(-1, 1, 2) == 2,
        "late fallback parse accepts the post-transition request");
    Expect(
        SelectTimeoutRequiredWakeSerial(0, 1, 1) == 2,
        "stale raw snapshot cannot select the prompt-transition BEL");

    Expect(
        !IsNativeTimeoutCleanupReady(true, 1, 2),
        "prompt-transition BEL cannot release timeout recovery");
    Expect(
        IsNativeTimeoutCleanupReady(true, 2, 2),
        "served timeout-local BEL releases timeout recovery");
    Expect(
        !IsNativeTimeoutCleanupReady(true, 2, 0),
        "missing causal wake serial cannot release a live helper");
    Expect(
        IsNativeTimeoutCleanupReady(false, 1, 2),
        "natural helper exit releases timeout recovery");
}
} // namespace

int main()
{
    TestInitialInputPlan();
    TestExactBelWakePredicate();
    TestReaderAndDelayContracts();
    TestTimeoutCleanupContract();

    if (g_failures != 0)
    {
        std::cerr << g_failures << " console handoff contract(s) failed\n";
        return 1;
    }

    std::cout << "console handoff contracts passed\n";
    return 0;
}
