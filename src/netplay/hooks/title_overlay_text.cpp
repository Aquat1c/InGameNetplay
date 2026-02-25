#include "netplay/hooks/internal/shared.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/render/draw_surface.h"
#include "logger.h"

#include <algorithm>
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
    case NetplayMenuAction::OpenLobby:
        return "LOBBY";
    case NetplayMenuAction::LobbyPlaying0:
    {
        // Show the playing pair at index 0 in a compact VS format.
        // This entry only exists in the menu when there is at least one pair.
        if (g_lobbySession)
        {
            const auto status = g_lobbySession->GetStatus();
            if (!status.playing.empty())
            {
                return status.playing[0].p1Name + " vs " + status.playing[0].p2Name;
            }
        }
        return "";
    }
    case NetplayMenuAction::LobbySlot0:
    case NetplayMenuAction::LobbySlot1:
    case NetplayMenuAction::LobbySlot2:
    case NetplayMenuAction::LobbySlot3:
    case NetplayMenuAction::LobbySlot4:
    case NetplayMenuAction::LobbySlot5:
    {
        // The visible slot index in the dynamic entry list.  Add the scroll
        // offset to get the real index into idlePlayers[].
        const int visSlot  = static_cast<int>(entry.action) - static_cast<int>(NetplayMenuAction::LobbySlot0);
        const int realSlot = visSlot + g_netplayMenuState.lobbyScrollOffset;
        if (g_lobbySession)
        {
            const auto status = g_lobbySession->GetStatus();
            if (realSlot < static_cast<int>(status.idlePlayers.size()))
            {
                // Show a scroll indicator on the first/last visible row so the
                // user knows there are more players above/below.
                const int idleCount = static_cast<int>(status.idlePlayers.size());
                const int visSlots  = std::min(idleCount, netplay::menu::kLobbyMaxDisplayPlayers);
                std::string name    = status.idlePlayers[realSlot].name;
                const bool canScrollUp   = (g_netplayMenuState.lobbyScrollOffset > 0);
                const bool canScrollDown = (g_netplayMenuState.lobbyScrollOffset < std::max(0, idleCount - visSlots));
                if (visSlot == 0 && canScrollUp)
                {
                    return "^ " + name;
                }
                if (visSlot == visSlots - 1 && canScrollDown)
                {
                    return name + " v";
                }
                return name;
            }
        }
        return "  --";
    }
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
    const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
    const auto bridgePhase = static_cast<netplay::bridge::NetbridgePhase>(bridgeStatus.phase);
    if (bridgePhase != netplay::bridge::NetbridgePhase::Idle)
    {
        netplay::bridge::BuildStatusLine(bridgeStatus, buffer, sizeof(buffer));
        return buffer;
    }

    if (g_netplayMenuState.menuId == NetplayMenuId::Lobby)
    {
        if (!g_lobbySession)
        {
            return "Not connected";
        }
        const auto status = g_lobbySession->GetStatus();
        switch (status.pollState)
        {
        case netplay::lobby::PollState::Joining:
            return "Connecting to lobby...";
        case netplay::lobby::PollState::Polling:
        {
            const DWORD elapsed = (GetTickCount() - status.lastPollTick) / 1000u;
            snprintf(buffer, sizeof(buffer),
                "%d idle  %d match  R=REFRESH  CONFIRM=Challenge  ESC=BACK",
                static_cast<int>(status.idlePlayers.size()),
                static_cast<int>(status.playing.size()));
            (void)elapsed; // used only for debug; keep footer short
            return buffer;
        }
        case netplay::lobby::PollState::Error:
            snprintf(buffer, sizeof(buffer), "Lobby error: %s", status.statusMessage.c_str());
            return buffer;
        default:
            return "Idle";
        }
    }
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
        // Playing-pair rows render in gold; idle slot rows render in default colors.
        [](const NetplayMenuEntry& entry, bool isSelected) -> std::optional<COLORREF>
        {
            if (entry.action == NetplayMenuAction::LobbyPlaying0)
            {
                // Selected: bright gold on dark highlight. Unselected: muted gold.
                return isSelected ? RGB(255, 215, 0) : RGB(160, 130, 0);
            }
            return std::nullopt;
        },
    };
    return callbacks;
}

bool DrawRuntimeTextOverlayGdi(uint32_t screenContext, bool allowWindowDc)
{
    // The lobby always forces GDI rendering regardless of whether the sprite
    // font is loaded or the global runtime-text / GDI-fallback flags are set.
    const bool lobbyForced = (g_netplayMenuState.menuId == netplay::menu::NetplayMenuId::Lobby);
    const netplay::render::RuntimeOverlayState state = {
        g_useRuntimeTextOverlay || lobbyForced, // lobby treated as always enabled
        g_netplayMenuState.active,
        g_enableGdiFallbackOverlay || lobbyForced,
        g_netplayMenuState.menuId,
    };
    return netplay::render::DrawRuntimeTextOverlayGdi(GetOverlayCallbacks(), state, screenContext, allowWindowDc);
}

