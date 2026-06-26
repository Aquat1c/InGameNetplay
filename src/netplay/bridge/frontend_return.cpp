#include "netplay/bridge/frontend_return.h"

#include "netplay/bridge/async_hosting.h"
#include "netplay/bridge/gameplay_exit_recovery.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/constants.h"

#include "logger.h"

#include <cstdio>
#include <cstring>
#include <windows.h>

// Rendered from the battle update hook below; defined in the hooks layer
// (title_overlay_text.cpp). Forward-declared here to avoid pulling the whole
// hooks/internal header into this bridge translation unit.
namespace netplay::hooks::internal
{
void DrawAsyncHostGameplayOverlay(uint32_t screenContext);
}

namespace netplay::bridge::frontend_return
{
#if defined(_M_IX86)
extern "C" void FrontendReturnBattleUpdateThunk();
extern "C" void FrontendReturnResultUpdateThunk();
extern "C" void FrontendReturnLoadingUpdateThunk();
#endif

namespace
{
using ScreenUpdateFn = char(__thiscall*)(uint32_t screenContext);

constexpr uintptr_t kVaScreenObjectTable = 0x00790110u;
constexpr uintptr_t kVaCurrentScreenIndex = 0x00790148u;
constexpr uintptr_t kVaGameSystemPtr = 0x0079010Cu;
constexpr uint32_t kGameSystemModeOffset = 4964u;
constexpr uint32_t kBattlePauseOffset = 1416u;
constexpr DWORD kLoadingGraceMs = 1500u;
constexpr DWORD kLoadingCriticalMs = 3000u;
constexpr DWORD kLoadingForceFallbackMs = 10000u;
constexpr DWORD kNativeExitTimeoutMs = 5000u;
constexpr DWORD kProgressLogThrottleMs = 1000u;

struct ActiveReturnRequest
{
    ReturnTarget target = ReturnTarget::Title;
    ReturnOwner owner = ReturnOwner::Diagnostic;
    char reason[128] = {};
    bool preserveHelper = true;
    bool allowNativeBattleExit = false;
    bool allowNativeResultExit = false;
    bool allowForcedFallback = false;
    bool enterNetplayMenuAfterTitle = false;
    bool confirmedDisconnect = false;
};

volatile LONG g_active = 0;
volatile LONG g_titleContinuationPending = 0;
volatile LONG g_netplayMenuContinuationConsumed = 0;
ReturnState g_state = ReturnState::Idle;
ActiveReturnRequest g_request = {};
ReturnOwner g_continuationOwner = ReturnOwner::Diagnostic;
ReturnTarget g_continuationTarget = ReturnTarget::Title;
ScreenId g_startScreen = ScreenId::Unknown;
ScreenId g_nativeStartScreen = ScreenId::Unknown;
DWORD g_startTick = 0;
DWORD g_stateStartTick = 0;
DWORD g_nativeStartTick = 0;
DWORD g_loadingStartTick = 0;
DWORD g_screenStableStartTick = 0;
DWORD g_lastContextLogTick = 0;
DWORD g_lastNativeProgressLogTick = 0;
DWORD g_lastLoadingWaitLogTick = 0;
DWORD g_lastLoadingTransientLogTick = 0;
DWORD g_lastLoadingStuckLogTick = 0;
DWORD g_lastFallbackSuppressedLogTick = 0;
uint8_t g_lastObservedRawScreen = 255;
bool g_titleReachedLogged = false;
bool g_menuContinuationLogged = false;
bool g_loadingUnblockDone = false;
bool g_nativeBattleUpdateReached = false;
bool g_nativeBattlePatchRestoreOk = true;
bool g_nativeCleanupFallbackSuppressed = false;

uint32_t g_battleUpdateSlotAddress = 0;
uint32_t g_resultUpdateSlotAddress = 0;
uint32_t g_loadingUpdateSlotAddress = 0;
ScreenUpdateFn g_originalBattleUpdate = nullptr;
ScreenUpdateFn g_originalResultUpdate = nullptr;
ScreenUpdateFn g_originalLoadingUpdate = nullptr;

uintptr_t ResolveEfzVa(uintptr_t va)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (base == 0)
    {
        return va;
    }
    return base + (va - netplay::constants::kEfzImageBase);
}

bool ReadU8(uintptr_t address, uint8_t* outValue)
{
    if (outValue == nullptr || address == 0)
    {
        return false;
    }

    __try
    {
        *outValue = *reinterpret_cast<const volatile uint8_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

bool ReadU32(uintptr_t address, uint32_t* outValue)
{
    if (outValue == nullptr || address == 0)
    {
        return false;
    }

    __try
    {
        *outValue = *reinterpret_cast<const volatile uint32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

uint8_t ReadCurrentRawScreenForDiag()
{
    uint8_t rawScreen = 255;
    (void)ReadU8(ResolveEfzVa(kVaCurrentScreenIndex), &rawScreen);
    return rawScreen;
}

void ReadBattleUpdateDiag(
    uint32_t screenContext,
    uint8_t* out44,
    uint8_t* out45,
    uint32_t* out1416)
{
    if (out44 != nullptr)
    {
        *out44 = 255;
        (void)ReadU8(
            screenContext + netplay::constants::kOffsetScreenInitState,
            out44);
    }
    if (out45 != nullptr)
    {
        *out45 = 255;
        (void)ReadU8(
            screenContext + netplay::constants::kOffsetScreenExitState,
            out45);
    }
    if (out1416 != nullptr)
    {
        *out1416 = 0xFFFFFFFFu;
        (void)ReadU32(screenContext + kBattlePauseOffset, out1416);
    }
}

bool WriteU8(uintptr_t address, uint8_t value)
{
    if (address == 0)
    {
        return false;
    }

    __try
    {
        *reinterpret_cast<volatile uint8_t*>(address) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return false;
}

ScreenId ScreenFromRaw(uint8_t raw)
{
    switch (raw)
    {
    case 0:
        return ScreenId::Title;
    case 1:
        return ScreenId::CharacterSelect;
    case 2:
        return ScreenId::Loading;
    case 3:
        return ScreenId::Battle;
    case 4:
        return ScreenId::Unused4;
    case 5:
        return ScreenId::Result;
    case 6:
        return ScreenId::Settings;
    case 7:
        return ScreenId::StaffRoll;
    case 8:
        return ScreenId::ReplaySelect;
    }
    return ScreenId::Unknown;
}

uint8_t ScreenToRaw(ScreenId screen)
{
    return static_cast<uint8_t>(screen);
}

const char* TargetToString(ReturnTarget target)
{
    switch (target)
    {
    case ReturnTarget::Title:
        return "title";
    case ReturnTarget::NetplayMenu:
        return "netplay_menu";
    case ReturnTarget::CharacterSelect:
        return "charselect";
    }
    return "unknown";
}

const char* OwnerToString(ReturnOwner owner)
{
    switch (owner)
    {
    case ReturnOwner::DisconnectRecovery:
        return "disconnect_recovery";
    case ReturnOwner::AsyncHostAccept:
        return "async_host_accept";
    case ReturnOwner::UserMenuExit:
        return "user_menu_exit";
    case ReturnOwner::Diagnostic:
        return "diagnostic";
    }
    return "unknown";
}

const char* StateToString(ReturnState state)
{
    switch (state)
    {
    case ReturnState::Idle:
        return "idle";
    case ReturnState::Requested:
        return "requested";
    case ReturnState::WaitForSafePoint:
        return "wait_for_safe_point";
    case ReturnState::LoadingWait:
        return "loading_wait";
    case ReturnState::LoadingCriticalUnblock:
        return "loading_critical_unblock";
    case ReturnState::WaitingForLoadingToResolve:
        return "waiting_for_loading_to_resolve";
    case ReturnState::NativeExitRequested:
        return "native_exit_requested";
    case ReturnState::WaitingForNativeExit:
        return "waiting_for_native_exit";
    case ReturnState::ForceTitleFallback:
        return "force_title_fallback";
    case ReturnState::TitleReached:
        return "title_reached";
    case ReturnState::NetplayMenuContinuationPending:
        return "netplay_menu_continuation_pending";
    case ReturnState::Complete:
        return "complete";
    case ReturnState::Failed:
        return "failed";
    }
    return "unknown";
}

bool IsTitleFamily(ScreenId screen)
{
    return screen == ScreenId::Title
        || screen == ScreenId::Settings
        || screen == ScreenId::ReplaySelect;
}

DWORD ElapsedMs(DWORD now, DWORD start)
{
    return start == 0 ? 0 : now - start;
}

void CopyReason(char* dest, size_t destSize, const char* reason)
{
    if (dest == nullptr || destSize == 0)
    {
        return;
    }
    dest[0] = '\0';
    if (reason == nullptr || reason[0] == '\0')
    {
        reason = "unknown";
    }
    std::snprintf(dest, destSize, "%s", reason);
    dest[destSize - 1] = '\0';
}

uint32_t ReadScreenObject(uint8_t index)
{
    uint32_t context = 0;
    const uintptr_t table = ResolveEfzVa(kVaScreenObjectTable);
    (void)ReadU32(table + static_cast<uintptr_t>(index) * sizeof(uint32_t), &context);
    return context;
}

void LogContext(const FrontendContext& ctx)
{
    mod::Log(
        "FRONTEND_RETURN_CONTEXT screen=%u mode=%u ctx=0x%08lX title=0x%08lX charselect=0x%08lX loading=0x%08lX battle=0x%08lX result=0x%08lX",
        static_cast<unsigned>(ctx.rawScreen),
        static_cast<unsigned>(ctx.gameModeRaw),
        static_cast<unsigned long>(ctx.screenContext),
        static_cast<unsigned long>(ctx.titleContext),
        static_cast<unsigned long>(ctx.charselectContext),
        static_cast<unsigned long>(ctx.loadingContext),
        static_cast<unsigned long>(ctx.battleContext),
        static_cast<unsigned long>(ctx.resultContext));
}

void LogContextIfDue(const FrontendContext& ctx, DWORD now)
{
    if (ctx.rawScreen != g_lastObservedRawScreen)
    {
        g_lastObservedRawScreen = ctx.rawScreen;
        g_screenStableStartTick = now;
        LogContext(ctx);
        g_lastContextLogTick = now;
        return;
    }

    if (g_lastContextLogTick == 0 || now - g_lastContextLogTick >= kProgressLogThrottleMs)
    {
        LogContext(ctx);
        g_lastContextLogTick = now;
    }
}

void SetState(ReturnState newState, const char* reason)
{
    if (g_state == newState)
    {
        return;
    }

    const ReturnState oldState = g_state;
    g_state = newState;
    g_stateStartTick = GetTickCount();
    mod::Log(
        "FRONTEND_RETURN_STATE old=%s new=%s reason=%s",
        StateToString(oldState),
        StateToString(newState),
        (reason != nullptr && reason[0] != '\0') ? reason : "unknown");
}

ReturnResult BuildResult(bool accepted, ScreenId currentScreen)
{
    ReturnResult result = {};
    result.accepted = accepted;
    result.completed = (g_state == ReturnState::Complete);
    result.pending = HasPendingReturn();
    result.state = g_state;
    result.startScreen = g_startScreen;
    result.currentScreen = currentScreen;
    return result;
}

void CompleteReturn(const char* reason)
{
    SetState(ReturnState::Complete, reason);
    InterlockedExchange(&g_active, 0);
    InterlockedExchange(&g_titleContinuationPending, 0);
    mod::Log(
        "FRONTEND_RETURN_COMPLETE owner=%s target=%s elapsedMs=%lu",
        OwnerToString(g_request.owner),
        TargetToString(g_request.target),
        static_cast<unsigned long>(ElapsedMs(GetTickCount(), g_startTick)));
}

void FailReturn(const char* reason)
{
    SetState(ReturnState::Failed, reason);
    InterlockedExchange(&g_active, 0);
    InterlockedExchange(&g_titleContinuationPending, 0);
    mod::Log(
        "FRONTEND_RETURN_FAILED reason=%s owner=%s target=%s",
        (reason != nullptr && reason[0] != '\0') ? reason : "unknown",
        OwnerToString(g_request.owner),
        TargetToString(g_request.target));
}

bool InstallScreenUpdateHook(
    uint8_t screenIndex,
    void* hookThunk,
    uint32_t* slotAddress,
    ScreenUpdateFn* originalUpdate,
    const char* tag)
{
#if defined(_M_IX86)
    if (hookThunk == nullptr || slotAddress == nullptr || originalUpdate == nullptr)
    {
        return false;
    }
    if (*slotAddress != 0)
    {
        return *originalUpdate != nullptr;
    }

    const uint32_t screenObject = ReadScreenObject(screenIndex);
    if (screenObject == 0)
    {
        mod::Log("FRONTEND_RETURN_HOOK_INSTALL_FAILED screen=%u reason=no_context tag=%s",
            static_cast<unsigned>(screenIndex),
            tag != nullptr ? tag : "unknown");
        return false;
    }

    uint32_t vtable = 0;
    if (!ReadU32(screenObject, &vtable) || vtable == 0)
    {
        mod::Log("FRONTEND_RETURN_HOOK_INSTALL_FAILED screen=%u reason=no_vtable tag=%s",
            static_cast<unsigned>(screenIndex),
            tag != nullptr ? tag : "unknown");
        return false;
    }

    auto* const updateSlot = reinterpret_cast<uint32_t*>(vtable + 4);
    uint32_t originalAddress = 0;
    if (!ReadU32(reinterpret_cast<uintptr_t>(updateSlot), &originalAddress) || originalAddress == 0)
    {
        mod::Log("FRONTEND_RETURN_HOOK_INSTALL_FAILED screen=%u reason=no_update tag=%s",
            static_cast<unsigned>(screenIndex),
            tag != nullptr ? tag : "unknown");
        return false;
    }

    const uint32_t hookAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hookThunk));
    if (originalAddress == hookAddress)
    {
        *slotAddress = reinterpret_cast<uint32_t>(updateSlot);
        return *originalUpdate != nullptr;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(updateSlot, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        mod::Log("FRONTEND_RETURN_HOOK_INSTALL_FAILED screen=%u reason=protect tag=%s err=%lu",
            static_cast<unsigned>(screenIndex),
            tag != nullptr ? tag : "unknown",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    *updateSlot = hookAddress;
    DWORD ignored = 0;
    (void)VirtualProtect(updateSlot, sizeof(uint32_t), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), updateSlot, sizeof(uint32_t));

    *originalUpdate = reinterpret_cast<ScreenUpdateFn>(originalAddress);
    *slotAddress = reinterpret_cast<uint32_t>(updateSlot);
    mod::Log(
        "FRONTEND_RETURN_HOOK_INSTALLED screen=%u tag=%s slot=0x%08lX original=0x%08lX hook=0x%08lX",
        static_cast<unsigned>(screenIndex),
        tag != nullptr ? tag : "unknown",
        static_cast<unsigned long>(*slotAddress),
        static_cast<unsigned long>(originalAddress),
        static_cast<unsigned long>(hookAddress));
    return true;
#else
    (void)screenIndex;
    (void)hookThunk;
    (void)slotAddress;
    (void)originalUpdate;
    (void)tag;
    return false;
#endif
}

void EnsureFrontendReturnUpdateHooksImpl()
{
#if defined(_M_IX86)
    (void)InstallScreenUpdateHook(
        2,
        reinterpret_cast<void*>(&FrontendReturnLoadingUpdateThunk),
        &g_loadingUpdateSlotAddress,
        &g_originalLoadingUpdate,
        "loading");
    (void)InstallScreenUpdateHook(
        3,
        reinterpret_cast<void*>(&FrontendReturnBattleUpdateThunk),
        &g_battleUpdateSlotAddress,
        &g_originalBattleUpdate,
        "battle");
    (void)InstallScreenUpdateHook(
        5,
        reinterpret_cast<void*>(&FrontendReturnResultUpdateThunk),
        &g_resultUpdateSlotAddress,
        &g_originalResultUpdate,
        "result");
#endif
}

bool RequestNativeExit(const FrontendContext& ctx, ScreenId screen, const char* reason)
{
    uintptr_t targetContext = ctx.screenContext;
    if (screen == ScreenId::CharacterSelect)
    {
        targetContext = ctx.charselectContext;
    }
    else if (screen == ScreenId::Battle)
    {
        targetContext = ctx.battleContext;
    }
    else if (screen == ScreenId::Result)
    {
        targetContext = ctx.resultContext;
    }

    if (targetContext == 0)
    {
        FailReturn("native_exit_missing_context");
        return false;
    }

    uint8_t old45 = 255;
    uint32_t old1416 = 0xFFFFFFFFu;
    uint32_t new1416 = 0xFFFFFFFFu;
    (void)ReadU8(targetContext + netplay::constants::kOffsetScreenExitState, &old45);
    if (screen == ScreenId::Battle)
    {
        (void)ReadU32(targetContext + kBattlePauseOffset, &old1416);
        const int patchStateBeforeNativeExit =
            takeover::GetRevivalGraphicsPatchState();
        mod::Log(
            "FRONTEND_RETURN_PATCH_STATE_BEFORE_NATIVE_EXIT state=%d",
            patchStateBeforeNativeExit);
        const bool patchRestoreOk =
            takeover::EnsureRevivalGraphicsPatchSetEnabled(
                "frontend_return_before_native_battle_exit");
        g_nativeBattlePatchRestoreOk = patchRestoreOk;
        g_nativeBattleUpdateReached = false;
        mod::Log(
            "FRONTEND_RETURN_PATCH_STATE_RESTORED result=%d",
            patchRestoreOk ? 1 : 0);
    }
    const bool writeExitOk =
        WriteU8(targetContext + netplay::constants::kOffsetScreenExitState, 1);
    bool unpauseOk = true;
    if (screen == ScreenId::Battle)
    {
        unpauseOk = WriteU8(targetContext + kBattlePauseOffset, 0);
        (void)ReadU32(targetContext + kBattlePauseOffset, &new1416);
        mod::Log(
            "FRONTEND_RETURN_BATTLE_EXIT_REQUEST old45=%u new45=1 "
            "old1416=%lu new1416=%lu",
            static_cast<unsigned>(old45),
            static_cast<unsigned long>(old1416),
            static_cast<unsigned long>(new1416));
    }

    mod::Log(
        "FRONTEND_RETURN_NATIVE_EXIT_REQUEST screen=%u ctx=0x%08lX old45=%u new45=1 result=%d unpause=%d reason=%s",
        static_cast<unsigned>(ScreenToRaw(screen)),
        static_cast<unsigned long>(targetContext),
        static_cast<unsigned>(old45),
        writeExitOk ? 1 : 0,
        unpauseOk ? 1 : 0,
        (reason != nullptr && reason[0] != '\0') ? reason : "unknown");

    if (!writeExitOk)
    {
        FailReturn("native_exit_write_failed");
        return false;
    }

    g_nativeStartScreen = screen;
    g_nativeStartTick = GetTickCount();
    g_lastNativeProgressLogTick = 0;
    SetState(ReturnState::NativeExitRequested, "native_exit_requested");
    SetState(ReturnState::WaitingForNativeExit, "waiting_for_native_exit");
    return true;
}

void HandleTitleReached(const char* reason)
{
    SetState(ReturnState::TitleReached, reason);
    if (!g_titleReachedLogged)
    {
        g_titleReachedLogged = true;
        mod::Log(
            "FRONTEND_RETURN_TITLE_REACHED owner=%s target=%s",
            OwnerToString(g_request.owner),
            TargetToString(g_request.target));
    }

    if (g_request.target == ReturnTarget::NetplayMenu
        && g_request.enterNetplayMenuAfterTitle)
    {
        g_continuationOwner = g_request.owner;
        g_continuationTarget = g_request.target;
        InterlockedExchange(&g_titleContinuationPending, 1);
        if (!g_menuContinuationLogged)
        {
            g_menuContinuationLogged = true;
            mod::Log(
                "FRONTEND_RETURN_MENU_CONTINUATION_PENDING owner=%s",
                OwnerToString(g_request.owner));
        }
        SetState(
            ReturnState::NetplayMenuContinuationPending,
            "netplay_menu_continuation_pending");
        return;
    }

    CompleteReturn("title_reached");
}

void EnterLoadingWait(const FrontendContext& ctx, DWORD now)
{
    if (g_state != ReturnState::LoadingWait
        && g_state != ReturnState::LoadingCriticalUnblock
        && g_state != ReturnState::WaitingForLoadingToResolve)
    {
        g_loadingStartTick = now;
        g_loadingUnblockDone = false;
        g_lastLoadingWaitLogTick = 0;
        g_lastLoadingTransientLogTick = 0;
        g_lastLoadingStuckLogTick = 0;
        SetState(ReturnState::LoadingWait, "loading_screen");
    }

    if (g_lastLoadingWaitLogTick == 0 || now - g_lastLoadingWaitLogTick >= kProgressLogThrottleMs)
    {
        mod::Log(
            "FRONTEND_RETURN_LOADING_WAIT screen=2 elapsedMs=%lu mode=%u ctx=0x%08lX +44=%u +45=%u",
            static_cast<unsigned long>(ElapsedMs(now, g_loadingStartTick)),
            static_cast<unsigned>(ctx.gameModeRaw),
            static_cast<unsigned long>(ctx.screenContext),
            static_cast<unsigned>(ctx.lifecycle44),
            static_cast<unsigned>(ctx.exit45));
        g_lastLoadingWaitLogTick = now;
    }
}

void PerformLoadingCriticalUnblock(DWORD now)
{
    if (g_loadingUnblockDone)
    {
        return;
    }
    g_loadingUnblockDone = true;

    LONG consoleErrorSerial = 0;
    if (takeover::g_hostBlock != nullptr)
    {
        consoleErrorSerial =
            InterlockedCompareExchange(&takeover::g_hostBlock->consoleErrorSerial, 0, 0);
    }

    const bool helperAlive = takeover::g_revivalProcess != nullptr;
    SetState(ReturnState::LoadingCriticalUnblock, "loading_critical_unblock");
    mod::Log(
        "FRONTEND_RETURN_LOADING_UNBLOCK_BEGIN reason=%s helperAlive=%d consoleErrorSerial=%ld stallTier=%s",
        g_request.reason,
        helperAlive ? 1 : 0,
        static_cast<long>(consoleErrorSerial),
        "unknown");

    bool neutralizeOk = false;
    if (g_request.confirmedDisconnect)
    {
        takeover::NeutralizeRevivalSessionVtable();
        neutralizeOk = true;
    }
    mod::Log(
        "FRONTEND_RETURN_LOADING_UNBLOCK_STEP step=neutralize_vtable result=%d",
        neutralizeOk ? 1 : 0);

    bool terminateOk = true;
    if (g_request.owner == ReturnOwner::DisconnectRecovery && !g_request.preserveHelper)
    {
        if (takeover::g_revivalProcess != nullptr)
        {
            const HANDLE helper = takeover::g_revivalProcess;
            const DWORD pid = takeover::g_revivalProcessId;
            const BOOL termOk = TerminateProcess(helper, 0);
            const DWORD termErr = termOk ? 0 : GetLastError();
            const BOOL closeOk = CloseHandle(helper);
            takeover::g_revivalProcess = nullptr;
            takeover::g_revivalProcessId = 0;
            terminateOk = termOk && closeOk;
            mod::Log(
                "FRONTEND_RETURN_LOADING_UNBLOCK_STEP step=terminate_helper result=%d pid=%lu termOk=%d closeOk=%d err=%lu",
                terminateOk ? 1 : 0,
                static_cast<unsigned long>(pid),
                termOk ? 1 : 0,
                closeOk ? 1 : 0,
                static_cast<unsigned long>(termErr));
        }
        else
        {
            mod::Log("FRONTEND_RETURN_LOADING_UNBLOCK_STEP step=terminate_helper result=1 pid=0 termOk=1 closeOk=1 err=0");
        }
    }
    else
    {
        mod::Log("FRONTEND_RETURN_LOADING_UNBLOCK_STEP step=terminate_helper result=1 preserved=1");
    }

    (void)terminateOk;
    SetState(ReturnState::WaitingForLoadingToResolve, "loading_unblock_done");
    g_stateStartTick = now;
}

void RequestForceTitleFallback(const char* reason)
{
    if (!g_request.allowForcedFallback)
    {
        mod::Log(
            "FRONTEND_RETURN_LOADING_FORCE_FALLBACK_SUPPRESSED reason=disabled");
        FailReturn("force_fallback_disabled");
        return;
    }

    if ((g_request.owner == ReturnOwner::DisconnectRecovery
            || g_request.owner == ReturnOwner::AsyncHostAccept)
        && g_nativeStartScreen == ScreenId::Battle
        && (g_state == ReturnState::NativeExitRequested
            || g_state == ReturnState::WaitingForNativeExit)
        && g_nativeBattleUpdateReached
        && g_nativeBattlePatchRestoreOk)
    {
        if (!g_nativeCleanupFallbackSuppressed)
        {
            g_nativeCleanupFallbackSuppressed = true;
            mod::Log(
                "FRONTEND_RETURN_FORCED_FALLBACK_SUPPRESSED reason=native_cleanup_reached");
            mod::Log("RECOVERY_PALETTE_BLACKOUT_SKIPPED reason=native_cleanup_path");
        }
        return;
    }

    SetState(ReturnState::ForceTitleFallback, reason);
}

void TryForceTitleFallback(const char* reason)
{
    const FrontendContext ctx = CaptureFrontendContext();
    if (ctx.insideFrameTick)
    {
        const DWORD now = GetTickCount();
        if (g_lastFallbackSuppressedLogTick == 0
            || now - g_lastFallbackSuppressedLogTick >= kProgressLogThrottleMs)
        {
            mod::Log(
                "FRONTEND_RETURN_LOADING_FORCE_FALLBACK_SUPPRESSED reason=inside_frame_tick");
            g_lastFallbackSuppressedLogTick = now;
        }
        SetState(ReturnState::WaitForSafePoint, "inside_frame_tick");
        return;
    }

    const bool forceOk = takeover::ForceGameModeToTitle();
    const bool dispatchOk =
        takeover::RestoreExeDispatchOriginalBytesForTitle(
            "FrontendReturn_force_title_fallback");
    mod::Log(
        "FRONTEND_RETURN_FORCED_FALLBACK_USED reason=%s",
        (reason != nullptr && reason[0] != '\0') ? reason : g_request.reason);
    mod::Log("RECOVERY_PALETTE_BLACKOUT_USED reason=forced_fallback");
    mod::Log(
        "FRONTEND_RETURN_FORCE_FALLBACK reason=%s result=%d",
        (reason != nullptr && reason[0] != '\0') ? reason : g_request.reason,
        forceOk ? 1 : 0);
    if (dispatchOk)
    {
        mod::Log("FRONTEND_RETURN_FORCE_FALLBACK titleDispatchRestored=1");
    }
    if (!forceOk)
    {
        FailReturn("force_title_failed");
        return;
    }

    HandleTitleReached("force_title_fallback");
}

void ClassifyAndAdvance(const FrontendContext& ctx);

void HandleLoading(const FrontendContext& ctx, DWORD now)
{
    if (ctx.screen != ScreenId::Loading)
    {
        mod::Log(
            "FRONTEND_RETURN_LOADING_RESOLVED old=2 new=%u elapsedMs=%lu",
            static_cast<unsigned>(ctx.rawScreen),
            static_cast<unsigned long>(ElapsedMs(now, g_loadingStartTick)));
        SetState(ReturnState::Requested, "loading_resolved");
        ClassifyAndAdvance(ctx);
        return;
    }

    EnterLoadingWait(ctx, now);

    const DWORD loadingElapsed = ElapsedMs(now, g_loadingStartTick);
    if (loadingElapsed < kLoadingGraceMs)
    {
        if (g_lastLoadingTransientLogTick == 0
            || now - g_lastLoadingTransientLogTick >= kProgressLogThrottleMs)
        {
            mod::Log(
                "FRONTEND_RETURN_LOADING_TRANSIENT screen2Ms=%lu",
                static_cast<unsigned long>(loadingElapsed));
            g_lastLoadingTransientLogTick = now;
        }
        return;
    }

    const DWORD stableMs = ElapsedMs(now, g_screenStableStartTick);
    if (g_lastLoadingStuckLogTick == 0
        || now - g_lastLoadingStuckLogTick >= kProgressLogThrottleMs)
    {
        mod::Log(
            "FRONTEND_RETURN_LOADING_STUCK elapsedMs=%lu screenStableMs=%lu frameTick=%u syncFrame=%d",
            static_cast<unsigned long>(loadingElapsed),
            static_cast<unsigned long>(stableMs),
            takeover::GetGameplayExitRecoveryFrameTick(),
            -1);
        g_lastLoadingStuckLogTick = now;
    }

    if (!g_loadingUnblockDone
        && g_request.confirmedDisconnect
        && loadingElapsed >= kLoadingCriticalMs)
    {
        PerformLoadingCriticalUnblock(now);
        return;
    }

    if (loadingElapsed >= kLoadingForceFallbackMs)
    {
        if (g_request.allowForcedFallback)
        {
            mod::Log(
                "FRONTEND_RETURN_LOADING_FORCE_FALLBACK elapsedMs=%lu reason=%s",
                static_cast<unsigned long>(loadingElapsed),
                g_request.reason);
            RequestForceTitleFallback("loading_force_fallback");
        }
        else
        {
            mod::Log(
                "FRONTEND_RETURN_LOADING_FORCE_FALLBACK_SUPPRESSED reason=disabled");
            FailReturn("loading_force_fallback_disabled");
        }
    }
}

void HandleNativeWait(const FrontendContext& ctx, DWORD now)
{
    if (g_nativeStartScreen == ScreenId::Unknown)
    {
        FailReturn("native_wait_missing_start");
        return;
    }

    if (ctx.screen != g_nativeStartScreen)
    {
        mod::Log(
            "FRONTEND_RETURN_NATIVE_EXIT_RESOLVED start=%u current=%u elapsedMs=%lu",
            static_cast<unsigned>(ScreenToRaw(g_nativeStartScreen)),
            static_cast<unsigned>(ctx.rawScreen),
            static_cast<unsigned long>(ElapsedMs(now, g_nativeStartTick)));
        SetState(ReturnState::Requested, "native_exit_resolved");
        ClassifyAndAdvance(ctx);
        return;
    }

    if (g_lastNativeProgressLogTick == 0
        || now - g_lastNativeProgressLogTick >= kProgressLogThrottleMs)
    {
        mod::Log(
            "FRONTEND_RETURN_NATIVE_EXIT_PROGRESS start=%u current=%u +44=%u +45=%u elapsedMs=%lu",
            static_cast<unsigned>(ScreenToRaw(g_nativeStartScreen)),
            static_cast<unsigned>(ctx.rawScreen),
            static_cast<unsigned>(ctx.lifecycle44),
            static_cast<unsigned>(ctx.exit45),
            static_cast<unsigned long>(ElapsedMs(now, g_nativeStartTick)));
        g_lastNativeProgressLogTick = now;
    }

    if (ElapsedMs(now, g_nativeStartTick) >= kNativeExitTimeoutMs)
    {
        mod::Log(
            "FRONTEND_RETURN_NATIVE_EXIT_TIMEOUT start=%u current=%u elapsedMs=%lu",
            static_cast<unsigned>(ScreenToRaw(g_nativeStartScreen)),
            static_cast<unsigned>(ctx.rawScreen),
            static_cast<unsigned long>(ElapsedMs(now, g_nativeStartTick)));
        RequestForceTitleFallback("native_exit_timeout");
    }
}

void ClassifyAndAdvance(const FrontendContext& ctx)
{
    if (ctx.screen == ScreenId::Unknown)
    {
        mod::Log(
            "FRONTEND_RETURN_FAILED reason=invalid_screen raw=%u",
            static_cast<unsigned>(ctx.rawScreen));
        RequestForceTitleFallback("invalid_screen");
        return;
    }

    if (g_request.target == ReturnTarget::CharacterSelect)
    {
        if (ctx.screen == ScreenId::CharacterSelect)
        {
            CompleteReturn("charselect_reached");
            return;
        }
    }
    else if (IsTitleFamily(ctx.screen))
    {
        HandleTitleReached("title_family_reached");
        return;
    }

    switch (ctx.screen)
    {
    case ScreenId::Title:
    case ScreenId::Settings:
    case ScreenId::ReplaySelect:
        HandleTitleReached("title_family_reached");
        return;
    case ScreenId::CharacterSelect:
        if (g_request.target == ReturnTarget::CharacterSelect)
        {
            CompleteReturn("charselect_reached");
            return;
        }
        (void)RequestNativeExit(ctx, ScreenId::CharacterSelect, "charselect_to_title");
        return;
    case ScreenId::Loading:
        HandleLoading(ctx, GetTickCount());
        return;
    case ScreenId::Battle:
        if (g_request.allowNativeBattleExit)
        {
            (void)RequestNativeExit(ctx, ScreenId::Battle, "battle_native_exit");
        }
        else
        {
            RequestForceTitleFallback("battle_native_exit_disabled");
        }
        return;
    case ScreenId::Result:
        if (g_request.allowNativeResultExit)
        {
            (void)RequestNativeExit(ctx, ScreenId::Result, "result_native_exit");
        }
        else
        {
            RequestForceTitleFallback("result_native_exit_disabled");
        }
        return;
    case ScreenId::Unused4:
    case ScreenId::StaffRoll:
    case ScreenId::Unknown:
        RequestForceTitleFallback("unsupported_screen");
        return;
    }
}

char HandleNativeUpdateReturn(ScreenId updateScreen, char nativeResult)
{
    const uint8_t rawResult = static_cast<uint8_t>(nativeResult);
    const ScreenId resultScreen = ScreenFromRaw(rawResult);
    const DWORD now = GetTickCount();

    if (!HasPendingReturn())
    {
        return nativeResult;
    }

    if (updateScreen == ScreenId::Loading
        && rawResult != ScreenToRaw(ScreenId::Loading)
        && (g_state == ReturnState::LoadingWait
            || g_state == ReturnState::LoadingCriticalUnblock
            || g_state == ReturnState::WaitingForLoadingToResolve))
    {
        mod::Log(
            "FRONTEND_RETURN_LOADING_RESOLVED old=2 new=%u elapsedMs=%lu",
            static_cast<unsigned>(rawResult),
            static_cast<unsigned long>(ElapsedMs(now, g_loadingStartTick)));
        SetState(ReturnState::Requested, "loading_update_resolved");
        return nativeResult;
    }

    if (g_state != ReturnState::WaitingForNativeExit
        || g_nativeStartScreen != updateScreen
        || resultScreen == updateScreen)
    {
        return nativeResult;
    }

    mod::Log(
        "FRONTEND_RETURN_NATIVE_EXIT_RESOLVED start=%u current=%u elapsedMs=%lu",
        static_cast<unsigned>(ScreenToRaw(updateScreen)),
        static_cast<unsigned>(rawResult),
        static_cast<unsigned long>(ElapsedMs(now, g_nativeStartTick)));

    if ((g_request.owner == ReturnOwner::DisconnectRecovery
            || g_request.owner == ReturnOwner::AsyncHostAccept)
        && g_request.target == ReturnTarget::NetplayMenu
        && (updateScreen == ScreenId::Battle || updateScreen == ScreenId::Result)
        && resultScreen != ScreenId::Title
        && (resultScreen == ScreenId::CharacterSelect || IsTitleFamily(resultScreen)))
    {
        mod::Log(
            "FRONTEND_RETURN_FORCED_FALLBACK_SUPPRESSED reason=native_cleanup_reached");
        mod::Log("RECOVERY_PALETTE_BLACKOUT_SKIPPED reason=native_cleanup_path");
        mod::Log(
            "FRONTEND_RETURN_NATIVE_EXIT_REDIRECT start=%u old=%u new=0 target=%s reason=disconnect_recovery_non_title_frontend",
            static_cast<unsigned>(ScreenToRaw(updateScreen)),
            static_cast<unsigned>(rawResult),
            TargetToString(g_request.target));
        HandleTitleReached("native_exit_redirect_title");
        return 0;
    }

    if ((g_request.target == ReturnTarget::Title
            || g_request.target == ReturnTarget::NetplayMenu)
        && IsTitleFamily(resultScreen))
    {
        HandleTitleReached("native_exit_title");
        return nativeResult;
    }

    if (g_request.target == ReturnTarget::CharacterSelect
        && resultScreen == ScreenId::CharacterSelect)
    {
        CompleteReturn("native_exit_charselect");
        return nativeResult;
    }

    SetState(ReturnState::Requested, "native_update_result");
    return nativeResult;
}
} // namespace

void EnsureFrontendReturnUpdateHooks()
{
    EnsureFrontendReturnUpdateHooksImpl();
}

FrontendContext CaptureFrontendContext()
{
    FrontendContext ctx = {};
    ctx.rawScreen = 255;
    ctx.screen = ScreenId::Unknown;
    ctx.gameModeRaw = 255;
    ctx.lifecycle44 = 255;
    ctx.exit45 = 255;

    uint8_t rawScreen = 255;
    if (ReadU8(ResolveEfzVa(kVaCurrentScreenIndex), &rawScreen))
    {
        ctx.rawScreen = rawScreen;
        ctx.screen = ScreenFromRaw(rawScreen);
    }

    ctx.titleContext = ReadScreenObject(0);
    ctx.charselectContext = ReadScreenObject(1);
    ctx.loadingContext = ReadScreenObject(2);
    ctx.battleContext = ReadScreenObject(3);
    ctx.resultContext = ReadScreenObject(5);
    if (rawScreen <= 8)
    {
        ctx.screenContext = ReadScreenObject(rawScreen);
    }

    if (ctx.screenContext != 0)
    {
        (void)ReadU8(
            ctx.screenContext + netplay::constants::kOffsetScreenInitState,
            &ctx.lifecycle44);
        (void)ReadU8(
            ctx.screenContext + netplay::constants::kOffsetScreenExitState,
            &ctx.exit45);
    }

    uint32_t gameSystem = 0;
    if (ctx.screenContext != 0)
    {
        (void)ReadU32(
            ctx.screenContext + netplay::constants::kOffsetGameSystem,
            &gameSystem);
    }
    if (gameSystem == 0)
    {
        (void)ReadU32(ResolveEfzVa(kVaGameSystemPtr), &gameSystem);
    }
    if (gameSystem != 0)
    {
        (void)ReadU8(gameSystem + kGameSystemModeOffset, &ctx.gameModeRaw);
    }

    const NetbridgeStatus status = netplay::bridge::GetStatus();
    const auto phase = static_cast<NetbridgePhase>(status.phase);
    ctx.netplaySessionActive =
        phase == NetbridgePhase::Connecting
        || phase == NetbridgePhase::DelaySetup
        || phase == NetbridgePhase::Connected;
    ctx.recoveryInProgress =
        recovery::IsGameplayExitRecoveryInProgress()
        || recovery::HasPendingGameplayExitMenuEntry();
    ctx.insideFrameTick =
        takeover::IsGameplayExitRecoveryInsideFrameTick()
        || takeover::IsInsideFrameTick();
    return ctx;
}

ReturnResult BeginReturnToFrontend(const ReturnRequest& request)
{
    if (InterlockedCompareExchange(&g_active, 1, 0) != 0
        || InterlockedCompareExchange(&g_titleContinuationPending, 0, 0) != 0)
    {
        const FrontendContext ctx = CaptureFrontendContext();
        mod::Log(
            "FRONTEND_RETURN_BEGIN owner=%s target=%s reason=%s screen=%u mode=%u ctx=0x%08lX +44=%u +45=%u accepted=0 pending=1",
            OwnerToString(request.owner),
            TargetToString(request.target),
            request.reason != nullptr ? request.reason : "unknown",
            static_cast<unsigned>(ctx.rawScreen),
            static_cast<unsigned>(ctx.gameModeRaw),
            static_cast<unsigned long>(ctx.screenContext),
            static_cast<unsigned>(ctx.lifecycle44),
            static_cast<unsigned>(ctx.exit45));
        return BuildResult(false, ctx.screen);
    }

    InterlockedExchange(&g_titleContinuationPending, 0);
    g_request.target = request.target;
    g_request.owner = request.owner;
    CopyReason(g_request.reason, sizeof(g_request.reason), request.reason);
    g_request.preserveHelper = request.preserveHelper;
    g_request.allowNativeBattleExit = request.allowNativeBattleExit;
    g_request.allowNativeResultExit = request.allowNativeResultExit;
    g_request.allowForcedFallback = request.allowForcedFallback;
    g_request.enterNetplayMenuAfterTitle = request.enterNetplayMenuAfterTitle;
    g_request.confirmedDisconnect = request.confirmedDisconnect;

    const DWORD now = GetTickCount();
    g_startTick = now;
    g_stateStartTick = now;
    g_nativeStartTick = 0;
    g_loadingStartTick = 0;
    g_screenStableStartTick = now;
    g_lastContextLogTick = 0;
    g_lastNativeProgressLogTick = 0;
    g_lastLoadingWaitLogTick = 0;
    g_lastLoadingTransientLogTick = 0;
    g_lastLoadingStuckLogTick = 0;
    g_lastFallbackSuppressedLogTick = 0;
    g_lastObservedRawScreen = 255;
    g_titleReachedLogged = false;
    g_menuContinuationLogged = false;
    g_loadingUnblockDone = false;
    g_nativeStartScreen = ScreenId::Unknown;
    g_nativeBattleUpdateReached = false;
    g_nativeBattlePatchRestoreOk = true;
    g_nativeCleanupFallbackSuppressed = false;
    InterlockedExchange(&g_netplayMenuContinuationConsumed, 0);

    EnsureFrontendReturnUpdateHooks();

    const FrontendContext ctx = CaptureFrontendContext();
    g_startScreen = ctx.screen;
    mod::Log(
        "FRONTEND_RETURN_BEGIN owner=%s target=%s reason=%s screen=%u mode=%u ctx=0x%08lX +44=%u +45=%u",
        OwnerToString(g_request.owner),
        TargetToString(g_request.target),
        g_request.reason,
        static_cast<unsigned>(ctx.rawScreen),
        static_cast<unsigned>(ctx.gameModeRaw),
        static_cast<unsigned long>(ctx.screenContext),
        static_cast<unsigned>(ctx.lifecycle44),
        static_cast<unsigned>(ctx.exit45));
    LogContext(ctx);
    if (ctx.screen == ScreenId::Unknown)
    {
        mod::Log(
            "FRONTEND_RETURN_FAILED reason=invalid_initial_screen raw=%u",
            static_cast<unsigned>(ctx.rawScreen));
    }

    SetState(ReturnState::Requested, "begin");
    TickFrontendReturn();
    return BuildResult(true, CaptureFrontendContext().screen);
}

void TickFrontendReturn()
{
    // Drive the async-hosting state machine from here too. This function is
    // invoked from every screen update hook (title, battle, result, loading),
    // so it is the per-frame chokepoint that lets async hosting detect a peer
    // connecting while the user is in gameplay/practice - contexts where the
    // full session_bridge::Tick() does not run. Cheap no-op while inactive.
    netplay::bridge::async_host::Tick();

    if (!HasPendingReturn())
    {
        return;
    }

    const DWORD now = GetTickCount();
    const FrontendContext ctx = CaptureFrontendContext();
    LogContextIfDue(ctx, now);

    switch (g_state)
    {
    case ReturnState::Idle:
        SetState(ReturnState::Requested, "tick_from_idle");
        ClassifyAndAdvance(ctx);
        return;
    case ReturnState::Requested:
    case ReturnState::WaitForSafePoint:
        if (g_state == ReturnState::WaitForSafePoint && ctx.insideFrameTick)
        {
            if (g_lastFallbackSuppressedLogTick == 0
                || now - g_lastFallbackSuppressedLogTick >= kProgressLogThrottleMs)
            {
                mod::Log(
                    "FRONTEND_RETURN_LOADING_FORCE_FALLBACK_SUPPRESSED reason=inside_frame_tick");
                g_lastFallbackSuppressedLogTick = now;
            }
            return;
        }
        if (g_state == ReturnState::WaitForSafePoint)
        {
            SetState(ReturnState::ForceTitleFallback, "safe_point_reached");
            TryForceTitleFallback("safe_point_reached");
            return;
        }
        ClassifyAndAdvance(ctx);
        return;
    case ReturnState::LoadingWait:
    case ReturnState::LoadingCriticalUnblock:
    case ReturnState::WaitingForLoadingToResolve:
        HandleLoading(ctx, now);
        return;
    case ReturnState::NativeExitRequested:
    case ReturnState::WaitingForNativeExit:
        HandleNativeWait(ctx, now);
        return;
    case ReturnState::ForceTitleFallback:
        TryForceTitleFallback("force_title_fallback");
        return;
    case ReturnState::TitleReached:
        HandleTitleReached("title_reached_tick");
        return;
    case ReturnState::NetplayMenuContinuationPending:
    case ReturnState::Complete:
    case ReturnState::Failed:
        return;
    }
}

bool HasPendingReturn()
{
    return InterlockedCompareExchange(&g_active, 0, 0) != 0
        || InterlockedCompareExchange(&g_titleContinuationPending, 0, 0) != 0;
}

bool IsReturningToFrontend()
{
    return HasPendingReturn();
}

bool ConsumeTitleContinuation(ReturnOwner* ownerOut, ReturnTarget* targetOut)
{
    if (InterlockedCompareExchange(&g_titleContinuationPending, 0, 0) == 0)
    {
        return false;
    }
    if (InterlockedCompareExchange(&g_titleContinuationPending, 0, 1) != 1)
    {
        return false;
    }

    if (ownerOut != nullptr)
    {
        *ownerOut = g_continuationOwner;
    }
    if (targetOut != nullptr)
    {
        *targetOut = g_continuationTarget;
    }

    if (g_continuationTarget == ReturnTarget::NetplayMenu)
    {
        InterlockedExchange(&g_netplayMenuContinuationConsumed, 1);
    }
    CompleteReturn("title_continuation_consumed");
    return true;
}

const char* CurrentStateName()
{
    return StateToString(g_state);
}

bool HasNetplayMenuContinuationPending()
{
    return InterlockedCompareExchange(&g_titleContinuationPending, 0, 0) != 0
        && g_continuationTarget == ReturnTarget::NetplayMenu;
}

bool HasConsumedNetplayMenuContinuation()
{
    return InterlockedCompareExchange(&g_netplayMenuContinuationConsumed, 0, 0) != 0;
}

extern "C" char __cdecl FrontendReturnBattleUpdateImpl(uint32_t screenContext)
{
    if (HasPendingReturn()
        && g_nativeStartScreen == ScreenId::Battle
        && (g_state == ReturnState::NativeExitRequested
            || g_state == ReturnState::WaitingForNativeExit))
    {
        g_nativeBattleUpdateReached = true;
    }

    // Per-frame battle-update diagnostics (EFZ_BATTLE_UPDATE_ENTER/EXIT,
    // BATTLE_PAUSE_STATE). Off by default - these fired every gameplay frame
    // and were a primary source of log bloat. The hook's actual return-flow
    // logic below always runs. Flip to true only when debugging frontend return.
    static constexpr bool kLogBattleUpdateDiag = false;
    if (kLogBattleUpdateDiag)
    {
        uint8_t pre44 = 255;
        uint8_t pre45 = 255;
        uint32_t pre1416 = 0xFFFFFFFFu;
        ReadBattleUpdateDiag(screenContext, &pre44, &pre45, &pre1416);
        const uint8_t rawScreen = ReadCurrentRawScreenForDiag();
        mod::Log(
            "EFZ_BATTLE_UPDATE_ENTER ctx=0x%08lX +44=%u +45=%u +1416=%lu",
            static_cast<unsigned long>(screenContext),
            static_cast<unsigned>(pre44),
            static_cast<unsigned>(pre45),
            static_cast<unsigned long>(pre1416));
        mod::Log(
            "BATTLE_PAUSE_STATE context=0x%08lX pause1416=%lu exit45=%u "
            "init44=%u screen=%u",
            static_cast<unsigned long>(screenContext),
            static_cast<unsigned long>(pre1416),
            static_cast<unsigned>(pre45),
            static_cast<unsigned>(pre44),
            static_cast<unsigned>(rawScreen));
    }

    char result = 3;
    if (g_originalBattleUpdate != nullptr)
    {
        result = g_originalBattleUpdate(screenContext);
    }

    // Draw the minimized async-host indicator on top of the rendered battle
    // frame (no-op unless async hosting is active and minimized).
    netplay::hooks::internal::DrawAsyncHostGameplayOverlay(screenContext);

    if (kLogBattleUpdateDiag)
    {
        uint8_t post44 = 255;
        uint8_t post45 = 255;
        uint32_t post1416 = 0xFFFFFFFFu;
        ReadBattleUpdateDiag(screenContext, &post44, &post45, &post1416);
        mod::Log(
            "EFZ_BATTLE_UPDATE_EXIT result=%d +44=%u +45=%u +1416=%lu",
            static_cast<int>(result),
            static_cast<unsigned>(post44),
            static_cast<unsigned>(post45),
            static_cast<unsigned long>(post1416));
    }

    result = HandleNativeUpdateReturn(ScreenId::Battle, result);
    TickFrontendReturn();
    return result;
}

extern "C" char __cdecl FrontendReturnResultUpdateImpl(uint32_t screenContext)
{
    char result = 5;
    if (g_originalResultUpdate != nullptr)
    {
        result = g_originalResultUpdate(screenContext);
    }
    result = HandleNativeUpdateReturn(ScreenId::Result, result);
    TickFrontendReturn();
    return result;
}

extern "C" char __cdecl FrontendReturnLoadingUpdateImpl(uint32_t screenContext)
{
    TickFrontendReturn();
    char result = 2;
    if (g_originalLoadingUpdate != nullptr)
    {
        result = g_originalLoadingUpdate(screenContext);
    }
    result = HandleNativeUpdateReturn(ScreenId::Loading, result);
    TickFrontendReturn();
    return result;
}

#if defined(_M_IX86)
extern "C" __declspec(naked) void FrontendReturnBattleUpdateThunk()
{
    __asm
    {
        push ecx
        call FrontendReturnBattleUpdateImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void FrontendReturnResultUpdateThunk()
{
    __asm
    {
        push ecx
        call FrontendReturnResultUpdateImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void FrontendReturnLoadingUpdateThunk()
{
    __asm
    {
        push ecx
        call FrontendReturnLoadingUpdateImpl
        add esp, 4
        ret
    }
}
#endif
} // namespace netplay::bridge::frontend_return
