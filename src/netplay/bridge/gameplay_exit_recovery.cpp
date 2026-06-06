#include "netplay/bridge/gameplay_exit_recovery.h"

#include "netplay/bridge/frontend_return.h"
#include "netplay/bridge/takeover_internal.h"

#include "crash_handler.h"
#include "logger.h"

#include <windows.h>

namespace netplay::bridge::recovery
{
namespace
{
volatile LONG g_recoveryInProgress = 0;
volatile LONG g_menuEntryPending = 0;
volatile LONG g_menuEntryStarted = 0;
volatile LONG g_menuEntryConsumed = 0;
volatile LONG g_recoveryCompleted = 0;
volatile LONG g_recoveryState = 0;
int g_menuEntryMode = -1;
char g_menuEntryOrigin[64] = {};
char g_activeOrigin[64] = {};
uint32_t g_menuEntryScheduledFrameTick = 0;
uint32_t g_menuEntryObservationCount = 0;
bool g_menuEntryStallLogged = false;
constexpr uint32_t kTitleContinuationWatchdogFrames = 120;

enum RecoveryStateValue
{
    RecoveryStateIdle = 0,
    RecoveryStateRunning = 1,
    RecoveryStateMenuPending = 2,
    RecoveryStateCompleted = 3,
};

const char* RecoveryStateToString(LONG state)
{
    switch (state)
    {
    case RecoveryStateIdle:
        return "idle";
    case RecoveryStateRunning:
        return "running";
    case RecoveryStateMenuPending:
        return "menu_pending";
    case RecoveryStateCompleted:
        return "completed";
    }
    return "unknown";
}

void SetRecoveryState(LONG newState, const char* origin)
{
    const LONG oldState = InterlockedExchange(&g_recoveryState, newState);
    if (oldState == newState)
    {
        return;
    }

    mod::Log(
        "RECOVERY_STATE_CHANGE old=%s new=%s origin=%s",
        RecoveryStateToString(oldState),
        RecoveryStateToString(newState),
        (origin != nullptr && origin[0] != '\0') ? origin : "unknown");
}

const char* OriginToString(GameplayExitOrigin origin)
{
    switch (origin)
    {
    case GameplayExitOrigin::ConsoleErrorDisconnect:
        return "console_error_disconnect";
    case GameplayExitOrigin::SpectatorEsc:
        return "spectator_esc";
    case GameplayExitOrigin::TickExitProcess:
        return "tick_exitprocess";
    case GameplayExitOrigin::FrameExitProcess:
        return "frame_exitprocess";
    case GameplayExitOrigin::HelperDeathWatchdog:
        return "helper_death_watchdog";
    case GameplayExitOrigin::QuitRing:
        return "quit_ring";
    case GameplayExitOrigin::BypassStallConsoleError:
        return "bypass_stall_console_error";
    case GameplayExitOrigin::BypassStallSyncFrameStalled:
        return "bypass_stall_syncframe_stalled";
    case GameplayExitOrigin::BypassStallWallTimeout:
        return "bypass_stall_wall_timeout";
    }
    return "unknown";
}

void CleanupRevivalTextForGameplayExit(
    bool* outRestoreOk,
    bool* outClearOk,
    bool* outDisableOk)
{
    bool restoreOk = takeover::RestoreRenderContextForGameplayExitCleanup();
    bool clearOk = false;
    bool disableOk = false;

    if (restoreOk)
    {
        clearOk = takeover::ClearRevivalTextWithCurrentRenderContext();
        disableOk = takeover::DisableRevivalTextRenderingWithCurrentRenderContext();
        takeover::MarkRenderContextConsumedForGameplayExitCleanup();
    }

    if (outRestoreOk != nullptr)
    {
        *outRestoreOk = restoreOk;
    }
    if (outClearOk != nullptr)
    {
        *outClearOk = clearOk;
    }
    if (outDisableOk != nullptr)
    {
        *outDisableOk = disableOk;
    }
}

bool RunGameplayExitContinuation(const char* origin)
{
    const char* originTag = (origin != nullptr && origin[0] != '\0')
        ? origin
        : "unknown";
    const int recoveredRole = takeover::g_localRoleFlag;
    const DWORD recoveredPid = takeover::g_revivalProcessId;

    takeover::LogSessionDiagnosticState("GameplayExitRecovery_entry");

    takeover::NeutralizeRevivalSessionVtable();
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_STEP neutralize_vtable result=1 origin=%s role=%d",
        originTag,
        recoveredRole);

