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
InputSnapshot g_lastInputSnapshot = {};

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
        return GetOriginalTitleUpdate()(screenContext);
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

