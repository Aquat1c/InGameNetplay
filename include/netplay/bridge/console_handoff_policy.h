#pragma once

#include "netplay/bridge/session_bridge.h"

#include <windows.h>

namespace netplay::bridge::console_handoff
{
struct InitialConsoleInputPlan
{
    int menuChoice = 0;
    const char* primaryInput = "";
    const char* auxiliaryInput = "";
};

inline InitialConsoleInputPlan BuildInitialConsoleInputPlan(
    NetbridgeRole role)
{
    switch (role)
    {
    case NetbridgeRole::Host:
        return {1, "1\r\n", ""};
    case NetbridgeRole::Join:
    case NetbridgeRole::JoinSpectate:
        return {3, "3\r\n", ""};
    case NetbridgeRole::Spectate:
        return {4, "4\r\n", ""};
    }
    return {};
}

inline bool IsHostReader(
    bool isHostSession,
    LONG consoleSerial,
    LONG consoleServedSerial)
{
    return isHostSession
        && consoleSerial > 0
        && consoleServedSerial >= consoleSerial;
}

inline bool IsExactBelControlWake(
    const INPUT_RECORD& record,
    DWORD recordCount)
{
    return recordCount == 1
        && record.EventType == KEY_EVENT
        && record.Event.KeyEvent.bKeyDown == TRUE
        && record.Event.KeyEvent.wRepeatCount == 0
        && record.Event.KeyEvent.wVirtualKeyCode == VK_RETURN
        && record.Event.KeyEvent.wVirtualScanCode == 0
        && record.Event.KeyEvent.uChar.AsciiChar == '\a'
        && record.Event.KeyEvent.dwControlKeyState == 0;
}

inline bool HasPendingExplicitDelayInput(
    LONG promptSerial,
    LONG promptServedSerial,
    LONG inputSerial,
    LONG inputServedSerial)
{
    return promptSerial > 0
        && promptServedSerial < promptSerial
        && inputSerial > inputServedSerial;
}

inline bool IsExplicitDelayInputServed(
    LONG inputSerial,
    LONG inputServedSerial)
{
    return inputSerial > 0 && inputServedSerial >= inputSerial;
}

inline bool IsHeldHostDelayReader(
    bool isHostSession,
    LONG promptSerial,
    LONG promptServedSerial,
    LONG inputSerial,
    LONG inputServedSerial)
{
    return isHostSession
        && promptSerial > 0
        && promptServedSerial < promptSerial
        && !IsExplicitDelayInputServed(inputSerial, inputServedSerial);
}

inline LONG SelectPromptTransitionWakeSerial(
    LONG rawWriteRequestSerialSnapshot,
    LONG currentRequestSerial)
{
    if (rawWriteRequestSerialSnapshot >= 0)
    {
        return rawWriteRequestSerialSnapshot + 1;
    }
    return currentRequestSerial > 0 ? currentRequestSerial : 1;
}

inline LONG SelectTimeoutRequiredWakeSerial(
    LONG rawWriteRequestSerialSnapshot,
    LONG promptTransitionWakeSerial,
    LONG currentRequestSerial)
{
    LONG requiredWakeSerial = 0;
    if (rawWriteRequestSerialSnapshot >= 0)
    {
        requiredWakeSerial = rawWriteRequestSerialSnapshot + 1;
    }
    else if (currentRequestSerial > promptTransitionWakeSerial)
    {
        requiredWakeSerial = currentRequestSerial;
    }
    else
    {
        requiredWakeSerial =
            (promptTransitionWakeSerial > 0
                ? promptTransitionWakeSerial
                : currentRequestSerial) + 1;
    }
    if (requiredWakeSerial <= promptTransitionWakeSerial)
    {
        requiredWakeSerial = promptTransitionWakeSerial + 1;
    }
    return requiredWakeSerial;
}

inline bool IsNativeTimeoutCleanupReady(
    bool helperAlive,
    LONG controlWakeServedSerial,
    LONG requiredControlWakeSerial)
{
    return !helperAlive
        || (requiredControlWakeSerial > 0
            && controlWakeServedSerial >= requiredControlWakeSerial);
}
} // namespace netplay::bridge::console_handoff
