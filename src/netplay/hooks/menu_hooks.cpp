#include "netplay/hooks/menu_hooks.h"
#include "netplay/hooks/internal/shared.h"

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
InputSnapshot g_lastInputSnapshot = {};
std::unique_ptr<netplay::lobby::LobbySession> g_lobbySession;

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
        const char result = GetOriginalTitleUpdate()(screenContext);

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