    bool restoreOk = false;
    bool clearOk = false;
    bool disableOk = false;
    CleanupRevivalTextForGameplayExit(&restoreOk, &clearOk, &disableOk);
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_STEP restore_render_context result=%d origin=%s",
        restoreOk ? 1 : 0,
        originTag);
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_STEP clear_text result=%d origin=%s",
        clearOk ? 1 : 0,
        originTag);
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_STEP disable_text result=%d origin=%s",
        disableOk ? 1 : 0,
        originTag);

    const int countBefore = takeover::GetForceLocalPlayInitCount();
    const bool initOk = takeover::ForceLocalPlayInit();
    const int countAfter = takeover::GetForceLocalPlayInitCount();
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_STEP force_local_init countBefore=%d countAfter=%d result=%d origin=%s",
        countBefore,
        countAfter,
        initOk ? 1 : 0,
        originTag);
    if (countAfter != countBefore + 1)
    {
        mod::Log(
            "GAMEPLAY_EXIT_RECOVERY_STEP force_local_init violation=1 countBefore=%d countAfter=%d origin=%s",
            countBefore,
            countAfter,
            originTag);
    }

    BOOL termOk = TRUE;
    DWORD termErr = 0;
    BOOL closeOk = TRUE;
    const bool hadHelper = (takeover::g_revivalProcess != nullptr);
    if (takeover::g_revivalProcess != nullptr)
    {
        termOk = TerminateProcess(takeover::g_revivalProcess, 0);
        termErr = termOk ? 0 : GetLastError();
        closeOk = CloseHandle(takeover::g_revivalProcess);
        takeover::g_revivalProcess = nullptr;
        takeover::g_revivalProcessId = 0;
    }
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_STEP terminate_helper result=%d hadHandle=%d pid=%lu termOk=%d closeOk=%d err=%lu origin=%s",
        (!hadHelper || (termOk && closeOk)) ? 1 : 0,
        hadHelper ? 1 : 0,
        static_cast<unsigned long>(recoveredPid),
        termOk ? 1 : 0,
        closeOk ? 1 : 0,
        static_cast<unsigned long>(termErr),
        originTag);

    const bool patchOk = takeover::RestoreDllExitProcessPatches();
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_STEP restore_patches result=%d origin=%s",
        patchOk ? 1 : 0,
        originTag);

    mod::ResetCrashRecoveryState();
    const bool deferredWasSet =
        takeover::ClearDeferredCancelCleanupForRecovery("gameplay_exit_reset_state");
    takeover::ResetGameModeValidation();
    if (takeover::g_hostBlock != nullptr)
    {
        InterlockedExchange(&takeover::g_hostBlock->consoleErrorSerial, 0);
        takeover::g_hostBlock->consoleErrorText[0] = '\0';
    }
    InterlockedExchange(&takeover::g_revivalExitIntercepted, 0);
    InterlockedExchange(&takeover::g_revivalExitMode, -1);
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_STEP reset_state result=1 deferredCancelWasSet=%d origin=%s",
        deferredWasSet ? 1 : 0,
        originTag);

    takeover::g_localInitAppliedForSession = false;

    const bool graphicsPatchOk =
        takeover::EnsureRevivalGraphicsPatchSetEnabled(
            "gameplay_exit_before_frontend_return");
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_STEP restore_graphics_patch_state result=%d origin=%s",
        graphicsPatchOk ? 1 : 0,
        originTag);

    g_menuEntryMode = recoveredRole;
    takeover::CopyString(
        g_menuEntryOrigin,
        sizeof(g_menuEntryOrigin),
        originTag);
    g_menuEntryScheduledFrameTick = takeover::GetGameplayExitRecoveryFrameTick();
    g_menuEntryObservationCount = 0;
    g_menuEntryStallLogged = false;

    frontend_return::ReturnRequest request = {};
    request.target = frontend_return::ReturnTarget::NetplayMenu;
    request.owner = frontend_return::ReturnOwner::DisconnectRecovery;
    request.reason = originTag;
    request.preserveHelper = false;
    request.allowNativeBattleExit = true;
    request.allowNativeResultExit = true;
    request.allowForcedFallback = true;
    request.enterNetplayMenuAfterTitle = true;
    request.confirmedDisconnect = true;

    const frontend_return::ReturnResult returnResult =
        frontend_return::BeginReturnToFrontend(request);
    const bool scheduled = returnResult.accepted
        && (returnResult.pending || returnResult.completed);

    if (scheduled)
    {
        InterlockedExchange(&g_menuEntryPending, 1);
        mod::Log(
            "RECOVERY_MENU_PENDING_SET origin=%s mode=%d frameTick=%u",
            originTag,
            recoveredRole,
            g_menuEntryScheduledFrameTick);
        SetRecoveryState(RecoveryStateMenuPending, originTag);
        mod::Log(
            "GAMEPLAY_EXIT_RECOVERY_MENU_ENTRY scheduled=1 origin=%s mode=%d",
            originTag,
            recoveredRole);
        (void)takeover::SuppressDeferredCancelCleanupAfterGameplayRecovery(originTag);
    }
    else
    {
        InterlockedExchange(&g_recoveryInProgress, 0);
        InterlockedExchange(&g_menuEntryPending, 0);
        SetRecoveryState(RecoveryStateIdle, originTag);
    }

    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_MENU_ENTRY direct=0 scheduled=%d origin=%s mode=%d",
        scheduled ? 1 : 0,
        originTag,
        recoveredRole);
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_COMPLETE origin=%s result=%d mode=%d",
        originTag,
        scheduled ? 1 : 0,
        recoveredRole);
    takeover::LogSessionDiagnosticState("GameplayExitRecovery_exit");
    return scheduled;
}
}

