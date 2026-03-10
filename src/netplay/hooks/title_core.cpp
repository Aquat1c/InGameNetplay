#include "netplay/hooks/internal/shared.h"

#include "logger.h"
#include "netplay/core/player_rooms_menu.h"

#include <cstring>

namespace netplay::hooks::internal
{
using namespace netplay::constants;
using NetplayMenuId = netplay::menu::NetplayMenuId;
using NetplayMenuAction = netplay::menu::NetplayMenuAction;
using NetplayMenuEntry = netplay::menu::NetplayMenuEntry;
using netplay::menu::GetDefaultSelectionForMenu;
using netplay::menu::GetMenuEntries;
using netplay::menu::MenuIdToString;
using netplay::menu::RowIndexToString;
using InlineEditInputResult = netplay::inline_edit::InputResult;
using InlineEditValues = netplay::inline_edit::Values;

int GetCurrentMenuEntryCount()
{
    int count = 0;
    (void)GetMenuEntries(g_netplayMenuState.menuId, &count);
    return count;
}

int ClampSelectionToCurrentMenu(int selection)
{
    const int count = GetCurrentMenuEntryCount();
    if (count <= 0)
    {
        return 0;
    }
    if (selection < 0)
    {
        return 0;
    }
    if (selection >= count)
    {
        return count - 1;
    }
    return selection;
}

const NetplayMenuEntry* GetCurrentMenuEntry(int selection)
{
    int count = 0;
    const NetplayMenuEntry* entries = GetMenuEntries(g_netplayMenuState.menuId, &count);
    if (entries == nullptr || count <= 0)
    {
        return nullptr;
    }
    const int clamped = ClampSelectionToCurrentMenu(selection);
    return &entries[clamped];
}

int GetRenderRowForSelection(int selection)
{
    if (!g_netplayMenuState.useConfigStyleRender)
    {
        return ClampSelectionToCurrentMenu(selection);
    }

    const NetplayMenuEntry* entry = GetCurrentMenuEntry(selection);
    if (entry == nullptr)
    {
        return 0;
    }
    return entry->renderRow;
}

bool IsRenderRowUsedByMenu(NetplayMenuId menuId, int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= kNetplayConfigOptionCount)
    {
        return false;
    }

    int count = 0;
    const NetplayMenuEntry* entries = GetMenuEntries(menuId, &count);
    if (entries == nullptr || count <= 0)
    {
        return false;
    }

    for (int i = 0; i < count; ++i)
    {
        if (entries[i].renderRow == rowIndex)
        {
            return true;
        }
    }
    return false;
}

int GetScaledNativeSlideY(uint32_t screenContext)
{
    if (!g_menuSlideTransition.active)
    {
        return 0;
    }

    const int rawSlideY = *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY);
    if (rawSlideY < -8192 || rawSlideY > 8192)
    {
        static bool loggedInvalidSlideY = false;
        if (!loggedInvalidSlideY)
        {
            mod::Log(
                "GetScaledNativeSlideY: ignoring out-of-range raw value=%d at 0x%08X",
                rawSlideY,
                screenContext + kOffsetSlideAnimationY);
            loggedInvalidSlideY = true;
        }
        return 0;
    }

    int slideY = rawSlideY / kNetplayNativeSlideDivisor;
    if (slideY < -120)
    {
        slideY = -120;
    }
    if (slideY > 120)
    {
        slideY = 120;
    }
    return slideY;
}

void ResetMenuSlideTransition()
{
    g_menuSlideTransition = {};
}

bool IsMenuSlideTransitionActive()
{
    return g_menuSlideTransition.active;
}

