#include "netplay/hooks/internal/shared.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/render/draw_surface.h"
#include "netplay/render/software_font.h"
#include "logger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

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
        // offset to get the real index into displayEntries[].
        const int visSlot  = static_cast<int>(entry.action) - static_cast<int>(NetplayMenuAction::LobbySlot0);
        const int realSlot = visSlot + g_netplayMenuState.lobbyScrollOffset;
        if (g_lobbySession)
        {
            const auto status = g_lobbySession->GetStatus();
            if (realSlot < static_cast<int>(status.displayEntries.size()))
            {
                const auto& de = status.displayEntries[realSlot];
                // Show a scroll indicator on the first/last visible row so the
                // user knows there are more entries above/below.
                const int displayCount = static_cast<int>(status.displayEntries.size());
                const int visSlots  = std::min(displayCount, netplay::menu::kLobbyMaxDisplayPlayers);
                // Prefix names with status indicators:
                //   [!]  = incoming challenge
                //   [O]  = player is in a match
                //   [YOU] = our own entry
                // All prefixes are padded to 6 chars so names align.
                std::string name;
                if (de.isSelf)
                {
                    name = "[YOU] " + de.name;
                }
                else if (de.isChallenge)
                {
                    name = " [!]  " + de.name;
                }
                else if (de.isPlaying)
                {
                    name = " [O]  " + de.name;
                }
                else
                {
                    name = "      " + de.name;
                }
                const bool canScrollUp   = (g_netplayMenuState.lobbyScrollOffset > 0);
                const bool canScrollDown = (g_netplayMenuState.lobbyScrollOffset < std::max(0, displayCount - visSlots));
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
            const int challengeCount = static_cast<int>(status.challenges.size());
            const int idleCount = static_cast<int>(status.idlePlayers.size());
            const int matchCount = static_cast<int>(status.playing.size());
            if (challengeCount > 0)
            {
                snprintf(buffer, sizeof(buffer),
                    "%d idle  %d challenge  %d match  R=REFRESH  ESC=BACK",
                    idleCount, challengeCount, matchCount);
            }
            else
            {
                snprintf(buffer, sizeof(buffer),
                    "%d idle  %d match  R=REFRESH  CONFIRM=Challenge  ESC=BACK",
                    idleCount, matchCount);
            }
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

bool DrawDelaySetupOverlayGdi(uint32_t screenContext, bool /*allowWindowDc*/)
{
    static bool s_loggedForCurrentOverlay = false;
    if (!g_netplayMenuState.active || !g_delaySetupOverlay.active)
    {
        s_loggedForCurrentOverlay = false;
        return false;
    }

    netplay::draw::LockedMenuSurface lockedSurface;
    if (!netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        return false;
    }

    if (!s_loggedForCurrentOverlay)
    {
        mod::Log(
            "DelayOverlay: pixel draw surface=%dx%d pitch=%d",
            lockedSurface.width,
            lockedSurface.height,
            lockedSurface.pitch);
        s_loggedForCurrentOverlay = true;
    }

    const netplay::font::IndexedSurfaceView sv = {
        lockedSurface.pixels,
        lockedSurface.width,
        lockedSurface.height,
        lockedSurface.pitch,
    };

    // Resolve palette colors
    const uint8_t bgColor = netplay::draw::ResolveBestPaletteColor(screenContext, 8, 16, 28);
    const uint8_t frameColor = netplay::draw::ResolveBestPaletteColor(screenContext, 96, 210, 200);
    const uint8_t titleColor = netplay::draw::ResolveBestPaletteColor(screenContext, 220, 245, 245);
    const uint8_t textColor = netplay::draw::ResolveBestPaletteColor(screenContext, 180, 208, 208);
    const uint8_t brightColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 255, 255);
    const uint8_t dimColor = netplay::draw::ResolveBestPaletteColor(screenContext, 176, 198, 198);
    const uint8_t errorColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 130, 130);

    // Panel background and frame — centered on 320x240 surface
    constexpr int delayPanelW = 236;
    constexpr int delayPanelH = 84;
    constexpr int delayPanelX = (320 - delayPanelW) / 2;
    constexpr int delayPanelY = (240 - delayPanelH) / 2;
    netplay::font::FillIndexedSurfaceRect(sv, delayPanelX, delayPanelY, delayPanelW, delayPanelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, delayPanelX, delayPanelY, delayPanelW, delayPanelH, frameColor);

    const int dTextL = delayPanelX + 6;
    const int dTextR = delayPanelX + delayPanelW - 4;

    // Title: "MATCH DELAY"
    netplay::font::DrawTextLeft5x7(sv, "MATCH DELAY", dTextL, dTextR, delayPanelY + 4, 1, 1, titleColor);

    // Ping line
    char line[192] = {};
    if (g_delaySetupOverlay.pingMs >= 0)
    {
        std::snprintf(line, sizeof(line), "Ping: %d ms", g_delaySetupOverlay.pingMs);
    }
    else
    {
        std::snprintf(line, sizeof(line), "Ping: measuring...");
    }
    netplay::font::DrawTextLeft5x7(sv, line, dTextL, dTextR, delayPanelY + 20, 1, 1, textColor);

    // Selected / recommended
    std::snprintf(
        line,
        sizeof(line),
        "Selected: %df  Recommended: %df",
        g_delaySetupOverlay.selectedDelay,
        g_delaySetupOverlay.recommendedDelay);
    netplay::font::DrawTextLeft5x7(sv, line, dTextL, dTextR, delayPanelY + 34, 1, 1, brightColor);

    // Allowed range
    std::snprintf(
        line,
        sizeof(line),
        "Allowed range: %d to %d",
        g_delaySetupOverlay.minDelay,
        g_delaySetupOverlay.maxDelay);
    netplay::font::DrawTextLeft5x7(sv, line, dTextL, dTextR, delayPanelY + 48, 1, 1, dimColor);

    // Status / error / names / help
    if (g_delaySetupOverlay.waitingForRuntimeReady)
    {
        netplay::font::DrawTextLeft5x7(sv, "Delay submitted. Waiting...", dTextL, dTextR, delayPanelY + 62, 1, 1, textColor);
    }
    else if (g_delaySetupOverlay.errorMessage[0] != '\0')
    {
        netplay::font::DrawTextLeft5x7(sv, g_delaySetupOverlay.errorMessage, dTextL, dTextR, delayPanelY + 62, 1, 1, errorColor);
    }
    else if (g_delaySetupOverlay.p1Name[0] != '\0' && g_delaySetupOverlay.p2Name[0] != '\0')
    {
        std::snprintf(line, sizeof(line), "%s vs %s", g_delaySetupOverlay.p1Name, g_delaySetupOverlay.p2Name);
        netplay::font::DrawTextLeft5x7(sv, line, dTextL, dTextR, delayPanelY + 62, 1, 1, textColor);
    }
    else
    {
        netplay::font::DrawTextLeft5x7(sv, "L/R adjust CONFIRM accept BACK cancel", dTextL, dTextR, delayPanelY + 62, 1, 1, textColor);
    }

    // Second status line
    if (g_delaySetupOverlay.waitingForRuntimeReady)
    {
        netplay::font::DrawTextLeft5x7(sv, "Waiting for game sync...", dTextL, dTextR, delayPanelY + 72, 1, 1, dimColor);
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

bool DrawHostingOverlayGdi(uint32_t screenContext, bool /*allowWindowDc*/)
{
    if (!g_netplayMenuState.active || !g_hostingOverlay.active)
    {
        return false;
    }

    netplay::draw::LockedMenuSurface lockedSurface;
    if (!netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        return false;
    }

    const netplay::font::IndexedSurfaceView sv = {
        lockedSurface.pixels,
        lockedSurface.width,
        lockedSurface.height,
        lockedSurface.pitch,
    };

    const uint8_t bgColor     = netplay::draw::ResolveBestPaletteColor(screenContext, 8, 16, 28);
    const uint8_t frameColor  = netplay::draw::ResolveBestPaletteColor(screenContext, 96, 210, 200);
    const uint8_t titleColor  = netplay::draw::ResolveBestPaletteColor(screenContext, 220, 245, 245);
    const uint8_t textColor   = netplay::draw::ResolveBestPaletteColor(screenContext, 180, 208, 208);
    const uint8_t brightColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 255, 255);
    const uint8_t dimColor    = netplay::draw::ResolveBestPaletteColor(screenContext, 176, 198, 198);
    const uint8_t greenColor  = netplay::draw::ResolveBestPaletteColor(screenContext, 100, 255, 130);

    constexpr int panelW = 260;
    constexpr int panelH = 64;
    constexpr int panelX = (320 - panelW) / 2;
    constexpr int panelY = (240 - panelH) / 2;
    netplay::font::FillIndexedSurfaceRect(sv, panelX, panelY, panelW, panelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, panelX, panelY, panelW, panelH, frameColor);

    const int tL = panelX + 6;
    const int tR = panelX + panelW - 4;

    netplay::font::DrawTextLeft5x7(sv, "HOSTING", tL, tR, panelY + 4, 1, 1, titleColor);

    char line[192] = {};
    if (!g_hostingOverlay.ipFetchDone)
    {
        std::snprintf(line, sizeof(line), "Fetching public IP...");
        netplay::font::DrawTextLeft5x7(sv, line, tL, tR, panelY + 18, 1, 1, dimColor);
    }
    else if (g_hostingOverlay.ipFetchFailed)
    {
        std::snprintf(line, sizeof(line), "Port: %u  (IP detection failed)",
            static_cast<unsigned>(g_hostingOverlay.port));
        netplay::font::DrawTextLeft5x7(sv, line, tL, tR, panelY + 18, 1, 1, textColor);
    }
    else
    {
        std::snprintf(line, sizeof(line), "%s:%u",
            g_hostingOverlay.publicIp, static_cast<unsigned>(g_hostingOverlay.port));
        netplay::font::DrawTextLeft5x7(sv, line, tL, tR, panelY + 18, 1, 1, brightColor);
    }

    // Copied feedback or help text
    const bool showCopiedFlash = g_hostingOverlay.copiedToClipboard
        && (GetTickCount() - g_hostingOverlay.copiedFlashTick) < 2000;
    if (showCopiedFlash)
    {
        netplay::font::DrawTextLeft5x7(sv, "Copied to clipboard!", tL, tR, panelY + 34, 1, 1, greenColor);
    }
    else if (g_hostingOverlay.ipFetchDone && !g_hostingOverlay.ipFetchFailed)
    {
        netplay::font::DrawTextLeft5x7(sv, "Press C button to copy the address", tL, tR, panelY + 34, 1, 1, dimColor);
    }

    netplay::font::DrawTextLeft5x7(sv, "Waiting for opponent...  ESC/BACK=Cancel", tL, tR, panelY + 50, 1, 1, textColor);

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

bool DrawJoiningOverlayGdi(uint32_t screenContext, bool /*allowWindowDc*/)
{
    if (!g_netplayMenuState.active || !g_joiningOverlay.active)
    {
        return false;
    }

    netplay::draw::LockedMenuSurface lockedSurface;
    if (!netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        return false;
    }

    const netplay::font::IndexedSurfaceView sv = {
        lockedSurface.pixels,
        lockedSurface.width,
        lockedSurface.height,
        lockedSurface.pitch,
    };

    const uint8_t bgColor     = netplay::draw::ResolveBestPaletteColor(screenContext, 8, 16, 28);
    const uint8_t frameColor  = netplay::draw::ResolveBestPaletteColor(screenContext, 96, 210, 200);
    const uint8_t titleColor  = netplay::draw::ResolveBestPaletteColor(screenContext, 220, 245, 245);
    const uint8_t textColor   = netplay::draw::ResolveBestPaletteColor(screenContext, 180, 208, 208);
    const uint8_t dimColor    = netplay::draw::ResolveBestPaletteColor(screenContext, 176, 198, 198);
    const uint8_t redColor    = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 80, 80);

    constexpr int panelW = 260;
    constexpr int panelH = 64;
    constexpr int panelX = (320 - panelW) / 2;
    constexpr int panelY = (240 - panelH) / 2;
    netplay::font::FillIndexedSurfaceRect(sv, panelX, panelY, panelW, panelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, panelX, panelY, panelW, panelH, frameColor);

    const int tL = panelX + 6;
    const int tR = panelX + panelW - 4;

    netplay::font::DrawTextLeft5x7(sv, "JOINING", tL, tR, panelY + 4, 1, 1, titleColor);

    char line[192] = {};
    if (g_joiningOverlay.failed)
    {
        // Error state
        netplay::font::DrawTextLeft5x7(sv, "Connection failed:", tL, tR, panelY + 18, 1, 1, redColor);
        if (g_joiningOverlay.errorText[0] != '\0')
        {
            netplay::font::DrawTextLeft5x7(sv, g_joiningOverlay.errorText, tL, tR, panelY + 30, 1, 1, textColor);
        }
        netplay::font::DrawTextLeft5x7(sv, "Press any button to dismiss", tL, tR, panelY + 50, 1, 1, dimColor);
    }
    else
    {
        // Connecting state
        std::snprintf(line, sizeof(line), "Connecting to %s:%u ...",
            g_joiningOverlay.address, static_cast<unsigned>(g_joiningOverlay.port));
        netplay::font::DrawTextLeft5x7(sv, line, tL, tR, panelY + 22, 1, 1, textColor);

        netplay::font::DrawTextLeft5x7(sv, "ESC/BACK=Cancel", tL, tR, panelY + 50, 1, 1, dimColor);
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

bool DrawSpectateConfirmOverlayGdi(uint32_t screenContext, bool /*allowWindowDc*/)
{
    static bool s_loggedForCurrentOverlay = false;
    if (!g_netplayMenuState.active || !g_spectateConfirmOverlay.active)
    {
        s_loggedForCurrentOverlay = false;
        return false;
    }

    netplay::draw::LockedMenuSurface lockedSurface;
    if (!netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        return false;
    }

    if (!s_loggedForCurrentOverlay)
    {
        mod::Log(
            "SpectateConfirmOverlay: pixel draw surface=%dx%d pitch=%d",
            lockedSurface.width,
            lockedSurface.height,
            lockedSurface.pitch);
        s_loggedForCurrentOverlay = true;
    }

    const netplay::font::IndexedSurfaceView sv = {
        lockedSurface.pixels,
        lockedSurface.width,
        lockedSurface.height,
        lockedSurface.pitch,
    };

    // Resolve palette colors
    const uint8_t bgColor = netplay::draw::ResolveBestPaletteColor(screenContext, 8, 16, 28);
    const uint8_t frameColor = netplay::draw::ResolveBestPaletteColor(screenContext, 96, 210, 200);
    const uint8_t titleColor = netplay::draw::ResolveBestPaletteColor(screenContext, 220, 245, 245);
    const uint8_t textColor = netplay::draw::ResolveBestPaletteColor(screenContext, 180, 208, 208);
    const uint8_t brightColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 255, 255);
    const uint8_t dimColor = netplay::draw::ResolveBestPaletteColor(screenContext, 160, 180, 180);
    const uint8_t hlColor = netplay::draw::ResolveBestPaletteColor(screenContext, 40, 100, 96);
    const uint8_t errorColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 130, 130);

    // Panel background and frame — centered on 320x240 surface
    constexpr int specPanelW = 200;
    constexpr int specPanelH = 100;
    constexpr int specPanelX = (320 - specPanelW) / 2;
    constexpr int specPanelY = (240 - specPanelH) / 2;
    netplay::font::FillIndexedSurfaceRect(sv, specPanelX, specPanelY, specPanelW, specPanelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, specPanelX, specPanelY, specPanelW, specPanelH, frameColor);

    const int sTextL = specPanelX + 6;
    const int sTextR = specPanelX + specPanelW - 6;

    // Title: "SPECTATE?"
    netplay::font::DrawTextCentered5x7(sv, "SPECTATE?", sTextL, sTextR, specPanelY + 6, 1, 1, titleColor);

    // Question lines
    netplay::font::DrawTextCentered5x7(sv, "Host is already in a match.", sTextL, sTextR, specPanelY + 24, 1, 1, textColor);
    netplay::font::DrawTextCentered5x7(sv, "Join as a spectator?", sTextL, sTextR, specPanelY + 40, 1, 1, textColor);

    // Option: Yes (index 0)
    const bool yesSelected = (g_spectateConfirmOverlay.selectedOption == 0);
    if (yesSelected)
    {
        netplay::font::FillIndexedSurfaceRect(sv, specPanelX + 40, specPanelY + 58, 120, 12, hlColor);
    }
    netplay::font::DrawTextCentered5x7(
        sv, yesSelected ? "> Yes" : "  Yes", specPanelX + 40, specPanelX + specPanelW - 40, specPanelY + 60, 1, 1,
        yesSelected ? brightColor : dimColor);

    // Option: No (index 1)
    const bool noSelected = (g_spectateConfirmOverlay.selectedOption == 1);
    if (noSelected)
    {
        netplay::font::FillIndexedSurfaceRect(sv, specPanelX + 40, specPanelY + 74, 120, 12, hlColor);
    }
    netplay::font::DrawTextCentered5x7(
        sv, noSelected ? "> No" : "  No", specPanelX + 40, specPanelX + specPanelW - 40, specPanelY + 76, 1, 1,
        noSelected ? brightColor : dimColor);

    // Error message if any
    if (g_spectateConfirmOverlay.errorMessage[0] != '\0')
    {
        netplay::font::DrawTextCentered5x7(
            sv, g_spectateConfirmOverlay.errorMessage, sTextL, sTextR, specPanelY + 90, 1, 1, errorColor);
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

// ---------------------------------------------------------------------------
// Debug overlay — toggled with keyboard D key
// ---------------------------------------------------------------------------

static constexpr int kDebugMenuItemCount = 4;
static constexpr const char* kDebugMenuItems[kDebugMenuItemCount] = {
    "Delay Overlay",
    "Spectate Overlay",
    "Runtime Text Overlay",
    "Close",
};

bool DrawDebugOverlay(uint32_t screenContext)
{
    if (!g_debugOverlay.open)
    {
        return false;
    }

    netplay::draw::LockedMenuSurface lockedSurface;
    if (!netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        return false;
    }

    const netplay::font::IndexedSurfaceView sv = {
        lockedSurface.pixels,
        lockedSurface.width,
        lockedSurface.height,
        lockedSurface.pitch,
    };

    // Resolve palette colors
    const uint8_t bgColor = netplay::draw::ResolveBestPaletteColor(screenContext, 10, 10, 30);
    const uint8_t frameColor = netplay::draw::ResolveBestPaletteColor(screenContext, 140, 140, 220);
    const uint8_t titleColor = netplay::draw::ResolveBestPaletteColor(screenContext, 220, 220, 255);
    const uint8_t normalColor = netplay::draw::ResolveBestPaletteColor(screenContext, 180, 180, 180);
    const uint8_t selectedColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 255, 255);
    const uint8_t hlColor = netplay::draw::ResolveBestPaletteColor(screenContext, 50, 50, 120);
    const uint8_t onColor = netplay::draw::ResolveBestPaletteColor(screenContext, 100, 255, 100);
    const uint8_t offColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 100, 100);

    // Panel: right side, similar to ImprovedReplayMenu's "D MENU"
    constexpr int panelLeft = 168;
    constexpr int panelRight = 312;
    constexpr int panelTop = 60;
    constexpr int panelW = panelRight - panelLeft;
    constexpr int itemHeight = 12;
    constexpr int itemGap = 4;
    constexpr int headerHeight = 16;
    constexpr int panelH = headerHeight + kDebugMenuItemCount * (itemHeight + itemGap) + 4;

    netplay::font::FillIndexedSurfaceRect(sv, panelLeft, panelTop, panelW, panelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, panelLeft, panelTop, panelW, panelH, frameColor);

    // Title
    netplay::font::DrawTextCentered5x7(sv, "D MENU", panelLeft + 2, panelRight - 2, panelTop + 4, 1, 1, titleColor);

    // Items
    const bool toggleStates[kDebugMenuItemCount] = {
        g_debugOverlay.forceDelayOverlay,
        g_debugOverlay.forceSpectateOverlay,
        g_debugOverlay.forceRuntimeTextOverlay,
        false, // "Close" has no toggle state
    };

    for (int i = 0; i < kDebugMenuItemCount; ++i)
    {
        const int itemY = panelTop + headerHeight + i * (itemHeight + itemGap);
        const bool isSelected = (i == g_debugOverlay.selectedIndex);

        if (isSelected)
        {
            netplay::font::FillIndexedSurfaceRect(sv, panelLeft + 2, itemY, panelW - 4, itemHeight, hlColor);
        }

        // Build label: "> Item [ON]" or "  Item [OFF]"
        char label[64] = {};
        if (i < kDebugMenuItemCount - 1)
        {
            std::snprintf(label, sizeof(label), "%s%s [%s]",
                isSelected ? "> " : "  ",
                kDebugMenuItems[i],
                toggleStates[i] ? "ON" : "OFF");
        }
        else
        {
            std::snprintf(label, sizeof(label), "%s%s",
                isSelected ? "> " : "  ",
                kDebugMenuItems[i]);
        }

        uint8_t textColor = isSelected ? selectedColor : normalColor;
        if (i < kDebugMenuItemCount - 1 && toggleStates[i])
        {
            textColor = isSelected ? onColor : onColor;
        }
        else if (i < kDebugMenuItemCount - 1 && !toggleStates[i])
        {
            textColor = isSelected ? offColor : normalColor;
        }

        const int textY = itemY + (itemHeight - 7) / 2;
        netplay::font::DrawTextLeft5x7(sv, label, panelLeft + 4, panelRight - 4, textY, 1, 1, textColor);
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

bool HandleDebugOverlayInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter)
{
    if (inputBytes == nullptr || inactivityCounter == nullptr)
    {
        return false;
    }

    // Game Button D toggle (offset 22/23 = P1/P2, already edge-detected by processPlayerInput).
    for (int pi = 0; pi < 2; ++pi)
    {
        if (inputBytes[pi + 22] == 1)
        {
            g_debugOverlay.open = !g_debugOverlay.open;
            g_debugOverlay.selectedIndex = 0;
            if (g_debugOverlay.open)
            {
                mod::Log("DebugOverlay: opened");
            }
            else
            {
                mod::Log("DebugOverlay: closed");
            }
            break;
        }
    }

    if (!g_debugOverlay.open)
    {
        return false;
    }

    // Navigate with game controller (same pattern as spectate confirm)
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch = reinterpret_cast<uint8_t*>(screenContext + netplay::constants::kOffsetInputLatchP1 + playerIndex);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);

        int step = 0;
        if (vertical > 0) step = 1;
        else if (vertical < 0) step = -1;

        if (step != 0)
        {
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                int newIndex = g_debugOverlay.selectedIndex + step;
                if (newIndex < 0) newIndex = kDebugMenuItemCount - 1;
                if (newIndex >= kDebugMenuItemCount) newIndex = 0;
                if (newIndex != g_debugOverlay.selectedIndex)
                {
                    g_debugOverlay.selectedIndex = newIndex;
                    PlayUiSound(screenContext, netplay::constants::kSfxMove);
                }
                *inputLatch = 1;
            }
        }
        else
        {
            *inputLatch = 0;
        }

        // Confirm
        if (inputBytes[playerIndex + 16] == 1)
        {
            *inactivityCounter = 0;
            const int sel = g_debugOverlay.selectedIndex;
            if (sel == 0)
            {
                g_debugOverlay.forceDelayOverlay = !g_debugOverlay.forceDelayOverlay;
                if (g_debugOverlay.forceDelayOverlay)
                {
                    // Populate with test data
                    g_delaySetupOverlay.active = true;
                    g_delaySetupOverlay.selectedDelay = 3;
                    g_delaySetupOverlay.recommendedDelay = 4;
                    g_delaySetupOverlay.minDelay = 1;
                    g_delaySetupOverlay.maxDelay = 10;
                    g_delaySetupOverlay.pingMs = 42;
                    std::snprintf(g_delaySetupOverlay.p1Name, sizeof(g_delaySetupOverlay.p1Name), "Player1");
                    std::snprintf(g_delaySetupOverlay.p2Name, sizeof(g_delaySetupOverlay.p2Name), "Player2");
                    g_delaySetupOverlay.errorMessage[0] = '\0';
                    g_delaySetupOverlay.waitingForRuntimeReady = false;
                    mod::Log("DebugOverlay: Delay overlay ON (test data)");
                }
                else
                {
                    g_delaySetupOverlay.active = false;
                    mod::Log("DebugOverlay: Delay overlay OFF");
                }
            }
            else if (sel == 1)
            {
                g_debugOverlay.forceSpectateOverlay = !g_debugOverlay.forceSpectateOverlay;
                if (g_debugOverlay.forceSpectateOverlay)
                {
                    g_spectateConfirmOverlay.active = true;
                    g_spectateConfirmOverlay.selectedOption = 0;
                    g_spectateConfirmOverlay.errorMessage[0] = '\0';
                    mod::Log("DebugOverlay: Spectate overlay ON");
                }
                else
                {
                    g_spectateConfirmOverlay.active = false;
                    mod::Log("DebugOverlay: Spectate overlay OFF");
                }
            }
            else if (sel == 2)
            {
                g_debugOverlay.forceRuntimeTextOverlay = !g_debugOverlay.forceRuntimeTextOverlay;
                g_enableGdiFallbackOverlay = g_debugOverlay.forceRuntimeTextOverlay;
                mod::Log("DebugOverlay: Runtime text overlay %s",
                    g_debugOverlay.forceRuntimeTextOverlay ? "ON" : "OFF");
            }
            else if (sel == 3)
            {
                g_debugOverlay.open = false;
                mod::Log("DebugOverlay: closed via menu");
            }
            PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
        }

        // Cancel closes
        if (inputBytes[playerIndex + 18] == 1)
        {
            g_debugOverlay.open = false;
            PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
            mod::Log("DebugOverlay: closed via cancel");
            *inactivityCounter = 0;
        }
    }

    return true;
}

} // namespace netplay::hooks::internal