bool BeginGameplayExitRecovery(GameplayExitOrigin origin)
{
    return BeginGameplayExitRecovery(OriginToString(origin));
}

bool BeginGameplayExitRecovery(const char* origin)
{
    const char* originTag = (origin != nullptr && origin[0] != '\0')
        ? origin
        : "unknown";

    if (InterlockedCompareExchange(&g_recoveryInProgress, 1, 0) != 0)
    {
        mod::Log(
            "GAMEPLAY_EXIT_RECOVERY_ALREADY_IN_PROGRESS origin=%s existingOrigin=%s pendingMenu=%ld completed=%ld",
            originTag,
            g_activeOrigin,
            static_cast<long>(InterlockedCompareExchange(&g_menuEntryPending, 0, 0)),
            static_cast<long>(InterlockedCompareExchange(&g_recoveryCompleted, 0, 0)));
        mod::Log(
            "GAMEPLAY_EXIT_RECOVERY_BEGIN origin=%s skipped=1 reason=in_progress frameTick=%u role=%d pendingMenu=%ld",
            originTag,
            takeover::GetGameplayExitRecoveryFrameTick(),
            takeover::g_localRoleFlag,
            static_cast<long>(InterlockedCompareExchange(&g_menuEntryPending, 0, 0)));
        return false;
    }

    InterlockedExchange(&g_recoveryCompleted, 0);
    InterlockedExchange(&g_menuEntryStarted, 0);
    InterlockedExchange(&g_menuEntryConsumed, 0);
    takeover::CopyString(g_activeOrigin, sizeof(g_activeOrigin), originTag);
    SetRecoveryState(RecoveryStateRunning, originTag);

    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_BEGIN origin=%s frameTick=%u role=%d pid=%lu insideFrameTick=%d",
        originTag,
        takeover::GetGameplayExitRecoveryFrameTick(),
        takeover::g_localRoleFlag,
        static_cast<unsigned long>(takeover::g_revivalProcessId),
        takeover::IsGameplayExitRecoveryInsideFrameTick() ? 1 : 0);

    (void)takeover::ClearDeferredCancelCleanupForRecovery("begin_gameplay_exit_recovery");
    const LONG oldExitIntercepted =
        InterlockedExchange(&takeover::g_revivalExitIntercepted, 0);
    const LONG oldExitMode =
        InterlockedExchange(&takeover::g_revivalExitMode, -1);
    if (oldExitIntercepted != 0 || oldExitMode != -1)
    {
        mod::Log(
            "GAMEPLAY_EXIT_SUPPRESS_OLD_EXIT_INTERCEPTION reason=begin_gameplay_exit_recovery exitIntercepted=%ld exitMode=%ld",
            static_cast<long>(oldExitIntercepted),
            static_cast<long>(oldExitMode));
    }

    if (takeover::IsGameplayExitRecoveryInsideFrameTick())
    {
        mod::Log(
            "GAMEPLAY_EXIT_RECOVERY_COMPLETE origin=%s result=0 reason=inside_original_tick",
            originTag);
        InterlockedExchange(&g_recoveryInProgress, 0);
        SetRecoveryState(RecoveryStateIdle, originTag);
        return false;
    }

    return RunGameplayExitContinuation(originTag);
}

