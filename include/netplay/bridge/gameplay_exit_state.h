#pragma once
#include <cstdint>

namespace netplay::bridge::recovery
{
enum class RecoveryState : long { Idle, Running, MenuPending, MenuClaimed, Completed };
constexpr bool RecoveryInProgress(RecoveryState state)
{
    return state == RecoveryState::Running || state == RecoveryState::MenuPending
        || state == RecoveryState::MenuClaimed;
}
constexpr bool CanBeginRecovery(RecoveryState state) { return !RecoveryInProgress(state); }

enum class MenuEntryProgress : long { None = 0, Started = 1, Consumed = 2, StartedAndConsumed = 3 };
constexpr MenuEntryProgress AdvanceMenuEntry(MenuEntryProgress current, MenuEntryProgress event)
{
    return static_cast<MenuEntryProgress>(static_cast<long>(current) | static_cast<long>(event));
}
constexpr bool EntryStarted(MenuEntryProgress state)
{
    return state == MenuEntryProgress::Started || state == MenuEntryProgress::StartedAndConsumed;
}
constexpr bool EntryConsumed(MenuEntryProgress state)
{
    return state == MenuEntryProgress::Consumed || state == MenuEntryProgress::StartedAndConsumed;
}
}