void StartMenuSlideTransition(uint32_t screenContext, NetplayMenuId targetMenu, int targetSelection, int direction)
{
    if (g_menuSlideTransition.active)
    {
        return;
    }
    if (g_inlineEditState.active)
    {
        CancelInlineEdit();
    }

    const int currentSelection = ClampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
    const int resolvedTargetSelection = (targetSelection < 0) ? GetDefaultSelectionForMenu(targetMenu) : targetSelection;
    const bool bypassNativeSlide =
        (g_netplayMenuState.menuId == NetplayMenuId::BattleLog
            || targetMenu == NetplayMenuId::BattleLog);
    auto const performSlide = reinterpret_cast<int(__thiscall*)(void*, unsigned char)>(RuntimeAddress(kVaPerformSlideAnimation));

    if (bypassNativeSlide)
    {
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        *reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter) = 0;
        *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY) = 0;
        mod::Log(
            "NetplaySlide(native): bypass from=%s(%d) to=%s(%d)",
            MenuIdToString(g_netplayMenuState.menuId),
            currentSelection,
            MenuIdToString(targetMenu),
            resolvedTargetSelection);
        SwitchToMenu(screenContext, targetMenu, resolvedTargetSelection);
        return;
    }

    g_menuSlideTransition.active = true;
    g_menuSlideTransition.fromMenu = g_netplayMenuState.menuId;
    g_menuSlideTransition.toMenu = targetMenu;
    g_menuSlideTransition.fromSelection = currentSelection;
    g_menuSlideTransition.toSelection = resolvedTargetSelection;
    g_menuSlideTransition.direction = (direction >= 0) ? 1 : -1;
    g_menuSlideTransition.frame = 0;

    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
    *reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter) = 0;

    mod::Log(
        "NetplaySlide(native): start from=%s(%d) to=%s(%d) dir=%d",
        MenuIdToString(g_menuSlideTransition.fromMenu),
        g_menuSlideTransition.fromSelection,
        MenuIdToString(g_menuSlideTransition.toMenu),
        g_menuSlideTransition.toSelection,
        g_menuSlideTransition.direction);

    if (performSlide != nullptr)
    {
        *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY) = 0;
        (void)performSlide(reinterpret_cast<void*>(screenContext), 0);
    }
    SwitchToMenu(screenContext, targetMenu, resolvedTargetSelection);
    if (performSlide != nullptr)
    {
        (void)performSlide(reinterpret_cast<void*>(screenContext), 1u);
    }
    *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY) = 0;
    g_menuSlideTransition.active = false;
    mod::Log("NetplaySlide(native): complete menu=%s selection=%d", MenuIdToString(targetMenu), resolvedTargetSelection);
}

void AdvanceMenuSlideTransition(uint32_t screenContext)
{
    (void)screenContext;
}

InlineEditValues GetInlineEditValuesSnapshot()
{
    InlineEditValues values;
    values.hostPort = g_netplayMenuState.hostPort;
    values.joinAddress = g_netplayMenuState.joinAddress;
    values.joinPort = g_netplayMenuState.joinPort;
    values.nickname = g_netplayMenuState.nickname;
    values.playerRoomsRoomCode = netplay::player_rooms::GetRoomCode();
    return values;
}

void ApplyInlineEditValues(const InlineEditValues& values)
{
    g_netplayMenuState.hostPort = values.hostPort;
    g_netplayMenuState.joinAddress = values.joinAddress;
    g_netplayMenuState.joinPort = values.joinPort;
    g_netplayMenuState.nickname = values.nickname;
    netplay::player_rooms::SetRoomCode(values.playerRoomsRoomCode);
}

bool IsInlineEditableAction(NetplayMenuAction action)
{
    return netplay::inline_edit::IsInlineEditableAction(action);
}

bool GetInlineEditDisplayValue(NetplayMenuAction action, std::string* outValue, bool includeCaret)
{
    if (outValue == nullptr)
    {
        return false;
    }
    const InlineEditValues values = GetInlineEditValuesSnapshot();
    return netplay::inline_edit::GetDisplayValue(g_inlineEditState, values, action, outValue, includeCaret);
}

void BeginInlineEdit(NetplayMenuAction action)
{
    if (!IsInlineEditableAction(action))
    {
        return;
    }

    const InlineEditValues values = GetInlineEditValuesSnapshot();
    netplay::inline_edit::BeginEdit(&g_inlineEditState, action, values);
}

void ResetInlineEditState()
{
    netplay::inline_edit::ResetState(&g_inlineEditState);
}

void CancelInlineEdit()
{
    netplay::inline_edit::CancelEdit(&g_inlineEditState);
}