bool HasPendingGameplayExitMenuEntry()
{
    return InterlockedCompareExchange(&g_menuEntryPending, 0, 0) != 0;
}

bool IsGameplayExitRecoveryInProgress()
{
    return InterlockedCompareExchange(&g_recoveryInProgress, 0, 0) != 0;
}

bool WasGameplayExitRecoveryCompleted()
{
    return InterlockedCompareExchange(&g_recoveryCompleted, 0, 0) != 0;
}

void ResetGameplayExitRecoveryCompletion()
{
    InterlockedExchange(&g_recoveryCompleted, 0);
    InterlockedExchange(&g_menuEntryStarted, 0);
    InterlockedExchange(&g_menuEntryConsumed, 0);
    if (!IsGameplayExitRecoveryInProgress() && !HasPendingGameplayExitMenuEntry())
    {
        SetRecoveryState(RecoveryStateIdle, g_activeOrigin);
        g_activeOrigin[0] = '\0';
    }
}

const char* CurrentGameplayExitRecoveryOrigin()
{
    return g_activeOrigin;
}

const char* CurrentGameplayExitRecoveryStateName()
{
    return RecoveryStateToString(InterlockedCompareExchange(&g_recoveryState, 0, 0));
}

bool HasGameplayExitMenuEntryStarted()
{
    return InterlockedCompareExchange(&g_menuEntryStarted, 0, 0) != 0;
}

bool HasGameplayExitMenuEntryBeenConsumed()
{
    return InterlockedCompareExchange(&g_menuEntryConsumed, 0, 0) != 0;
}

bool ShouldSuppressLegacyGameplayExitCleanup()
{
    return IsGameplayExitRecoveryInProgress()
        || HasPendingGameplayExitMenuEntry()
        || HasGameplayExitMenuEntryStarted()
        || HasGameplayExitMenuEntryBeenConsumed()
        || WasGameplayExitRecoveryCompleted()
        || frontend_return::HasPendingReturn()
        || frontend_return::HasNetplayMenuContinuationPending()
        || frontend_return::HasConsumedNetplayMenuContinuation();
}

void NoteGameplayExitMenuEntryStarted(uint32_t screenContext, const char* source)
{
    if (InterlockedExchange(&g_menuEntryStarted, 1) == 0)
    {
        mod::Log(
            "GAMEPLAY_EXIT_MENU_ENTRY_STARTED screenContext=0x%08X origin=%s source=%s state=%s frontendReturnState=%s",
            screenContext,
            CurrentGameplayExitRecoveryOrigin(),
            source != nullptr ? source : "unknown",
            CurrentGameplayExitRecoveryStateName(),
            frontend_return::CurrentStateName());
    }
}

