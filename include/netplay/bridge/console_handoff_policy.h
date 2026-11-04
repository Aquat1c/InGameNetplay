#pragma once

#include "netplay/bridge/session_bridge.h"

#include <windows.h>

namespace netplay::bridge::console_handoff
{
enum class PromptState { Unseen, Pending, Served };
inline PromptState ReadPromptGeneration(LONG request, LONG served)
{
    return request <= 0 ? PromptState::Unseen
        : request == served ? PromptState::Served : PromptState::Pending;
}
inline PromptState ReadOrderedPromptProgress(LONG request, LONG served)
{
    return request <= 0 ? PromptState::Unseen
        : served >= request ? PromptState::Served : PromptState::Pending;
}
enum class InputDeliveryState { Absent, Queued, Served };
inline InputDeliveryState ReadInputDelivery(LONG request, LONG served)
{
    return request > served ? InputDeliveryState::Queued
        : request > 0 ? InputDeliveryState::Served : InputDeliveryState::Absent;
}
enum class DelayHandoffState { NoPrompt, PromptServed, WaitingInput, InputQueued, InputServed };
inline DelayHandoffState ReadDelayHandoff(LONG prompt, LONG promptServed, LONG input, LONG inputServed)
{
    const auto progress = ReadOrderedPromptProgress(prompt, promptServed);
    if (progress == PromptState::Unseen) return DelayHandoffState::NoPrompt;
    if (progress == PromptState::Served) return DelayHandoffState::PromptServed;
    const auto delivery = ReadInputDelivery(input, inputServed);
    return delivery == InputDeliveryState::Queued ? DelayHandoffState::InputQueued
        : delivery == InputDeliveryState::Served ? DelayHandoffState::InputServed : DelayHandoffState::WaitingInput;
}
enum class TimeoutCleanupState { WaitingForWake, Ready };
inline TimeoutCleanupState ReadTimeoutCleanup(bool helperAlive, LONG served, LONG required)
{
    return !helperAlive || (required > 0 && served >= required)
        ? TimeoutCleanupState::Ready : TimeoutCleanupState::WaitingForWake;
}

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

inline bool IsControlWakeSession(
    bool isHostSession,
    LONG delayPromptSerial,
    bool onlineMenuServed)
{
    return isHostSession || delayPromptSerial > 0 || onlineMenuServed;
}

inline void CompleteControlWakeFlush(
    volatile LONG* servedSerial,
    LONG requestSerialSnapshot,
    bool nativeFlushSucceeded)
{
    if (!nativeFlushSucceeded) return;
    LONG served = InterlockedCompareExchange(servedSerial, 0, 0);
    while (served < requestSerialSnapshot)
    {
        const LONG previous = InterlockedCompareExchange(
            servedSerial, requestSerialSnapshot, served);
        if (previous == served) return;
        served = previous;
    }
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
    return ReadDelayHandoff(promptSerial, promptServedSerial, inputSerial, inputServedSerial)
        == DelayHandoffState::InputQueued;
}

inline bool IsExplicitDelayInputServed(
    LONG inputSerial,
    LONG inputServedSerial)
{
    return ReadInputDelivery(inputSerial, inputServedSerial) == InputDeliveryState::Served;
}

inline bool IsHeldHostDelayReader(
    bool isHostSession,
    LONG promptSerial,
    LONG promptServedSerial,
    LONG inputSerial,
    LONG inputServedSerial)
{
    const auto state = ReadDelayHandoff(promptSerial, promptServedSerial, inputSerial, inputServedSerial);
    return isHostSession && (state == DelayHandoffState::WaitingInput || state == DelayHandoffState::InputQueued);
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
    return ReadTimeoutCleanup(helperAlive, controlWakeServedSerial, requiredControlWakeSerial)
        == TimeoutCleanupState::Ready;
}
}
