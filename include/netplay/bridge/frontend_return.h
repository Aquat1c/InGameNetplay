#pragma once

#include <cstdint>

namespace netplay::bridge::frontend_return
{
enum class ScreenId : uint8_t
{
    Title = 0,
    CharacterSelect = 1,
    Loading = 2,
    Battle = 3,
    Unused4 = 4,
    Result = 5,
    Settings = 6,
    StaffRoll = 7,
    ReplaySelect = 8,
    Unknown = 255,
};

enum class ReturnTarget
{
    Title,
    NetplayMenu,
    CharacterSelect,
};

enum class ReturnOwner
{
    DisconnectRecovery,
    AsyncHostAccept,
    UserMenuExit,
    Diagnostic,
};

enum class ReturnState
{
    Idle,
    Requested,
    WaitForSafePoint,
    LoadingWait,
    LoadingCriticalUnblock,
    WaitingForLoadingToResolve,
    NativeExitRequested,
    WaitingForNativeExit,
    ForceTitleFallback,
    TitleReached,
    NetplayMenuContinuationPending,
    Complete,
    Failed,
};

struct FrontendContext
{
    ScreenId screen = ScreenId::Unknown;
    uint8_t rawScreen = 255;
    uint8_t gameModeRaw = 255;
    uintptr_t screenContext = 0;
    uintptr_t titleContext = 0;
    uintptr_t charselectContext = 0;
    uintptr_t loadingContext = 0;
    uintptr_t battleContext = 0;
    uintptr_t resultContext = 0;
    uint8_t lifecycle44 = 255;
    uint8_t exit45 = 255;
    bool netplaySessionActive = false;
    bool recoveryInProgress = false;
    bool insideFrameTick = false;
};

struct ReturnRequest
{
    ReturnTarget target = ReturnTarget::Title;
    ReturnOwner owner = ReturnOwner::Diagnostic;
    const char* reason = nullptr;
    bool preserveHelper = true;
    bool allowNativeBattleExit = false;
    bool allowNativeResultExit = false;
    bool allowForcedFallback = false;
    bool enterNetplayMenuAfterTitle = false;
    bool confirmedDisconnect = false;
};

struct ReturnResult
{
    bool accepted = false;
    bool completed = false;
    bool pending = false;
    ReturnState state = ReturnState::Idle;
    ScreenId startScreen = ScreenId::Unknown;
    ScreenId currentScreen = ScreenId::Unknown;
};

FrontendContext CaptureFrontendContext();
ReturnResult BeginReturnToFrontend(const ReturnRequest& request);
void TickFrontendReturn();
bool HasPendingReturn();
bool IsReturningToFrontend();
bool ConsumeTitleContinuation(ReturnOwner* ownerOut, ReturnTarget* targetOut);
const char* CurrentStateName();
bool HasNetplayMenuContinuationPending();
bool HasConsumedNetplayMenuContinuation();
}