void ObserveGameplayExitRecoveryProgress(uint8_t screen, int phase, int role)
{
    if (!HasPendingGameplayExitMenuEntry() || g_menuEntryStallLogged)
    {
        return;
    }

    const uint32_t frameTick = takeover::GetGameplayExitRecoveryFrameTick();
    const uint32_t elapsedFrames = frameTick - g_menuEntryScheduledFrameTick;
    ++g_menuEntryObservationCount;
    if (elapsedFrames < kTitleContinuationWatchdogFrames
        && g_menuEntryObservationCount < kTitleContinuationWatchdogFrames)
    {
        return;
    }

    g_menuEntryStallLogged = true;
    mod::Log(
        "GAMEPLAY_EXIT_RECOVERY_STALLED waiting_for_title_continuation screen=%u mode=%d phase=%d role=%d origin=%s elapsedFrames=%u observations=%u scheduledFrame=%u currentFrame=%u",
        static_cast<unsigned>(screen),
        g_menuEntryMode,
        phase,
        role,
        g_menuEntryOrigin,
        elapsedFrames,
        g_menuEntryObservationCount,
        g_menuEntryScheduledFrameTick,
        frameTick);
}

bool ConsumePendingGameplayExitMenuEntry(
    uint32_t screenContext,
    PendingGameplayExitMenuEntry* outEntry)
{
    if (InterlockedCompareExchange(&g_menuEntryPending, 0, 0) == 0)
    {
        return false;
    }

    if (InterlockedCompareExchange(&g_menuEntryPending, 0, 1) != 1)
    {
        return false;
    }

    if (outEntry != nullptr)
    {
        outEntry->mode = g_menuEntryMode;
        takeover::CopyString(
            outEntry->origin,
            sizeof(outEntry->origin),
            g_menuEntryOrigin);
    }

    mod::Log(
        "RECOVERY_MENU_PENDING_CONSUMED origin=%s screenContext=0x%08X mode=%d",
        g_menuEntryOrigin,
        screenContext,
        g_menuEntryMode);
    mod::Log(
        "RECOVERY_MENU_PENDING_CLEARED reason=consumed origin=%s",
        g_menuEntryOrigin);
    mod::Log(
        "GAMEPLAY_EXIT_TITLE_CONTINUATION_REACHED screenContext=0x%08X origin=%s mode=%d",
        screenContext,
        g_menuEntryOrigin,
        g_menuEntryMode);

    InterlockedExchange(&g_menuEntryConsumed, 1);
    (void)takeover::SuppressDeferredCancelCleanupAfterGameplayRecovery(g_menuEntryOrigin);
    InterlockedExchange(&g_recoveryInProgress, 0);
    InterlockedExchange(&g_recoveryCompleted, 1);
    SetRecoveryState(RecoveryStateCompleted, g_menuEntryOrigin);
    return true;
}

bool CompleteFrontendReturnMenuEntry(
    uint32_t screenContext,
    PendingGameplayExitMenuEntry* outEntry)
{
    if (InterlockedCompareExchange(&g_menuEntryPending, 0, 0) == 0)
    {
        return false;
    }

    if (InterlockedCompareExchange(&g_menuEntryPending, 0, 1) != 1)
    {
        return false;
    }

    if (outEntry != nullptr)
    {
        outEntry->mode = g_menuEntryMode;
        takeover::CopyString(
            outEntry->origin,
            sizeof(outEntry->origin),
            g_menuEntryOrigin);
    }

    mod::Log(
        "RECOVERY_MENU_PENDING_CONSUMED origin=%s screenContext=0x%08X mode=%d source=frontend_return",
        g_menuEntryOrigin,
        screenContext,
        g_menuEntryMode);
    mod::Log(
        "RECOVERY_MENU_PENDING_CLEARED reason=frontend_return_consumed origin=%s",
        g_menuEntryOrigin);
    mod::Log(
        "GAMEPLAY_EXIT_TITLE_CONTINUATION_REACHED screenContext=0x%08X origin=%s mode=%d",
        screenContext,
        g_menuEntryOrigin,
        g_menuEntryMode);

    InterlockedExchange(&g_menuEntryConsumed, 1);
    (void)takeover::SuppressDeferredCancelCleanupAfterGameplayRecovery(g_menuEntryOrigin);
    InterlockedExchange(&g_recoveryInProgress, 0);
    InterlockedExchange(&g_recoveryCompleted, 1);
    SetRecoveryState(RecoveryStateCompleted, g_menuEntryOrigin);
    return true;
}
}
