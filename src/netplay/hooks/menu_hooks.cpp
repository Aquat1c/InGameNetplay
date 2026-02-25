#include "netplay/hooks/menu_hooks.h"
#include "netplay/hooks/internal/shared.h"
#include "netplay/bridge/session_bridge.h"

#include "logger.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

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
InputSnapshot g_lastInputSnapshot = {};
DelaySetupOverlayState g_delaySetupOverlay = {};
std::unique_ptr<netplay::lobby::LobbySession> g_lobbySession;
bool g_titleConfirmDown = false;

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

extern "C" char __cdecl HookedTitleUpdateImpl(uint32_t screenContext)
{
    ++g_titleUpdateCallCount;
    if ((g_titleUpdateCallCount % 300ull) == 0ull)
    {
        mod::Log(
            "HookedTitleUpdateImpl: calls=%llu netplayActive=%d",
            static_cast<unsigned long long>(g_titleUpdateCallCount),
            g_netplayMenuState.active);
    }

    if (!g_netplayMenuState.active)
    {
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
