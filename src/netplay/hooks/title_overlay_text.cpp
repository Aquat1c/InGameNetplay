#include "netplay/hooks/internal/shared.h"

#include <cstdio>

namespace netplay::hooks::internal
{
using NetplayMenuId = netplay::menu::NetplayMenuId;
using NetplayMenuSpec = netplay::menu::NetplayMenuSpec;
using NetplayMenuAction = netplay::menu::NetplayMenuAction;
using NetplayMenuEntry = netplay::menu::NetplayMenuEntry;
using netplay::menu::GetMenuSpec;

std::string BuildMenuHeaderText()
{
    const NetplayMenuSpec* spec = GetMenuSpec(g_netplayMenuState.menuId);
    if (spec == nullptr || spec->headerLabel == nullptr)
    {
        return "NETPLAY";
    }
    return spec->headerLabel;
}

std::string BuildRowLabel(const NetplayMenuEntry& entry)
{
    std::string value;
    switch (entry.action)
    {
    case NetplayMenuAction::OpenHost:
        return "HOST";
    case NetplayMenuAction::OpenJoin:
        return "JOIN";
    case NetplayMenuAction::OpenNickname:
        return "CHANGE NICKNAME";
    case NetplayMenuAction::LeaveNetplay:
        return "RETURN TO TITLE";
    case NetplayMenuAction::HostStart:
        return "START HOST";
    case NetplayMenuAction::HostEditPort:
        if (GetInlineEditDisplayValue(entry.action, &value, true))
        {
            return "PORT: " + value;
        }
        return "PORT";
    case NetplayMenuAction::BackToMain:
        return "BACK";
    case NetplayMenuAction::JoinConnect:
        return "CONNECT";
    case NetplayMenuAction::JoinEditAddress:
        if (GetInlineEditDisplayValue(entry.action, &value, true))
        {
            return "ADDRESS: " + value;
        }
        return "ADDRESS";
    case NetplayMenuAction::JoinEditPort:
        if (GetInlineEditDisplayValue(entry.action, &value, true))
        {
            return "PORT: " + value;
        }
        return "PORT";
    case NetplayMenuAction::NicknameEdit:
        if (GetInlineEditDisplayValue(entry.action, &value, true))
        {
            return "NAME: " + value;
        }
        return "NAME";
    default:
        return entry.debugLabel;
    }
}

std::string BuildFooterText()
{
    if (g_inlineEditState.active)
    {
        if (!g_inlineEditState.errorMessage.empty() && GetTickCount() < g_inlineEditState.errorExpireTick)
        {
            return g_inlineEditState.errorMessage + "  ENTER=SAVE ESC=CANCEL";
        }

        switch (g_inlineEditState.action)
        {
        case NetplayMenuAction::HostEditPort:
            return "EDIT HOST PORT  ENTER=SAVE ESC=CANCEL";
        case NetplayMenuAction::JoinEditAddress:
            return "EDIT JOIN ADDRESS  ENTER=SAVE ESC=CANCEL";
        case NetplayMenuAction::JoinEditPort:
            return "EDIT JOIN PORT  ENTER=SAVE ESC=CANCEL";
        case NetplayMenuAction::NicknameEdit:
            return "EDIT NICKNAME  ENTER=SAVE ESC=CANCEL";
        default:
            return "ENTER=SAVE ESC=CANCEL";
        }
    }

    char buffer[256] = {};
    snprintf(
        buffer,
        sizeof(buffer),
        "Nick: %s   Host:%u   Join:%s:%u",
        g_netplayMenuState.nickname.c_str(),
        static_cast<unsigned>(g_netplayMenuState.hostPort),
        g_netplayMenuState.joinAddress.c_str(),
        static_cast<unsigned>(g_netplayMenuState.joinPort));
    return buffer;
}

HFONT GetMenuOverlayFont()
{
    if (g_menuOverlayFont != nullptr)
    {
        return g_menuOverlayFont;
    }

    g_menuOverlayFont = CreateFontA(
        -14,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        SHIFTJIS_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        NONANTIALIASED_QUALITY,
        FIXED_PITCH | FF_MODERN,
        "MS Gothic");

    if (g_menuOverlayFont == nullptr)
    {
        g_menuOverlayFont = CreateFontA(
            -14,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            ANSI_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY,
            FIXED_PITCH | FF_MODERN,
            "Terminal");
    }

    if (g_menuOverlayFont == nullptr)
    {
        g_menuOverlayFont = reinterpret_cast<HFONT>(GetStockObject(SYSTEM_FIXED_FONT));
    }

    return g_menuOverlayFont;
}

const netplay::render::OverlayCallbacks& GetOverlayCallbacks()
{
    static const netplay::render::OverlayCallbacks callbacks = {
        [](int selection) -> int { return ClampSelectionToCurrentMenu(selection); },
        [](NetplayMenuId menuId, int* count) -> const NetplayMenuEntry* { return netplay::menu::GetMenuEntries(menuId, count); },
        []() -> std::string { return BuildMenuHeaderText(); },
        [](const NetplayMenuEntry& entry) -> std::string { return BuildRowLabel(entry); },
        []() -> std::string { return BuildFooterText(); },
        []() -> HFONT { return GetMenuOverlayFont(); },
        [](NetplayMenuAction action) -> bool { return IsInlineEditableAction(action); },
        [](NetplayMenuAction action, std::string* outValue, bool includeCaret) -> bool
        {
            return GetInlineEditDisplayValue(action, outValue, includeCaret);
        },
        [](uint32_t screenContext) -> int { return GetScaledNativeSlideY(screenContext); },
    };
    return callbacks;
}

bool DrawRuntimeTextOverlayGdi(uint32_t screenContext, bool allowWindowDc)
{
    const netplay::render::RuntimeOverlayState state = {
        g_useRuntimeTextOverlay,
        g_netplayMenuState.active,
        g_enableGdiFallbackOverlay,
        g_netplayMenuState.menuId,
    };
    return netplay::render::DrawRuntimeTextOverlayGdi(GetOverlayCallbacks(), state, screenContext, allowWindowDc);
}
} // namespace netplay::hooks::internal