bool DrawDelaySetupOverlayGdi(uint32_t screenContext, bool allowWindowDc)
{
    static bool s_loggedForCurrentOverlay = false;
    if (!g_netplayMenuState.active || !g_delaySetupOverlay.active)
    {
        s_loggedForCurrentOverlay = false;
        return false;
    }

    HDC dc = nullptr;
    void* surface = nullptr;
    HWND window = nullptr;
    if (!netplay::draw::AcquireMenuDrawDc(screenContext, &dc, &surface, &window, allowWindowDc))
    {
        return false;
    }

    const bool useWindowDc = (window != nullptr);
    if (!s_loggedForCurrentOverlay)
    {
        mod::Log(
            "DelayOverlay: draw path=%s allowWindowDc=%d",
            useWindowDc ? "window_dc" : "surface_dc",
            allowWindowDc ? 1 : 0);
        s_loggedForCurrentOverlay = true;
    }
    auto scaleX = [useWindowDc, window](int x)
    {
        if (!useWindowDc)
        {
            return x;
        }
        RECT clientRect = {};
        if (window == nullptr || GetClientRect(window, &clientRect) == FALSE)
        {
            return x;
        }
        const int clientW = clientRect.right - clientRect.left;
        return (clientW > 0) ? MulDiv(x, clientW, 320) : x;
    };
    auto scaleY = [useWindowDc, window](int y)
    {
        if (!useWindowDc)
        {
            return y;
        }
        RECT clientRect = {};
        if (window == nullptr || GetClientRect(window, &clientRect) == FALSE)
        {
            return y;
        }
        const int clientH = clientRect.bottom - clientRect.top;
        return (clientH > 0) ? MulDiv(y, clientH, 240) : y;
    };

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, GetMenuOverlayFont());

    RECT panelRect = {scaleX(82), scaleY(10), scaleX(318), scaleY(94)};
    HBRUSH panelBrush = CreateSolidBrush(RGB(8, 16, 28));
    FillRect(dc, &panelRect, panelBrush);
    DeleteObject(panelBrush);
    HBRUSH frameBrush = CreateSolidBrush(RGB(96, 210, 200));
    FrameRect(dc, &panelRect, frameBrush);
    DeleteObject(frameBrush);

    RECT textRect = {scaleX(88), scaleY(14), scaleX(314), scaleY(30)};
    SetTextColor(dc, RGB(220, 245, 245));
    DrawTextA(dc, "MATCH DELAY", -1, &textRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    char line[192] = {};
    RECT lineRect = {scaleX(88), scaleY(30), scaleX(314), scaleY(44)};
    if (g_delaySetupOverlay.pingMs >= 0)
    {
        std::snprintf(line, sizeof(line), "Ping: %d ms", g_delaySetupOverlay.pingMs);
    }
    else
    {
        std::snprintf(line, sizeof(line), "Ping: measuring...");
    }
    SetTextColor(dc, RGB(180, 208, 208));
    DrawTextA(dc, line, -1, &lineRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT lineRect2 = {scaleX(88), scaleY(44), scaleX(314), scaleY(58)};
    std::snprintf(
        line,
        sizeof(line),
        "Selected: %df  Recommended: %df",
        g_delaySetupOverlay.selectedDelay,
        g_delaySetupOverlay.recommendedDelay);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextA(dc, line, -1, &lineRect2, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT lineRect3 = {scaleX(88), scaleY(58), scaleX(314), scaleY(72)};
    std::snprintf(
        line,
        sizeof(line),
        "Allowed range: %d to %d",
        g_delaySetupOverlay.minDelay,
        g_delaySetupOverlay.maxDelay);
    SetTextColor(dc, RGB(176, 198, 198));
    DrawTextA(dc, line, -1, &lineRect3, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    RECT lineRect4 = {scaleX(88), scaleY(72), scaleX(314), scaleY(86)};
    if (g_delaySetupOverlay.waitingForRuntimeReady)
    {
        SetTextColor(dc, RGB(184, 208, 208));
        DrawTextA(dc, "Delay submitted. Waiting for game sync...", -1, &lineRect4, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    else if (g_delaySetupOverlay.errorMessage[0] != '\0')
    {
        SetTextColor(dc, RGB(255, 130, 130));
        DrawTextA(dc, g_delaySetupOverlay.errorMessage, -1, &lineRect4, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    else if (g_delaySetupOverlay.p1Name[0] != '\0' && g_delaySetupOverlay.p2Name[0] != '\0')
    {
        std::snprintf(line, sizeof(line), "%s vs %s", g_delaySetupOverlay.p1Name, g_delaySetupOverlay.p2Name);
        SetTextColor(dc, RGB(184, 208, 208));
        DrawTextA(dc, line, -1, &lineRect4, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    else
    {
        SetTextColor(dc, RGB(184, 208, 208));
        DrawTextA(dc, "LEFT/RIGHT adjust  CONFIRM accept  BACK cancel", -1, &lineRect4, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    if (oldFont != nullptr)
    {
        SelectObject(dc, oldFont);
    }
    netplay::draw::ReleaseMenuDrawDc(dc, surface, window);
    return true;
}
} // namespace netplay::hooks::internal
