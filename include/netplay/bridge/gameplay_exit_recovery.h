#pragma once

#include <cstdint>

namespace netplay::bridge::recovery
{
enum class GameplayExitOrigin
{
    ConsoleErrorDisconnect,
    SpectatorEsc,
    TickExitProcess,
    FrameExitProcess,
    HelperDeathWatchdog,
    QuitRing,
    BypassStallConsoleError,
    BypassStallSyncFrameStalled,
    BypassStallWallTimeout,
};

struct PendingGameplayExitMenuEntry
{
    int mode = -1;
    char origin[64] = {};
};

bool BeginGameplayExitRecovery(GameplayExitOrigin origin);
bool BeginGameplayExitRecovery(const char* origin);

bool IsGameplayExitRecoveryInProgress();
bool HasPendingGameplayExitMenuEntry();
bool WasGameplayExitRecoveryCompleted();
void ResetGameplayExitRecoveryCompletion();
const char* CurrentGameplayExitRecoveryOrigin();
const char* CurrentGameplayExitRecoveryStateName();
bool HasGameplayExitMenuEntryStarted();
bool HasGameplayExitMenuEntryBeenConsumed();
bool ShouldSuppressLegacyGameplayExitCleanup();
void NoteGameplayExitMenuEntryStarted(uint32_t screenContext, const char* source);
void ObserveGameplayExitRecoveryProgress(uint8_t screen, int phase, int role);
bool ConsumePendingGameplayExitMenuEntry(
    uint32_t screenContext,
    PendingGameplayExitMenuEntry* outEntry);
bool CompleteFrontendReturnMenuEntry(
    uint32_t screenContext,
    PendingGameplayExitMenuEntry* outEntry);
}
