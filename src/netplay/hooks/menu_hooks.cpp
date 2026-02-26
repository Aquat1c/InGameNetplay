#include "netplay/hooks/menu_hooks.h"
#include "netplay/hooks/internal/shared.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/bridge/takeover_internal.h"

#include "logger.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace netplay::hooks::internal
{
std::mutex g_patchMutex;
std::atomic<bool> g_hooksInstalled{false};
uintptr_t g_exeBase = 0;
std::vector<netplay::patch::PatchRecord> g_appliedPatches;
uint32_t g_customDispatchTable[8] = {};
uint32_t g_replayCaseDispatchAddress = 0;
extern "C" uint32_t g_titleCaseReturnAddress = 0;
std::string g_moduleDirectory;

NetplayMenuState g_netplayMenuState;
MenuSlideTransition g_menuSlideTransition;
netplay::inline_edit::State g_inlineEditState;
DWORD g_lastNetplayFrameLogTick = 0;
uint64_t g_titleUpdateCallCount = 0;
uint64_t g_netplayUpdateCallCount = 0;
int8_t g_lastLoggedSelection = -1;
bool g_hasLoggedInputSnapshot = false;
netplay::fontmap::SpriteFont g_spriteFont = {};
bool g_useRuntimeTextOverlay = true;
bool g_enableGdiFallbackOverlay = false;
HFONT g_menuOverlayFont = nullptr;
HWND g_hookedWindow = nullptr;
WNDPROC g_originalWindowProc = nullptr;
bool g_netplayEscapeDown = false;
bool g_restoreReplaySelectionOnNextTitleUpdate = false;
uint32_t g_replaySelectionGuardFramesRemaining = 0;
int8_t g_replaySelectionRestoreTarget = -1;
bool g_pendingVsHumanAutoConfirm = false;
DWORD g_pendingVsHumanAutoConfirmTick = 0;
DWORD g_pendingVsHumanAutoConfirmLastLogTick = 0;
bool g_returnToNetplayAfterMatch = false;
bool g_charSelectEntryHoldArmed = false;
InputSnapshot g_lastInputSnapshot = {};
DelaySetupOverlayState g_delaySetupOverlay = {};
SpectateConfirmOverlayState g_spectateConfirmOverlay = {};
DebugOverlayState g_debugOverlay = {};
std::unique_ptr<netplay::lobby::LobbySession> g_lobbySession;
bool g_titleConfirmDown = false;
// Number of frames to keep calling ClearRevivalText after an ExitProcess
// interception.  Ensures stale tournament text is removed even if transient
// state (e.g. init(2,102) resetting dword_100A0778) re-adds it briefly.
static int g_postExitTextClearFrames = 0;
static bool g_charSelectEntryHoldActive = false;
static int g_charSelectEntryHoldFramesRemaining = 0;
static uint32_t g_charSelectUpdateSlotAddress = 0;
static TitleUpdateFn g_originalCharSelectUpdate = nullptr;
static constexpr int kCharSelectEntryHoldFrames = 8;

extern "C" char __cdecl HookedCharSelectUpdateImpl(uint32_t screenContext);
extern "C" void HookedCharSelectUpdateThunk();

bool EnsureCharSelectEntryHoldHook()
{
    if (g_charSelectUpdateSlotAddress != 0)
    {
        return g_originalCharSelectUpdate != nullptr;
    }

    constexpr uintptr_t kVaScreenObjectTable = 0x00790110;
    const uintptr_t tableAddress = RuntimeAddress(kVaScreenObjectTable);
    if (tableAddress == 0)
    {
        return false;
    }

    uint32_t charSelectObject = 0;
    uint32_t charSelectVtable = 0;
    __try
    {
        auto* const screenTable = reinterpret_cast<uint32_t*>(tableAddress);
        charSelectObject = screenTable[1];
        if (charSelectObject == 0)
        {
            return false;
        }
        charSelectVtable = *reinterpret_cast<uint32_t*>(charSelectObject);
        if (charSelectVtable == 0)
        {
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    auto* const updateSlot = reinterpret_cast<uint32_t*>(charSelectVtable + 4);
    uint32_t originalUpdateAddress = 0;
    __try
    {
        originalUpdateAddress = *updateSlot;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    const uint32_t hookAddress = reinterpret_cast<uint32_t>(&HookedCharSelectUpdateThunk);
    if (originalUpdateAddress == hookAddress)
    {
        g_charSelectUpdateSlotAddress = reinterpret_cast<uint32_t>(updateSlot);
        return g_originalCharSelectUpdate != nullptr;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(updateSlot, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        return false;
    }

    *updateSlot = hookAddress;
    DWORD ignored = 0;
    (void)VirtualProtect(updateSlot, sizeof(uint32_t), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), updateSlot, sizeof(uint32_t));

    g_originalCharSelectUpdate = reinterpret_cast<TitleUpdateFn>(originalUpdateAddress);
    g_charSelectUpdateSlotAddress = reinterpret_cast<uint32_t>(updateSlot);
    mod::Log(
        "InstallHooks: charselect update hook installed slot=0x%08X original=0x%08X",
        g_charSelectUpdateSlotAddress,
        originalUpdateAddress);
    return true;
}

void ArmCharSelectEntryHold()
{
    if (!EnsureCharSelectEntryHoldHook())
    {
        mod::Log("CharSelectHold: failed to arm (charselect hook unavailable)");
        return;
    }

    g_charSelectEntryHoldArmed = true;
    g_charSelectEntryHoldActive = false;
    g_charSelectEntryHoldFramesRemaining = 0;
    mod::Log("CharSelectHold: armed frames=%d", kCharSelectEntryHoldFrames);
}

void ObserveOfflineSelectionConfirm(uint32_t screenContext)
{
    const int gameSystem = GetGameSystem(screenContext);
    const auto* const inputBytes = reinterpret_cast<const uint8_t*>(gameSystem);
    const bool confirmDown = (inputBytes[16] == 1) || (inputBytes[17] == 1);
    const bool confirmEdge = confirmDown && !g_titleConfirmDown;
    g_titleConfirmDown = confirmDown;

    if (!confirmEdge)
    {
        return;
    }

    const auto* const selectionPtr = reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection);
    const int selection = static_cast<int>(*selectionPtr);
    mod::Log("HookedTitleUpdateImpl: title confirm edge selection=%d", selection);
    netplay::bridge::OnTitleSelectionConfirmed(selection);
}

// ---------------------------------------------------------------------------
// HookedTitleUpdateImplBody — the real title-screen update logic.
// Called from HookedTitleUpdateImpl which wraps it in setjmp/longjmp
// protection so NeutralizeExitProcess can safely escape.
// ---------------------------------------------------------------------------
static char HookedTitleUpdateImplBody(uint32_t screenContext)
{
    ++g_titleUpdateCallCount;
    if ((g_titleUpdateCallCount % 300ull) == 0ull)
    {
        mod::Log(
            "HookedTitleUpdateImpl: calls=%llu netplayActive=%d",
            static_cast<unsigned long long>(g_titleUpdateCallCount),
            g_netplayMenuState.active);
    }

    // Post-exit text clearing: keep issuing ClearRevivalText for a few
    // frames after an ExitProcess interception to guarantee stale
    // tournament text (nicknames, win counts) is removed even if
    // init(2,102) or session tick re-adds it transiently.
    if (g_postExitTextClearFrames > 0)
    {
        --g_postExitTextClearFrames;
        netplay::bridge::takeover::ClearRevivalText();
    }

    if (!g_netplayMenuState.active)
    {
        // When the tournament match ends and the game returns to mode 0 (title
        // screen), ExitProcess is blocked by Jcc patches so NeutralizeExitProcess
        // and ConsumeRevivalExitInterception never fire on the normal path.
        // Detect this proactively: if we're still flagged as tournament but the
        // game mode is 0, clean up immediately and schedule text clearing.
        if (netplay::bridge::NotifyTitleScreenActive())
        {
            g_postExitTextClearFrames = 5;
            mod::Log("HookedTitleUpdateImpl: tournament return to title detected, clearing text");
        }

        // Check for Revival DLL ExitProcess interception BEFORE ticking.
        // If ExitProcess was intercepted, the session vtable has been
        // neutralized and we need to clean up and route the user to the
        // appropriate screen:  netplay menu for online modes, title screen
        // for tournament / offline modes.
        int exitMode = -1;
        if (netplay::bridge::ConsumeRevivalExitInterception(&exitMode))
        {
            g_returnToNetplayAfterMatch = false;
            g_pendingVsHumanAutoConfirm = false;
            g_pendingVsHumanAutoConfirmTick = 0;
            g_pendingVsHumanAutoConfirmLastLogTick = 0;

            if (exitMode == 0 || exitMode == 1)
            {
                // Netplay (host / join) — return to the netplay menu.
                mod::Log(
                    "HookedTitleUpdateImpl: exit intercepted (netplay mode=%d), re-entering netplay menu",
                    exitMode);
                EnterNetplayMenu(screenContext);
                return 0;
            }

            // Tournament or other — stay on the normal title screen.
            // Schedule a few frames of ClearRevivalText to ensure stale
            // tournament text overlays are fully cleared.
            g_postExitTextClearFrames = 5;
            mod::Log(
                "HookedTitleUpdateImpl: exit intercepted (mode=%d), staying on title screen",
                exitMode);
            return GetOriginalTitleUpdate()(screenContext);
        }

        netplay::bridge::Tick();

        // After a netplay-initiated VS Human match ends, the game returns to
        // the title screen (state 0).  Re-enter the netplay menu automatically
        // instead of showing the normal title screen.
        if (g_returnToNetplayAfterMatch)
        {
            g_returnToNetplayAfterMatch = false;
            mod::Log("HookedTitleUpdateImpl: post-match return, re-entering netplay menu");
            EnterNetplayMenu(screenContext);
            return 0;
        }

        if (g_pendingVsHumanAutoConfirm)
        {
            if (g_pendingVsHumanAutoConfirmTick == 0)
            {
                g_pendingVsHumanAutoConfirmTick = GetTickCount();
                g_pendingVsHumanAutoConfirmLastLogTick = 0;
            }

            const auto bridgeStatus = netplay::bridge::GetStatus();
            const auto bridgePhase = static_cast<netplay::bridge::NetbridgePhase>(bridgeStatus.phase);
            if (bridgePhase != netplay::bridge::NetbridgePhase::Connected
                && bridgePhase != netplay::bridge::NetbridgePhase::DelaySetup)
            {
                mod::Log(
                    "HookedTitleUpdateImpl: canceled pending VS Human auto-confirm phase=%s",
                    netplay::bridge::PhaseToString(bridgePhase));
                g_pendingVsHumanAutoConfirm = false;
                g_pendingVsHumanAutoConfirmTick = 0;
                g_pendingVsHumanAutoConfirmLastLogTick = 0;
            }
            else
            {
                const bool syncReady =
                    bridgeStatus.syncMode0Flag1084 == 4 &&
                    (bridgeStatus.syncSessionByte == 0 ||
                     bridgeStatus.syncSessionByte == 1 ||
                     bridgeStatus.syncSessionByte == 2);

                const DWORD nowTick = GetTickCount();
                const DWORD elapsed = nowTick - g_pendingVsHumanAutoConfirmTick;
                if (syncReady)
                {
                    (void)netplay::bridge::PrepareVsHumanHandoff();
                    const int gameSystem = GetGameSystem(screenContext);
                    auto* const inputBytes = reinterpret_cast<uint8_t*>(gameSystem);
                    inputBytes[12] = 0;
                    inputBytes[13] = 0;
                    inputBytes[14] = 0;
                    inputBytes[15] = 0;
                    inputBytes[16] = 1;
                    inputBytes[17] = 0;
                    inputBytes[18] = 0;
                    inputBytes[19] = 0;
                    mod::Log(
                        "HookedTitleUpdateImpl: injecting one-shot VS Human confirm syncReady=%d elapsed=%lums sync(mode=%d flag1084=%d session=%d)",
                        syncReady ? 1 : 0,
                        static_cast<unsigned long>(elapsed),
                        bridgeStatus.syncGameMode,
                        bridgeStatus.syncMode0Flag1084,
                        bridgeStatus.syncSessionByte);
                    g_pendingVsHumanAutoConfirm = false;
                    g_pendingVsHumanAutoConfirmTick = 0;
                    g_pendingVsHumanAutoConfirmLastLogTick = 0;
                }
                else
                {
                    if (g_pendingVsHumanAutoConfirmLastLogTick == 0 ||
                        nowTick - g_pendingVsHumanAutoConfirmLastLogTick >= 1000)
                    {
                        mod::Log(
                            "HookedTitleUpdateImpl: waiting VS Human sync elapsed=%lums sync(mode=%d flag1084=%d session=%d flags=%d/%d)",
                            static_cast<unsigned long>(elapsed),
                            bridgeStatus.syncGameMode,
                            bridgeStatus.syncMode0Flag1084,
                            bridgeStatus.syncSessionByte,
                            bridgeStatus.syncGlobalFlag4964,
                            bridgeStatus.syncGlobalFlag4965);
                        g_pendingVsHumanAutoConfirmLastLogTick = nowTick;
                    }
                }
            }
        }

        const char result = GetOriginalTitleUpdate()(screenContext);

        ObserveOfflineSelectionConfirm(screenContext);

        if (g_restoreReplaySelectionOnNextTitleUpdate)
        {
            auto* const selectionPtr = reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection);
            if (g_replaySelectionRestoreTarget >= 0 && *selectionPtr == 4)
            {
                *selectionPtr = g_replaySelectionRestoreTarget;
                mod::Log(
                    "HookedTitleUpdateImpl: guarded replay title selection 4 -> %d (guardFrames=%u)",
                    static_cast<int>(g_replaySelectionRestoreTarget),
                    static_cast<unsigned>(g_replaySelectionGuardFramesRemaining));
            }

            if (g_replaySelectionGuardFramesRemaining > 0)
            {
                --g_replaySelectionGuardFramesRemaining;
                if (g_replaySelectionGuardFramesRemaining == 0)
                {
                    g_restoreReplaySelectionOnNextTitleUpdate = false;
                    g_replaySelectionRestoreTarget = -1;
                    mod::Log("HookedTitleUpdateImpl: replay selection guard window ended");
                }
            }
        }
        return result;
    }

    g_titleConfirmDown = false;
    return UpdateNetplayMenu(screenContext);
}

extern "C" char __cdecl HookedCharSelectUpdateImpl(uint32_t screenContext)
{
    if (g_originalCharSelectUpdate == nullptr)
    {
        return 1;
    }

    if (g_charSelectEntryHoldArmed && !g_charSelectEntryHoldActive)
    {
        g_charSelectEntryHoldArmed = false;
        g_charSelectEntryHoldActive = true;
        g_charSelectEntryHoldFramesRemaining = kCharSelectEntryHoldFrames;
        mod::Log("CharSelectHold: started on first charselect frame budget=%d", g_charSelectEntryHoldFramesRemaining);
    }

    if (!g_charSelectEntryHoldActive)
    {
        return g_originalCharSelectUpdate(screenContext);
    }

    netplay::bridge::Tick();
    const auto bridgeStatus = netplay::bridge::GetStatus();
    const bool syncReady = bridgeStatus.vsHumanSyncReady != 0;
    if (syncReady)
    {
        g_charSelectEntryHoldActive = false;
        g_charSelectEntryHoldFramesRemaining = 0;
        mod::Log(
            "CharSelectHold: released early sync(mode=%d flag1084=%d session=%d)",
            bridgeStatus.syncGameMode,
            bridgeStatus.syncMode0Flag1084,
            bridgeStatus.syncSessionByte);
        return g_originalCharSelectUpdate(screenContext);
    }

    --g_charSelectEntryHoldFramesRemaining;
    if (g_charSelectEntryHoldFramesRemaining <= 0)
    {
        g_charSelectEntryHoldActive = false;
        g_charSelectEntryHoldFramesRemaining = 0;
        mod::Log(
            "CharSelectHold: timeout release sync(mode=%d flag1084=%d session=%d)",
            bridgeStatus.syncGameMode,
            bridgeStatus.syncMode0Flag1084,
            bridgeStatus.syncSessionByte);
        return g_originalCharSelectUpdate(screenContext);
    }

    return 1;
}

// ---------------------------------------------------------------------------
// HookedTitleUpdateImpl — dispatches to HookedTitleUpdateImplBody.
//
// ExitProcess interception no longer uses longjmp.  The DLL call-site
// patches (SaveAndApplyDllExitProcessPatches) make ExitProcess unreachable
// during tournament mode.  The IAT hook is a safety fallback only.
// ---------------------------------------------------------------------------
extern "C" char __cdecl HookedTitleUpdateImpl(uint32_t screenContext)
{
    return HookedTitleUpdateImplBody(screenContext);
}

extern "C" BOOL __cdecl HookedTitleRenderImpl(uint32_t screenContext)
{
    if (g_netplayMenuState.active && g_useRuntimeTextOverlay)
    {
        return RenderNetplayMenuRuntimeText(screenContext);
    }
    if (g_netplayMenuState.active && g_netplayMenuState.useConfigStyleRender)
    {
        return RenderNetplayMenuConfigStyle(screenContext);
    }
    return GetOriginalTitleRender()(screenContext);
}

#if defined(_M_IX86)
extern "C" void __cdecl NetplayCaseImpl(uint32_t screenContext)
{
    mod::Log("NetplayCaseImpl: invoked");
    TriggerNetplayMenuEntry(screenContext);
}

extern "C" __declspec(naked) void NetplayCaseThunk()
{
    __asm
    {
        mov eax, dword ptr [ebp-8]
        push eax
        call NetplayCaseImpl
        add esp, 4
        mov al, 0
        mov edx, dword ptr [g_titleCaseReturnAddress]
        jmp edx
    }
}

extern "C" void __cdecl ReplayCaseCompatImpl(uint32_t screenContext)
{
    auto* const selectionPtr =
        reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection);
    const int oldSelection = static_cast<int>(*selectionPtr);
    if (oldSelection != 4)
    {
        // Revival checks mode0+1084 == 4 to recognize replay context.
        g_replaySelectionRestoreTarget = static_cast<int8_t>(oldSelection);
        *selectionPtr = 4;
        g_restoreReplaySelectionOnNextTitleUpdate = true;
        g_replaySelectionGuardFramesRemaining = 300;
        mod::Log(
            "ReplayCaseCompatImpl: remapped title selection %d -> 4 for replay compatibility (restoreTarget=%d guardFrames=%u)",
            oldSelection,
            static_cast<int>(g_replaySelectionRestoreTarget),
            static_cast<unsigned>(g_replaySelectionGuardFramesRemaining));
    }
}

extern "C" __declspec(naked) void ReplayCaseCompatThunk()
{
    __asm
    {
        mov eax, dword ptr [ebp-8]
        push eax
        call ReplayCaseCompatImpl
        add esp, 4
        mov edx, dword ptr [g_replayCaseDispatchAddress]
        jmp edx
    }
}

extern "C" __declspec(naked) void HookedTitleUpdateThunk()
{
    __asm
    {
        push ecx
        call HookedTitleUpdateImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void HookedTitleRenderThunk()
{
    __asm
    {
        push ecx
        call HookedTitleRenderImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void HookedCharSelectUpdateThunk()
{
    __asm
    {
        push ecx
        call HookedCharSelectUpdateImpl
        add esp, 4
        ret
    }
}
#endif
} // namespace netplay::hooks::internal

namespace netplay
{
bool AreHooksInstalled()
{
    return hooks::internal::g_hooksInstalled.load();
}

void ShowInProgressMessage(HWND owner)
{
    mod::Log("ShowInProgressMessage: owner=0x%p", owner);
    MessageBoxA(owner, "In progress", "Netplay", MB_OK | MB_ICONINFORMATION);
}
} // namespace netplay