bool HandleInlineEditInput(uint32_t screenContext, const uint8_t* inputBytes)
{
    if (!g_inlineEditState.active || inputBytes == nullptr)
    {
        return false;
    }

    const HWND owner = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
    InlineEditValues values = GetInlineEditValuesSnapshot();
    const InlineEditInputResult result =
        netplay::inline_edit::HandleInput(&g_inlineEditState, &values, owner, inputBytes, &g_netplayEscapeDown);

    switch (result)
    {
    case InlineEditInputResult::Cancelled:
        PlayUiSound(screenContext, kSfxConfirm);
        return true;
    case InlineEditInputResult::CommitSuccess:
        ApplyInlineEditValues(values);
        PlayUiSound(screenContext, kSfxConfirm);
        return true;
    case InlineEditInputResult::CommitFailed:
        PlayUiSound(screenContext, kSfxMove);
        return true;
    case InlineEditInputResult::Consumed:
        return true;
    case InlineEditInputResult::NotEditing:
    default:
        return false;
    }
}

LRESULT CALLBACK NetplayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_netplayMenuState.active)
    {
        const bool isEsc = (wParam == static_cast<WPARAM>(VK_ESCAPE));
        const bool isEscMessage =
            message == WM_KEYDOWN
            || message == WM_KEYUP
            || message == WM_SYSKEYDOWN
            || message == WM_SYSKEYUP
            || message == WM_CHAR
            || message == WM_SYSCHAR;

        if (g_inlineEditState.active && isEsc && isEscMessage)
        {
            return 0;
        }
    }

    if (g_originalWindowProc != nullptr)
    {
        return CallWindowProcA(g_originalWindowProc, hwnd, message, wParam, lParam);
    }
    return DefWindowProcA(hwnd, message, wParam, lParam);
}

void InstallNetplayWindowHook(uint32_t screenContext)
{
    const HWND hwnd = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
    if (hwnd == nullptr || !IsWindow(hwnd))
    {
        return;
    }

    if (g_hookedWindow == hwnd && g_originalWindowProc != nullptr)
    {
        return;
    }

    RemoveNetplayWindowHook();
    LONG_PTR previousProc = SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&NetplayWindowProc));
    if (previousProc == 0)
    {
        const DWORD err = GetLastError();
        if (err != 0)
        {
            mod::Log("InstallNetplayWindowHook: SetWindowLongPtrA failed (err=%lu)", err);
            return;
        }
    }

    g_originalWindowProc = reinterpret_cast<WNDPROC>(previousProc);
    g_hookedWindow = hwnd;
    mod::Log("InstallNetplayWindowHook: hwnd=0x%p originalProc=0x%p", hwnd, reinterpret_cast<void*>(previousProc));
}

void RemoveNetplayWindowHook()
{
    if (g_hookedWindow == nullptr || g_originalWindowProc == nullptr)
    {
        g_hookedWindow = nullptr;
        g_originalWindowProc = nullptr;
        return;
    }

    if (IsWindow(g_hookedWindow))
    {
        LONG_PTR result = SetWindowLongPtrA(g_hookedWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWindowProc));
        if (result == 0)
        {
            const DWORD err = GetLastError();
            if (err != 0)
            {
                mod::Log("RemoveNetplayWindowHook: restore failed (err=%lu)", err);
            }
        }
    }

    mod::Log("RemoveNetplayWindowHook: restored");
    g_hookedWindow = nullptr;
    g_originalWindowProc = nullptr;
}

bool IsWindowFocused(HWND hwnd)
{
    if (hwnd == nullptr || !IsWindow(hwnd))
    {
        return false;
    }

    const HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || !IsWindow(foreground))
    {
        return false;
    }

    const HWND hwndRoot = GetAncestor(hwnd, GA_ROOTOWNER);
    const HWND foregroundRoot = GetAncestor(foreground, GA_ROOTOWNER);
    const HWND resolvedHwndRoot = hwndRoot != nullptr ? hwndRoot : hwnd;
    const HWND resolvedForegroundRoot = foregroundRoot != nullptr ? foregroundRoot : foreground;
    return resolvedHwndRoot == resolvedForegroundRoot;
}

bool IsScreenWindowFocused(uint32_t screenContext)
{
    const HWND hwnd = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
    return IsWindowFocused(hwnd);
}

bool ConsumeNetplayEscapeEdge()
{
    const bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool focused = IsWindowFocused(g_hookedWindow);
    const bool pressed = focused && down && !g_netplayEscapeDown;
    g_netplayEscapeDown = down;
    return pressed;
}

uintptr_t RuntimeAddress(uintptr_t va)
{
    return g_exeBase + (va - kEfzImageBase);
}
} // namespace netplay::hooks::internal
