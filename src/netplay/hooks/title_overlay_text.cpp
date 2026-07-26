#include "netplay/hooks/internal/shared.h"
#include "netplay/bridge/async_hosting.h"
#include "netplay/hooks/debug_overlay.h"
#include "netplay/bridge/gameplay_exit_recovery.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/core/battle_log_menu.h"
#include "netplay/core/mod_settings.h"
#include "netplay/core/options_menu.h"
#include "netplay/core/player_rooms_menu.h"
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
constexpr char kLobbyPlayingEyeGlyph = '\x7F';

std::string BuildMenuHeaderText()
{
    if (g_netplayMenuState.menuId == NetplayMenuId::Lobby && g_lobbySession)
    {
        if (g_lobbySession->GetOrigin() == netplay::lobby::RoomOrigin::PlayerRooms)
        {
            if (!g_lobbySession->GetRoomAlias().empty())
            {
                return g_lobbySession->GetRoomAlias();
            }
            return "PLAYER ROOM";
        }
        return "LOBBY";
    }

    const NetplayMenuSpec* spec = GetMenuSpec(g_netplayMenuState.menuId);
    if (spec == nullptr || spec->headerLabel == nullptr)
    {
        return "NETPLAY";
    }
    return spec->headerLabel;
}

std::string BuildRowLabel(const NetplayMenuEntry& entry)
{
    if (g_netplayMenuState.menuId == NetplayMenuId::BattleLog)
    {
        return netplay::battle_log::BuildRowLabel(entry.action);
    }

    std::string value;
    switch (entry.action)
    {
    case NetplayMenuAction::OpenHost:
        return "HOST";
    case NetplayMenuAction::OpenJoin:
        return "JOIN";
    case NetplayMenuAction::OpenPlayerRooms:
        return "PLAYER ROOMS";
    case NetplayMenuAction::OpenLobby:
        return "LOBBY";
    case NetplayMenuAction::OpenBattleLog:
        return "BATTLE LOG";
    case NetplayMenuAction::OpenOptions:
        return "OPTIONS";
    case NetplayMenuAction::PlayerRoomsRefresh:
    case NetplayMenuAction::PlayerRoomsJoin:
    case NetplayMenuAction::PlayerRoomsEditCode:
    case NetplayMenuAction::PlayerRoomsCreate:
    case NetplayMenuAction::PlayerRoomsRoomType:
    case NetplayMenuAction::PlayerRoomsSlot0:
    case NetplayMenuAction::PlayerRoomsSlot1:
    case NetplayMenuAction::PlayerRoomsSlot2:
        return netplay::player_rooms::BuildRowLabel(entry.action);
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
    case NetplayMenuAction::OptionRow0:
    case NetplayMenuAction::OptionRow1:
    case NetplayMenuAction::OptionRow2:
    case NetplayMenuAction::OptionRow3:
    case NetplayMenuAction::OptionRow4:
    case NetplayMenuAction::OptionRow5:
    case NetplayMenuAction::OptionRow6:
    case NetplayMenuAction::OptionRow7:
        return netplay::options::BuildRowLabel(entry.action);
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
                //   [eye] = player is in a match
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
                    name = std::string(" [") + kLobbyPlayingEyeGlyph + "]  " + de.name;
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

std::string BuildRowPrimaryText(const NetplayMenuEntry& entry)
{
    if (g_netplayMenuState.menuId == NetplayMenuId::BattleLog)
    {
        return netplay::battle_log::BuildRowPrimaryText(entry.action);
    }
    if (g_netplayMenuState.menuId == NetplayMenuId::PlayerRooms)
    {
        return netplay::player_rooms::BuildRowPrimaryText(entry.action);
    }
    if (g_netplayMenuState.menuId == NetplayMenuId::Options)
    {
        return netplay::options::BuildRowPrimaryText(entry.action);
    }
    return BuildRowLabel(entry);
}

std::string BuildRowSecondaryText(const NetplayMenuEntry& entry)
{
    if (g_netplayMenuState.menuId == NetplayMenuId::BattleLog)
    {
        return netplay::battle_log::BuildRowSecondaryText(entry.action);
    }
    if (g_netplayMenuState.menuId == NetplayMenuId::PlayerRooms)
    {
        return netplay::player_rooms::BuildRowSecondaryText(entry.action);
    }
    if (g_netplayMenuState.menuId == NetplayMenuId::Options)
    {
        return netplay::options::BuildRowSecondaryText(entry.action);
    }
    return {};
}

namespace
{
std::string AppendPlayerRoomCodeFooter(std::string footer)
{
    if (g_netplayMenuState.menuId != NetplayMenuId::Lobby || !g_lobbySession)
    {
        return footer;
    }
    if (g_lobbySession->GetOrigin() != netplay::lobby::RoomOrigin::PlayerRooms)
    {
        return footer;
    }
    if (g_lobbySession->GetRoomCode().empty())
    {
        return footer;
    }

    const std::string codeLine = "Room Code: " + g_lobbySession->GetRoomCode();
    if (footer.empty())
    {
        return codeLine;
    }
    return footer + "\n" + codeLine;
}

std::string BuildActionTooltip(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::OpenHost:
        return "Host a direct match.";
    case NetplayMenuAction::OpenJoin:
        return "Configure a direct host connection.\nPress C to paste IP:PORT. Press D to Spectate IP.";
    case NetplayMenuAction::OpenPlayerRooms:
        return "Browse public and private player rooms.";
    case NetplayMenuAction::OpenLobby:
        return "Enter the global lobby player list.";
    case NetplayMenuAction::OpenBattleLog:
        return "Browse and search BattleLog.txt.";
    case NetplayMenuAction::OpenOptions:
        return "Adjust netplay settings.\nSaved in EfzRevival.ini.";
    case NetplayMenuAction::PlayerRoomsRefresh:
        return "Refresh the public room list.";
    case NetplayMenuAction::PlayerRoomsJoin:
        return "Join the room code shown below.";
    case NetplayMenuAction::PlayerRoomsEditCode:
        return "Set the room code or alias to join.";
    case NetplayMenuAction::PlayerRoomsCreate:
        return "Create a room using the selected visibility.";
    case NetplayMenuAction::PlayerRoomsRoomType:
        return "Choose whether new rooms are Private or Public.";
    case NetplayMenuAction::PlayerRoomsSlot0:
    case NetplayMenuAction::PlayerRoomsSlot1:
    case NetplayMenuAction::PlayerRoomsSlot2:
        return "Join the highlighted public room.";
    case NetplayMenuAction::LeaveNetplay:
        return "Return to the title screen.";
    case NetplayMenuAction::HostStart:
        return "Begin hosting on the selected port.";
    case NetplayMenuAction::HostEditPort:
        return "Set the port other players will use.";
    case NetplayMenuAction::JoinConnect:
        return "Connect to the host using the settings below.\nPress C to paste IP:PORT. Press D to Spectate IP.";
    case NetplayMenuAction::JoinEditAddress:
        return "Set the host address.\nPress C to paste IP:PORT. Press D to Spectate IP.";
    case NetplayMenuAction::JoinEditPort:
        return "Set the host port.\nPress C to paste IP:PORT. Press D to Spectate IP.";
    case NetplayMenuAction::NicknameEdit:
        return "Update the nickname shown in lobbies and online matches.";
    case NetplayMenuAction::BackToMain:
        return "Return to the netplay main menu.";
    case NetplayMenuAction::LobbyPlaying0:
        return "Spectate the highlighted live match.";
    case NetplayMenuAction::LobbySlot0:
    case NetplayMenuAction::LobbySlot1:
    case NetplayMenuAction::LobbySlot2:
    case NetplayMenuAction::LobbySlot3:
    case NetplayMenuAction::LobbySlot4:
    case NetplayMenuAction::LobbySlot5:
        return "Inspect this lobby entry.";
    default:
        return {};
    }
}

bool IsLobbySlotAction(NetplayMenuAction action)
{
    return action >= NetplayMenuAction::LobbySlot0
        && action <= NetplayMenuAction::LobbySlot5;
}

std::string BuildLobbyRowTooltip(const NetplayMenuEntry& entry)
{
    if (entry.action == NetplayMenuAction::LobbyPlaying0)
    {
        return "Spectate the highlighted live match.";
    }

    if (!IsLobbySlotAction(entry.action))
    {
        return {};
    }

    if (!g_lobbySession)
    {
        return "Refreshing lobby status.";
    }

    const auto status = g_lobbySession->GetStatus();
    const int visSlot =
        static_cast<int>(entry.action) - static_cast<int>(NetplayMenuAction::LobbySlot0);
    const int realSlot = visSlot + g_netplayMenuState.lobbyScrollOffset;
    if (realSlot < 0 || realSlot >= static_cast<int>(status.displayEntries.size()))
    {
        return "Refreshing lobby status.";
    }

    const auto& displayEntry = status.displayEntries[realSlot];
    if (displayEntry.isSelf)
    {
        return "This is your current lobby entry.";
    }
    if (displayEntry.isChallenge)
    {
        return "Accept this player's challenge.";
    }
    if (displayEntry.isPlaying)
    {
        return "Spectate this player's live match.";
    }
    return "Challenge this idle player to a match.";
}

std::string BuildEntryTooltip(const NetplayMenuEntry& entry)
{
    if (const std::string dynamicTooltip = BuildLobbyRowTooltip(entry);
        !dynamicTooltip.empty())
    {
        return dynamicTooltip;
    }
    return BuildActionTooltip(entry.action);
}

// Submits one footer line to the game-RT TTF layer.  The submit happens even
// while the layer is not live yet - a committed item is what makes the
// EndScene hook initialise ImGui in the first place (same bootstrap the
// battle log shims rely on).  Returns true only when the TTF layer will
// actually draw this frame, i.e. the 5x7 fallback should be suppressed.
// Drop the last full UTF-8 code point (continuation bytes plus lead byte).
void Utf8PopBack(std::string* text)
{
    while (!text->empty()
        && (static_cast<unsigned char>(text->back()) & 0xC0u) == 0x80u)
    {
        text->pop_back();
    }
    if (!text->empty())
    {
        text->pop_back();
    }
}

bool SubmitFooterRtLine(const std::string& line, int left, int right, int y)
{
    namespace ov = netplay::debug_overlay;
    if (!netplay::mod_settings::IsMenuTtfTextEnabled())
    {
        return false;
    }
    bool hasNonAscii = false;
    for (unsigned char c : line)
    {
        if ((c & 0x80u) != 0)
        {
            hasNonAscii = true;
            break;
        }
    }
    // Non-ASCII needs the Cyrillic ranges + merged Japanese font; without
    // them the TTF would render '?' boxes, so keep 5x7 for those lines.
    if (hasNonAscii && !ov::RtTextHasExtendedGlyphs())
    {
        return false;
    }

    const int availableWidth = right - left;
    std::string window = line;
    // Width is unmeasurable until the fonts are loaded (-1); submit the whole
    // line in that case - the marquee sizing corrects on the next frame.
    const int textWidth = ov::MeasureRtTextWidth(ov::RtTextProfile::Footer, line.c_str());
    if (textWidth > availableWidth && !line.empty())
    {
        if (!hasNonAscii)
        {
            // Same marquee as the 5x7 path, sized with proportional metrics
            // (average glyph width) instead of the fixed 6px cell.
            constexpr DWORD kScrollStepMs = 180;
            constexpr size_t kPadChars = 6;
            const size_t visibleChars = (std::max)(
                static_cast<size_t>(1),
                line.size() * static_cast<size_t>(availableWidth)
                    / static_cast<size_t>(textWidth));
            const std::string spacer(kPadChars, ' ');
            const std::string marquee = line + spacer + line + spacer;
            const size_t cycle = line.size() + spacer.size();
            const size_t start = (GetTickCount() / kScrollStepMs) % cycle;
            window = marquee.substr(start, (std::min)(visibleChars + 2, marquee.size() - start));
        }
        // The RT layer has no right-edge clipping (unlike the indexed
        // surface), so trim until the text actually fits the field.  For
        // multibyte text the sliding marquee is skipped (byte-offset slicing
        // would split code points); it is trimmed to fit instead.
        while (!window.empty()
            && ov::MeasureRtTextWidth(ov::RtTextProfile::Footer, window.c_str()) > availableWidth)
        {
            Utf8PopBack(&window);
        }
    }

    ov::RtTextItem item;
    item.x0 = static_cast<int16_t>(left);
    item.x1 = static_cast<int16_t>(right);
    item.y = static_cast<int16_t>(y);
    item.align = ov::RtTextAlign::Left;
    item.profile = ov::RtTextProfile::Footer;
    item.rgba = 0xFFD0D0D0u; // footer grey (208,208,208), same as the 5x7 color
    const size_t bytes = (std::min)(window.size(), sizeof(item.text) - 1);
    std::memcpy(item.text, window.data(), bytes);
    item.text[bytes] = '\0';
    ov::SubmitRtText(item);
    return ov::IsRtTextAvailable();
}

void DrawTooltipTextLines(
    const netplay::font::IndexedSurfaceView& surface,
    const std::string& text,
    int left,
    int right,
    int panelTop,
    uint8_t color)
{
    auto drawTooltipLine = [&](const std::string& line, int y)
    {
        const int availableWidth = right - left;
        if (availableWidth <= 0)
        {
            return;
        }

        // Crisp TTF path via the game-RT overlay; the panel/frame stay on the
        // indexed surface underneath.  Falls back to 5x7 per line.
        if (SubmitFooterRtLine(line, left, right, y))
        {
            return;
        }

        const int textWidth = netplay::font::MeasureText5x7Width(line, 1);
        bool hasNonAscii = false;
        for (unsigned char c : line)
        {
            if ((c & 0x80u) != 0)
            {
                hasNonAscii = true;
                break;
            }
        }

        if (textWidth <= availableWidth || hasNonAscii)
        {
            netplay::font::DrawTextLeft5x7(surface, line, left, right, y, 1, 1, color);
            return;
        }

        constexpr DWORD kScrollStepMs = 180;
        constexpr size_t kPadChars = 6;
        const size_t visibleChars = (std::max)(static_cast<size_t>(1), static_cast<size_t>(availableWidth / 6));
        const std::string spacer(kPadChars, ' ');
        const std::string marquee = line + spacer + line + spacer;
        const size_t cycle = line.size() + spacer.size();
        const size_t start = (GetTickCount() / kScrollStepMs) % cycle;
        const std::string window = marquee.substr(start, (std::min)(visibleChars + 2, marquee.size() - start));
        netplay::font::DrawTextLeft5x7(surface, window, left, right, y, 1, 1, color);
    };

    const size_t newline = text.find('\n');
    if (newline == std::string::npos)
    {
        drawTooltipLine(text, panelTop + ((netplay::constants::kNetplayFooterPanelHeight - 7) / 2));
        return;
    }

    const std::string line1 = text.substr(0, newline);
    const std::string line2 = text.substr(newline + 1);
    drawTooltipLine(line1, panelTop + 4);
    drawTooltipLine(line2, panelTop + 13);
}
}

std::string BuildFooterText()
{
    // The stop-hosting modal covers the footer area on the indexed surface,
    // but RT text draws above everything - suppress the footer outright so
    // it cannot float over the modal.
    if (g_stopHostingConfirm.active)
    {
        return {};
    }

    if (g_inlineEditState.active)
    {
        if (!g_inlineEditState.errorMessage.empty() && GetTickCount() < g_inlineEditState.errorExpireTick)
        {
            return g_inlineEditState.errorMessage + "  Enter=Save Esc=Cancel";
        }

        switch (g_inlineEditState.action)
        {
        case NetplayMenuAction::HostEditPort:
            return "Edit host port\nEnter=Save Esc=Cancel";
        case NetplayMenuAction::JoinEditAddress:
            return "Edit host address\nEnter=Save Esc=Cancel Ctrl+V=Paste";
        case NetplayMenuAction::JoinEditPort:
            return "Edit host port\nEnter=Save Esc=Cancel Ctrl+V=Paste";
        case NetplayMenuAction::NicknameEdit:
            return "Edit nickname\nEnter=Save Esc=Cancel Ctrl+V=Paste";
        default:
            return "Enter=Save Esc=Cancel";
        }
    }

    if (HasNetplayStatusMessage())
    {
        return GetNetplayStatusMessage();
    }

    if (g_netplayMenuState.menuId == NetplayMenuId::Options)
    {
        if (netplay::options::IsSaveOverlayActive())
        {
            return {};
        }

        const NetplayMenuEntry* entry = GetCurrentMenuEntry(
            ClampSelectionToCurrentMenu(static_cast<int>(g_lastLoggedSelection >= 0 ? g_lastLoggedSelection : 0)));
        const NetplayMenuAction action =
            entry != nullptr ? entry->action : NetplayMenuAction::BackToMain;
        const std::string footer = netplay::options::BuildFooterText(action);
        if (!footer.empty())
        {
            return footer;
        }
    }

    if (g_netplayMenuState.menuId == NetplayMenuId::BattleLog)
    {
        const NetplayMenuEntry* entry = GetCurrentMenuEntry(
            ClampSelectionToCurrentMenu(static_cast<int>(g_lastLoggedSelection >= 0 ? g_lastLoggedSelection : 0)));
        const NetplayMenuAction action =
            entry != nullptr ? entry->action : NetplayMenuAction::BattleLogBack;
        const std::string footer = netplay::battle_log::BuildFooterText(action);
        if (!footer.empty())
        {
            return footer;
        }
    }

    if (g_netplayMenuState.menuId == NetplayMenuId::PlayerRooms)
    {
        const NetplayMenuEntry* entry = GetCurrentMenuEntry(
            ClampSelectionToCurrentMenu(static_cast<int>(g_lastLoggedSelection >= 0 ? g_lastLoggedSelection : 0)));
        const NetplayMenuAction action =
            entry != nullptr ? entry->action : NetplayMenuAction::BackToMain;
        const std::string footer = netplay::player_rooms::BuildFooterText(action);
        if (!footer.empty())
        {
            return footer;
        }
    }

    const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
    const auto bridgePhase = static_cast<netplay::bridge::NetbridgePhase>(bridgeStatus.phase);
    const bool recoveryCompletedSessionEnded =
        bridgePhase == netplay::bridge::NetbridgePhase::SessionEnded
        && netplay::bridge::recovery::WasGameplayExitRecoveryCompleted();
    // While async hosting is minimized, the user is browsing the menu normally
    // (the hosting overlay is just a corner badge), so the per-entry footer
    // tooltip should still show - the session being in the Connecting phase and
    // g_hostingOverlay being active must NOT suppress it here.
    const bool minimizedHosting =
        netplay::bridge::async_host::IsActive() && netplay::bridge::async_host::IsMinimized();
    if (!minimizedHosting
        && ((bridgePhase != netplay::bridge::NetbridgePhase::Idle && !recoveryCompletedSessionEnded)
            || g_delaySetupOverlay.active
            || g_spectateConfirmOverlay.active
            || g_hostingOverlay.active
            || g_joiningOverlay.active))
    {
        return {};
    }

    if (const NetplayMenuEntry* entry = GetCurrentMenuEntry(
            ClampSelectionToCurrentMenu(static_cast<int>(g_lastLoggedSelection >= 0 ? g_lastLoggedSelection : 0)));
        entry != nullptr)
    {
        const std::string tip = BuildEntryTooltip(*entry);
        if (!tip.empty())
        {
            return AppendPlayerRoomCodeFooter(tip);
        }
    }
    return AppendPlayerRoomCodeFooter({});
}

bool DrawFooterTooltipOverlayGdi(uint32_t screenContext, bool /*allowWindowDc*/)
{
    if (!g_netplayMenuState.active)
    {
        return false;
    }

    const std::string tooltip = BuildFooterText();
    if (tooltip.empty())
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

    const uint8_t bgColor = netplay::draw::ResolveBestPaletteColor(screenContext, 0, 0, 0);
    const uint8_t frameColor = netplay::draw::ResolveBestPaletteColor(screenContext, 220, 220, 220);
    const uint8_t textColor = netplay::draw::ResolveBestPaletteColor(screenContext, 208, 208, 208);

    const int panelX = netplay::constants::kNetplayFooterTextLeft - 6;
    const int panelY = netplay::constants::kNetplayFooterPanelTopY;
    const int panelW =
        (netplay::constants::kNetplayFooterTextRight - netplay::constants::kNetplayFooterTextLeft) + 12;
    const int panelH = netplay::constants::kNetplayFooterPanelHeight;

    netplay::font::FillIndexedSurfaceRect(sv, panelX, panelY, panelW, panelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, panelX, panelY, panelW, panelH, frameColor);
    DrawTooltipTextLines(
        sv,
        tooltip,
        netplay::constants::kNetplayFooterTextLeft,
        netplay::constants::kNetplayFooterTextRight,
        panelY,
        textColor);

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
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
        [](const NetplayMenuEntry& entry) -> std::string { return BuildRowPrimaryText(entry); },
        [](const NetplayMenuEntry& entry) -> std::string { return BuildRowSecondaryText(entry); },
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
        // Section-header rows (small tinted labels, non-selectable). Only the
        // options menu declares them today; other menus can opt in here.
        [](const NetplayMenuEntry& entry) -> bool
        {
            return g_netplayMenuState.menuId == NetplayMenuId::Options
                && netplay::options::IsHeaderRowAction(entry.action);
        },
    };
    return callbacks;
}

bool DrawRuntimeTextOverlayGdi(uint32_t screenContext, bool allowWindowDc)
{
    // The lobby always forces GDI rendering regardless of whether the sprite
    // font is loaded or the global runtime-text / GDI-fallback flags are set.
    const bool lobbyForced = (g_netplayMenuState.menuId == netplay::menu::NetplayMenuId::Lobby);
    const int optionsSlideOffsetX =
        (g_netplayMenuState.menuId == netplay::menu::NetplayMenuId::Options)
            ? netplay::options::GetOptionsSlideOffsetX()
            : 0;
    const netplay::render::RuntimeOverlayState state = {
        g_useRuntimeTextOverlay || lobbyForced, // lobby treated as always enabled
        g_netplayMenuState.active,
        g_enableGdiFallbackOverlay || lobbyForced,
        netplay::options::IsSaveOverlayActive(), // modal covers the rows
        g_netplayMenuState.menuId,
        optionsSlideOffsetX,
    };
    return netplay::render::DrawRuntimeTextOverlayGdi(GetOverlayCallbacks(), state, screenContext, allowWindowDc);
}

namespace
{
constexpr uint32_t kRtOverlayTitle = 0xFFF5F5DCu;
constexpr uint32_t kRtOverlayText = 0xFFD0D0B4u;
constexpr uint32_t kRtOverlayBright = 0xFFFFFFFFu;
constexpr uint32_t kRtOverlayDim = 0xFFC6C6B0u;
constexpr uint32_t kRtOverlayError = 0xFF8282FFu;
constexpr uint32_t kRtOverlayRed = 0xFF5050FFu;
constexpr uint32_t kRtOverlayGreen = 0xFF82FF64u;
constexpr uint32_t kRtOverlayYellow = 0xFF64DCFFu;

// Transient panels keep their indexed background/frame, but submit their text
// to the existing game-RT TTF queue. The 5x7 call remains the bootstrap and
// failure fallback until ImGui has a loaded font atlas.
void DrawTransientTextCentered(
    const netplay::font::IndexedSurfaceView& surface,
    const char* text,
    int x0,
    int x1,
    int y,
    netplay::debug_overlay::RtTextProfile profile,
    uint8_t fallbackColor,
    uint32_t rgba)
{
    if (text == nullptr || text[0] == '\0')
    {
        return;
    }

    if (netplay::mod_settings::IsMenuTtfTextEnabled())
    {
        netplay::debug_overlay::RtTextItem item;
        item.x0 = static_cast<int16_t>(x0);
        item.x1 = static_cast<int16_t>(x1);
        item.y = static_cast<int16_t>(y);
        item.align = netplay::debug_overlay::RtTextAlign::Center;
        item.profile = profile;
        item.rgba = rgba;
        const size_t bytes = (std::min)(std::strlen(text), sizeof(item.text) - 1);
        std::memcpy(item.text, text, bytes);
        item.text[bytes] = '\0';
        netplay::debug_overlay::SubmitRtText(item);
        if (netplay::debug_overlay::IsRtTextAvailable())
        {
            return;
        }
    }

    netplay::font::DrawTextCentered5x7(
        surface,
        text,
        x0,
        x1,
        y,
        1,
        1,
        fallbackColor);
}

// TTF items are rendered together at EndScene, after all indexed panel fills.
// A secondary modal must therefore discard text staged by the covered menu or
// connection panel; otherwise that older text appears on top of the modal.
void BeginTopModalTextLayer()
{
    if (netplay::mod_settings::IsMenuTtfTextEnabled())
    {
        netplay::debug_overlay::BeginRtTextFrame();
    }
}
} // namespace

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

    // Panel background and frame - centered on 320x240 surface
    constexpr int delayPanelW = 296;
    constexpr int delayPanelH = 96;
    constexpr int delayPanelX = (320 - delayPanelW) / 2;
    constexpr int delayPanelY = (240 - delayPanelH) / 2;
    netplay::font::FillIndexedSurfaceRect(sv, delayPanelX, delayPanelY, delayPanelW, delayPanelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, delayPanelX, delayPanelY, delayPanelW, delayPanelH, frameColor);

    const int cL = delayPanelX + 6;
    const int cR = delayPanelX + delayPanelW - 6;

    DrawTransientTextCentered(
        sv, "MATCH DELAY", cL, cR, delayPanelY + 8,
        netplay::debug_overlay::RtTextProfile::OverlayTitle,
        titleColor, kRtOverlayTitle);

    char line[192] = {};
    if (g_delaySetupOverlay.pingMs >= 0)
    {
        std::snprintf(line, sizeof(line), "Ping: %d ms", g_delaySetupOverlay.pingMs);
    }
    else
    {
        std::snprintf(line, sizeof(line), "Ping: measuring...");
    }
    DrawTransientTextCentered(
        sv, line, cL, cR, delayPanelY + 26,
        netplay::debug_overlay::RtTextProfile::OverlayBody,
        textColor, kRtOverlayText);

    std::snprintf(line, sizeof(line), "Delay: %d   (recommended %d)",
        g_delaySetupOverlay.selectedDelay, g_delaySetupOverlay.recommendedDelay);
    DrawTransientTextCentered(
        sv, line, cL, cR, delayPanelY + 42,
        netplay::debug_overlay::RtTextProfile::OverlayBody,
        brightColor, kRtOverlayBright);

    std::snprintf(line, sizeof(line), "Range: %d to %d",
        g_delaySetupOverlay.minDelay, g_delaySetupOverlay.maxDelay);
    DrawTransientTextCentered(
        sv, line, cL, cR, delayPanelY + 56,
        netplay::debug_overlay::RtTextProfile::OverlayHint,
        dimColor, kRtOverlayDim);

    // Status line: waiting / error / player names.
    if (g_delaySetupOverlay.waitingForRuntimeReady)
    {
        DrawTransientTextCentered(
            sv, "Delay submitted. Waiting for sync...", cL, cR, delayPanelY + 72,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            textColor, kRtOverlayText);
    }
    else
    {
        if (g_delaySetupOverlay.errorMessage[0] != '\0')
        {
            DrawTransientTextCentered(
                sv, g_delaySetupOverlay.errorMessage, cL, cR, delayPanelY + 70,
                netplay::debug_overlay::RtTextProfile::OverlayBody,
                errorColor, kRtOverlayError);
        }
        else if (g_delaySetupOverlay.p1Name[0] != '\0' && g_delaySetupOverlay.p2Name[0] != '\0')
        {
            std::snprintf(line, sizeof(line), "%s vs %s", g_delaySetupOverlay.p1Name, g_delaySetupOverlay.p2Name);
            DrawTransientTextCentered(
                sv, line, cL, cR, delayPanelY + 70,
                netplay::debug_overlay::RtTextProfile::OverlayBody,
                textColor, kRtOverlayText);
        }
        DrawTransientTextCentered(
            sv, "Left/Right: Adjust    A: Confirm    B: Cancel",
            cL, cR, delayPanelY + 84,
            netplay::debug_overlay::RtTextProfile::OverlayHint,
            dimColor, kRtOverlayDim);
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

bool DrawHostingOverlayGdi(uint32_t screenContext, bool /*allowWindowDc*/)
{
    // Both the full hosting panel AND the minimized top-right badge are netplay-
    // menu elements. Outside the menu the ImGui top-middle badge handles the
    // indicator (see netplay::debug_overlay::DrawAsyncIndicator), so this indexed
    // path only renders while the netplay menu is open.
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
    const uint8_t yellowColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 220, 100);
    const uint8_t redColor    = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 80, 80);

    // Minimized: draw a small top-right badge instead of the full panel, so the
    // user can browse the rest of the netplay menu while still hosting.
    if (!g_hostingOverlay.failed
        && !g_hostingOverlay.challengeMode
        && netplay::bridge::async_host::IsMinimized())
    {
        const char* badge = "HOSTING";
        uint8_t badgeColor = frameColor;
        if (netplay::bridge::async_host::
                HasHostListenerStartupFailed())
        {
            badge = "HOST FAILED";
            badgeColor = redColor;
        }
        else if (netplay::bridge::async_host::IsTimedOut())
        {
            badge = "TIMED OUT";
            badgeColor = yellowColor;
        }
        else if (netplay::bridge::async_host::IsPeerFoundHeld())
        {
            badge = "OPPONENT FOUND!";
            badgeColor = greenColor;
        }
        int badgeTextW = netplay::font::MeasureText5x7Width(badge, 1);
        if (netplay::mod_settings::IsMenuTtfTextEnabled()
            && netplay::debug_overlay::IsRtTextAvailable())
        {
            const int rtWidth = netplay::debug_overlay::MeasureRtTextWidth(
                netplay::debug_overlay::RtTextProfile::OverlayHint,
                badge);
            if (rtWidth >= 0)
            {
                badgeTextW = rtWidth;
            }
        }
        const int badgeW = badgeTextW + 12;
        const int badgeX = 320 - badgeW - 4;
        const int badgeY = 4;
        netplay::font::FillIndexedSurfaceRect(sv, badgeX, badgeY, badgeW, 13, bgColor);
        netplay::font::DrawIndexedSurfaceFrame(sv, badgeX, badgeY, badgeW, 13, badgeColor);
        DrawTransientTextCentered(
            sv, badge, badgeX + 2, badgeX + badgeW - 2, badgeY + 3,
            netplay::debug_overlay::RtTextProfile::OverlayHint,
            badgeColor,
            netplay::bridge::async_host::
                    HasHostListenerStartupFailed()
                ? kRtOverlayRed
                : netplay::bridge::async_host::IsTimedOut()
                    ? kRtOverlayYellow
                : (netplay::bridge::async_host::IsPeerFoundHeld()
                    ? kRtOverlayGreen
                    : kRtOverlayText));
        netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
        return true;
    }

    constexpr int panelW = 296;
    constexpr int panelH = 74;
    constexpr int panelX = (320 - panelW) / 2;
    constexpr int panelY = (240 - panelH) / 2;
    netplay::font::FillIndexedSurfaceRect(sv, panelX, panelY, panelW, panelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, panelX, panelY, panelW, panelH, frameColor);

    const int cL = panelX + 6;
    const int cR = panelX + panelW - 6;

    // Centered title.
    DrawTransientTextCentered(
        sv,
        g_hostingOverlay.challengeMode ? "CHALLENGING" : "HOSTING",
        cL, cR, panelY + 8,
        netplay::debug_overlay::RtTextProfile::OverlayTitle,
        titleColor, kRtOverlayTitle);

    if (g_hostingOverlay.failed)
    {
        DrawTransientTextCentered(
            sv, "Hosting failed", cL, cR, panelY + 28,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            redColor, kRtOverlayRed);
        if (g_hostingOverlay.errorText[0] != '\0')
        {
            DrawTransientTextCentered(
                sv, g_hostingOverlay.errorText,
                cL, cR, panelY + 42,
                netplay::debug_overlay::RtTextProfile::OverlayBody,
                textColor, kRtOverlayText);
        }
        DrawTransientTextCentered(
            sv, "Press any button to dismiss",
            cL, cR, panelY + 58,
            netplay::debug_overlay::RtTextProfile::OverlayHint,
            dimColor, kRtOverlayDim);
        netplay::draw::ReleaseMenuDrawSurfaceLock(
            lockedSurface);
        return true;
    }

    char line[192] = {};
    if (g_hostingOverlay.discoveryInProgress)
    {
        std::snprintf(
            line,
            sizeof(line),
            "Detecting public %s address...",
            netplay::network::FamilyName(
                g_hostingOverlay.effectiveFamily));
        DrawTransientTextCentered(
            sv, line, cL, cR, panelY + 28,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            dimColor, kRtOverlayDim);
        DrawTransientTextCentered(
            sv,
            g_hostingOverlay.challengeMode
                ? "B/ESC: Cancel challenge"
                : "B/ESC: Cancel hosting",
            cL, cR, panelY + 58,
            netplay::debug_overlay::RtTextProfile::OverlayHint,
            textColor, kRtOverlayText);
    }
    else if (g_hostingOverlay.challengeMode)
    {
        if (!g_hostingOverlay.listenerReady)
        {
            std::snprintf(
                line,
                sizeof(line),
                "Starting %s Revival netplay session...",
                netplay::network::FamilyName(
                    g_hostingOverlay.effectiveFamily));
        }
        else
        {
            std::snprintf(
                line,
                sizeof(line),
                "vs %s",
                g_hostingOverlay.targetName);
        }
        DrawTransientTextCentered(
            sv, line, cL, cR, panelY + 28,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            brightColor, kRtOverlayBright);
        DrawTransientTextCentered(
            sv, "B/ESC: Cancel challenge", cL, cR, panelY + 58,
            netplay::debug_overlay::RtTextProfile::OverlayHint,
            textColor, kRtOverlayText);
    }
    else if (g_hostingOverlay.listenerMismatch)
    {
        DrawTransientTextCentered(
            sv, "Retrying hosting setup...", cL, cR, panelY + 28,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            yellowColor, kRtOverlayYellow);
    }
    else if (!g_hostingOverlay.listenerReady)
    {
        std::snprintf(
            line,
            sizeof(line),
            "Starting %s Revival netplay session...",
            netplay::network::FamilyName(
                g_hostingOverlay.effectiveFamily));
        DrawTransientTextCentered(
            sv, line, cL, cR, panelY + 28,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            dimColor, kRtOverlayDim);
    }
    else if (g_hostingOverlay.ipFetchFailed)
    {
        std::snprintf(
            line,
            sizeof(line),
            "%s ready on port %u; public IP unavailable",
            netplay::network::FamilyName(
                g_hostingOverlay.effectiveFamily),
            static_cast<unsigned>(g_hostingOverlay.port));
        DrawTransientTextCentered(
            sv, line, cL, cR, panelY + 28,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            textColor, kRtOverlayText);
    }
    else
    {
        netplay::network::NetworkEndpoint endpoint;
        endpoint.family = g_hostingOverlay.effectiveFamily;
        endpoint.host = g_hostingOverlay.publicIp;
        endpoint.port = g_hostingOverlay.port;
        std::string formattedEndpoint;
        if (!netplay::network::FormatEndpoint(
                endpoint,
                &formattedEndpoint))
        {
            formattedEndpoint = "Invalid detected endpoint";
        }
        DrawTransientTextCentered(
            sv, formattedEndpoint.c_str(), cL, cR, panelY + 28,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            brightColor, kRtOverlayBright);
    }

    if (!g_hostingOverlay.challengeMode)
    {
        // Middle line: copy hint or "copied" flash.
        const bool showCopiedFlash = g_hostingOverlay.copiedToClipboard
            && (GetTickCount() - g_hostingOverlay.copiedFlashTick) < 2000;
        if (showCopiedFlash)
        {
            DrawTransientTextCentered(
                sv, "Copied to clipboard!", cL, cR, panelY + 42,
                netplay::debug_overlay::RtTextProfile::OverlayBody,
                greenColor, kRtOverlayGreen);
        }
        else if (g_hostingOverlay.listenerReady
            && g_hostingOverlay.usedFamilyFallback)
        {
            if (g_hostingOverlay
                    .automaticFamilyRetryAttempted)
            {
                std::snprintf(
                    line,
                    sizeof(line),
                    "Automatic retry succeeded with %s",
                    netplay::network::FamilyName(
                        g_hostingOverlay.effectiveFamily));
            }
            else
            {
                std::snprintf(
                    line,
                    sizeof(line),
                    "%s address not detected; using %s",
                    netplay::network::FamilyName(
                        g_hostingOverlay.preferredFamily),
                    netplay::network::FamilyName(
                        g_hostingOverlay.effectiveFamily));
            }
            DrawTransientTextCentered(
                sv, line, cL, cR, panelY + 42,
                netplay::debug_overlay::RtTextProfile::OverlayHint,
                yellowColor, kRtOverlayYellow);
        }
        else if (g_hostingOverlay.listenerReady
            && g_hostingOverlay.ipFetchDone
            && !g_hostingOverlay.ipFetchFailed)
        {
            DrawTransientTextCentered(
                sv, "Press C to copy IP address", cL, cR, panelY + 42,
                netplay::debug_overlay::RtTextProfile::OverlayHint,
                dimColor, kRtOverlayDim);
        }

        // Bottom line: state / controls.
        if (!g_hostingOverlay.listenerReady)
        {
            DrawTransientTextCentered(
                sv, "B/ESC: Cancel", cL, cR, panelY + 58,
                netplay::debug_overlay::RtTextProfile::OverlayHint,
                textColor, kRtOverlayText);
        }
        else if (netplay::bridge::async_host::
                     HasHostListenerStartupFailed())
        {
            DrawTransientTextCentered(
                sv, "HOSTING FAILED   B/ESC: Cancel",
                cL, cR, panelY + 58,
                netplay::debug_overlay::RtTextProfile::OverlayHint,
                textColor, kRtOverlayText);
        }
        else if (netplay::bridge::async_host::IsTimedOut())
        {
            DrawTransientTextCentered(
                sv, "OPPONENT TIMED OUT   B/ESC: Cancel", cL, cR, panelY + 58,
                netplay::debug_overlay::RtTextProfile::OverlayHint,
                textColor, kRtOverlayText);
        }
        else if (netplay::bridge::async_host::IsPeerFoundHeld())
        {
            // Accept is automatic once this overlay is on screen - no manual key.
            DrawTransientTextCentered(
                sv, "OPPONENT FOUND!", cL, cR, panelY + 58,
                netplay::debug_overlay::RtTextProfile::OverlayBody,
                greenColor, kRtOverlayGreen);
        }
        else
        {
            DrawTransientTextCentered(
                sv, "D: Minimize    B/ESC: Cancel", cL, cR, panelY + 58,
                netplay::debug_overlay::RtTextProfile::OverlayHint,
                textColor, kRtOverlayText);
        }
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

// Compile-time switch for the in-gameplay async-host indicator.
//
// NOTE: This indexed-surface path does NOT render during battle/practice -
// EFZ Revival presents the gameplay frame through Direct3D9, so drawing onto the
// DirectDraw-style menu backbuffer here is never shown in-match. The working
// in-battle indicator must go through the D3D9 EndScene hook (see
// battle_log_menu.cpp's textured-quad overlay), which is a separate piece of
// work. Disabled until that D3D9 text path lands.
static constexpr bool kEnableAsyncHostGameplayOverlay = false;

// "Stop hosting?" confirmation modal - shown over the netplay menu when the
// user picks a host-conflicting option (Join / Lobby / Player Rooms) while an
// async-host listener is active. YES stops hosting and proceeds; NO keeps it.
bool DrawStopHostingConfirmGdi(uint32_t screenContext)
{
    if (!g_netplayMenuState.active || !g_stopHostingConfirm.active)
    {
        return false;
    }

    netplay::draw::LockedMenuSurface lockedSurface;
    if (!netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        return false;
    }

    BeginTopModalTextLayer();

    const netplay::font::IndexedSurfaceView sv = {
        lockedSurface.pixels, lockedSurface.width, lockedSurface.height, lockedSurface.pitch,
    };

    const uint8_t bgColor     = netplay::draw::ResolveBestPaletteColor(screenContext, 8, 16, 28);
    const uint8_t frameColor  = netplay::draw::ResolveBestPaletteColor(screenContext, 96, 210, 200);
    const uint8_t titleColor  = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 220, 100);
    const uint8_t textColor   = netplay::draw::ResolveBestPaletteColor(screenContext, 180, 208, 208);
    const uint8_t brightColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 255, 255);
    const uint8_t dimColor    = netplay::draw::ResolveBestPaletteColor(screenContext, 176, 198, 198);
    const uint8_t hlColor     = netplay::draw::ResolveBestPaletteColor(screenContext, 40, 70, 90);

    constexpr int panelW = 240;
    constexpr int panelH = 80;
    constexpr int panelX = (320 - panelW) / 2;
    constexpr int panelY = (240 - panelH) / 2;
    netplay::font::FillIndexedSurfaceRect(sv, panelX, panelY, panelW, panelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, panelX, panelY, panelW, panelH, frameColor);

    const int cL = panelX + 6;
    const int cR = panelX + panelW - 6;
    DrawTransientTextCentered(
        sv, "STOP HOSTING?", cL, cR, panelY + 8,
        netplay::debug_overlay::RtTextProfile::OverlayTitle,
        titleColor, kRtOverlayYellow);
    DrawTransientTextCentered(
        sv, "End the host session to continue?", cL, cR, panelY + 24,
        netplay::debug_overlay::RtTextProfile::OverlayBody,
        textColor, kRtOverlayText);

    const char* labels[2] = {"Stop hosting", "Keep hosting"};
    for (int i = 0; i < 2; ++i)
    {
        const bool selected = (g_stopHostingConfirm.selection == i);
        const int rowY = panelY + 44 + i * 16;
        if (selected)
        {
            netplay::font::FillIndexedSurfaceRect(sv, panelX + 30, rowY - 1, panelW - 60, 13, hlColor);
        }
        char optionText[48] = {};
        std::snprintf(
            optionText,
            sizeof(optionText),
            "%s%s",
            selected ? "> " : "  ",
            labels[i]);
        DrawTransientTextCentered(
            sv, optionText, cL, cR, rowY + 2,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            selected ? brightColor : dimColor,
            selected ? kRtOverlayBright : kRtOverlayDim);
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

// Small top-of-screen indicator drawn while the host overlay is MINIMIZED and
// the user is in gameplay (practice/VS-CPU). Rendered from the battle update
// hook. Every EFZ screen object shares the +0x20 graphics-context / palette
// header, so the menu surface lock works here too; AcquireMenuDrawSurfaceLock
// fails gracefully if the surface is not lockable.
void DrawAsyncHostGameplayOverlay(uint32_t screenContext)
{
    namespace ah = netplay::bridge::async_host;
    if (!kEnableAsyncHostGameplayOverlay)
    {
        return;
    }
    if (!ah::IsActive() || !ah::IsMinimized())
    {
        return;
    }

    netplay::draw::LockedMenuSurface lockedSurface;
    if (!netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        return;
    }

    const netplay::font::IndexedSurfaceView sv = {
        lockedSurface.pixels,
        lockedSurface.width,
        lockedSurface.height,
        lockedSurface.pitch,
    };

    const uint8_t bgColor    = netplay::draw::ResolveBestPaletteColor(screenContext, 8, 16, 28);
    const uint8_t textColor  = netplay::draw::ResolveBestPaletteColor(screenContext, 200, 224, 224);
    const uint8_t greenColor = netplay::draw::ResolveBestPaletteColor(screenContext, 100, 255, 130);
    const uint8_t redColor   = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 80, 80);

    char msg[160] = {};
    uint8_t col = textColor;
    if (ah::HasHostListenerStartupFailed())
    {
        std::snprintf(
            msg,
            sizeof(msg),
            "Hosting failed - %s to return",
            ah::ReturnKeyDisplay());
        col = redColor;
    }
    else if (ah::IsTimedOut())
    {
        std::snprintf(
            msg,
            sizeof(msg),
            "Opponent timed out - %s to return",
            ah::ReturnKeyDisplay());
    }
    else if (ah::IsPeerFoundHeld())
    {
        std::snprintf(msg, sizeof(msg), "OPPONENT FOUND!  Press %s to join", ah::ReturnKeyDisplay());
        col = greenColor;
    }
    else
    {
        std::snprintf(msg, sizeof(msg), "Hosting...  (%s to return)", ah::ReturnKeyDisplay());
    }

    const int surfaceW = static_cast<int>(lockedSurface.width);
    const int w = netplay::font::MeasureText5x7Width(msg, 1);
    int x = (surfaceW - w) / 2;
    if (x < 3)
    {
        x = 3;
    }
    netplay::font::FillIndexedSurfaceRect(sv, x - 3, 1, w + 6, 11, bgColor);
    netplay::font::DrawTextLeft5x7(sv, msg, x, x + w, 2, 1, 1, col);

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
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

    constexpr int panelW = 296;
    constexpr int panelH = 74;
    constexpr int panelX = (320 - panelW) / 2;
    constexpr int panelY = (240 - panelH) / 2;
    netplay::font::FillIndexedSurfaceRect(sv, panelX, panelY, panelW, panelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, panelX, panelY, panelW, panelH, frameColor);

    const int cL = panelX + 6;
    const int cR = panelX + panelW - 6;

    const char* panelTitle = "JOINING";
    if (g_joiningOverlay.spectateMode)
    {
        panelTitle = g_joiningOverlay.waitingForGameBegin ? "WAIT TO SPECTATE" : "SPECTATING";
    }
    DrawTransientTextCentered(
        sv, panelTitle, cL, cR, panelY + 8,
        netplay::debug_overlay::RtTextProfile::OverlayTitle,
        titleColor, kRtOverlayTitle);

    char line[192] = {};
    if (g_joiningOverlay.failed)
    {
        DrawTransientTextCentered(
            sv, "Connection failed", cL, cR, panelY + 28,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            redColor, kRtOverlayRed);
        if (g_joiningOverlay.errorText[0] != '\0')
        {
            DrawTransientTextCentered(
                sv, g_joiningOverlay.errorText, cL, cR, panelY + 42,
                netplay::debug_overlay::RtTextProfile::OverlayBody,
                textColor, kRtOverlayText);
        }
        DrawTransientTextCentered(
            sv, "Press any button to dismiss", cL, cR, panelY + 58,
            netplay::debug_overlay::RtTextProfile::OverlayHint,
            dimColor, kRtOverlayDim);
    }
    else if (g_joiningOverlay.waitingForGameBegin)
    {
        if (g_joiningOverlay.displayTargetName && g_joiningOverlay.targetName[0] != '\0')
        {
            std::snprintf(line, sizeof(line), "%s is not in a match yet", g_joiningOverlay.targetName);
        }
        else
        {
            std::snprintf(line, sizeof(line), "Host is not in a match yet");
        }
        DrawTransientTextCentered(
            sv, line, cL, cR, panelY + 28,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            textColor, kRtOverlayText);
        DrawTransientTextCentered(
            sv, "Waiting for game to begin...", cL, cR, panelY + 42,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            dimColor, kRtOverlayDim);
        DrawTransientTextCentered(
            sv, "B/ESC: Cancel", cL, cR, panelY + 58,
            netplay::debug_overlay::RtTextProfile::OverlayHint,
            dimColor, kRtOverlayDim);
    }
    else
    {
        if (g_joiningOverlay.displayTargetName && g_joiningOverlay.targetName[0] != '\0')
        {
            std::snprintf(line, sizeof(line), "Connecting to %s ...", g_joiningOverlay.targetName);
        }
        else
        {
            netplay::network::NetworkEndpoint endpoint;
            endpoint.family = g_joiningOverlay.family;
            endpoint.host = g_joiningOverlay.address;
            endpoint.port = g_joiningOverlay.port;
            std::string formattedEndpoint;
            if (!netplay::network::FormatEndpoint(
                    endpoint,
                    &formattedEndpoint))
            {
                formattedEndpoint = "invalid endpoint";
            }
            std::snprintf(
                line,
                sizeof(line),
                "Connecting to %s ...",
                formattedEndpoint.c_str());
        }
        DrawTransientTextCentered(
            sv, line, cL, cR, panelY + 30,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            textColor, kRtOverlayText);
        DrawTransientTextCentered(
            sv, "B/ESC: Cancel", cL, cR, panelY + 58,
            netplay::debug_overlay::RtTextProfile::OverlayHint,
            dimColor, kRtOverlayDim);
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

    BeginTopModalTextLayer();

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

    const bool hostNotYetPlayingPrompt =
        g_spectateConfirmOverlay.promptKind
            == static_cast<int>(netplay::bridge::NetbridgeSpectatePromptKind::HostNotYetPlaying);

    // Panel background and frame - centered on 320x240 surface
    constexpr int specPanelW = 240;
    const int specPanelH = hostNotYetPlayingPrompt ? 130 : 112;
    constexpr int specPanelX = (320 - specPanelW) / 2;
    const int specPanelY = (240 - specPanelH) / 2;
    netplay::font::FillIndexedSurfaceRect(sv, specPanelX, specPanelY, specPanelW, specPanelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, specPanelX, specPanelY, specPanelW, specPanelH, frameColor);

    const int sTextL = specPanelX + 6;
    const int sTextR = specPanelX + specPanelW - 6;

    auto drawOption = [&](int optionIndex, const char* label, int y)
    {
        const bool selected = (g_spectateConfirmOverlay.selectedOption == optionIndex);
        if (selected)
        {
            netplay::font::FillIndexedSurfaceRect(sv, specPanelX + 40, y - 2, specPanelW - 80, 12, hlColor);
        }
        char optionText[40] = {};
        std::snprintf(optionText, sizeof(optionText), "%s %s", selected ? ">" : " ", label);
        DrawTransientTextCentered(
            sv,
            optionText,
            specPanelX + 40,
            specPanelX + specPanelW - 40,
            y,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            selected ? brightColor : dimColor,
            selected ? kRtOverlayBright : kRtOverlayDim);
    };

    if (hostNotYetPlayingPrompt)
    {
        DrawTransientTextCentered(
            sv, "HOST NOT PLAYING", sTextL, sTextR, specPanelY + 6,
            netplay::debug_overlay::RtTextProfile::OverlayTitle,
            titleColor, kRtOverlayTitle);
        DrawTransientTextCentered(
            sv, "Host is not in a match yet.", sTextL, sTextR, specPanelY + 24,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            textColor, kRtOverlayText);
        DrawTransientTextCentered(
            sv, "Choose what to do:", sTextL, sTextR, specPanelY + 38,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            textColor, kRtOverlayText);
        drawOption(0, "Join", specPanelY + 56);
        drawOption(1, "Wait", specPanelY + 70);
        drawOption(2, "Cancel", specPanelY + 84);
    }
    else
    {
        DrawTransientTextCentered(
            sv, "SPECTATE?", sTextL, sTextR, specPanelY + 6,
            netplay::debug_overlay::RtTextProfile::OverlayTitle,
            titleColor, kRtOverlayTitle);
        DrawTransientTextCentered(
            sv, "Host is already in a match.", sTextL, sTextR, specPanelY + 24,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            textColor, kRtOverlayText);
        DrawTransientTextCentered(
            sv, "Join as a spectator?", sTextL, sTextR, specPanelY + 40,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            textColor, kRtOverlayText);
        drawOption(0, "Yes", specPanelY + 60);
        drawOption(1, "No", specPanelY + 76);
    }

    // Error message (if any) just above the controls footer.
    if (g_spectateConfirmOverlay.errorMessage[0] != '\0')
    {
        DrawTransientTextCentered(
            sv,
            g_spectateConfirmOverlay.errorMessage,
            sTextL,
            sTextR,
            specPanelY + specPanelH - 26,
            netplay::debug_overlay::RtTextProfile::OverlayBody,
            errorColor,
            kRtOverlayError);
    }

    // Controls footer.
    DrawTransientTextCentered(
        sv, "Up/Down: Select    A: Confirm    B: Cancel",
        sTextL, sTextR, specPanelY + specPanelH - 13,
        netplay::debug_overlay::RtTextProfile::OverlayHint,
        dimColor, kRtOverlayDim);

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

// ---------------------------------------------------------------------------
// Debug overlay - toggled with keyboard D key
// ---------------------------------------------------------------------------

static constexpr int kDebugMenuItemCount = 6;
static constexpr const char* kDebugMenuItems[kDebugMenuItemCount] = {
    "Delay Overlay",
    "Spectate Overlay",
    "Runtime Text Overlay",
    "Console Window",
    "Wait to Spectate",
    "Close",
};

bool DrawDebugOverlay(uint32_t screenContext)
{
    if (!netplay::mod_settings::IsDebugMenuEnabled())
    {
        g_debugOverlay.open = false;
        return false;
    }

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
    netplay::font::DrawTextCentered5x7(sv, "DEBUG MENU", panelLeft + 2, panelRight - 2, panelTop + 4, 1, 1, titleColor);

    // Items
    const bool toggleStates[kDebugMenuItemCount] = {
        g_debugOverlay.forceDelayOverlay,
        g_debugOverlay.forceSpectateOverlay,
        g_debugOverlay.forceRuntimeTextOverlay,
        g_debugOverlay.showConsole,
        false, // "Wait to Spectate" - action, no toggle
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
        // Items 0..3 are toggles, items 4+ are plain actions/close
        char label[64] = {};
        const bool isToggleItem = (i <= 3);
        if (isToggleItem)
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
        if (isToggleItem && toggleStates[i])
        {
            textColor = onColor;
        }
        else if (isToggleItem && !toggleStates[i])
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

    if (!netplay::mod_settings::IsDebugMenuEnabled())
    {
        g_debugOverlay.open = false;
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
                g_debugOverlay.showConsole = !g_debugOverlay.showConsole;
                mod::SetConsoleVisible(g_debugOverlay.showConsole);
                mod::Log("DebugOverlay: Console window %s",
                    g_debugOverlay.showConsole ? "ON" : "OFF");
            }
            else if (sel == 4)
            {
                std::string errorMessage;
                const bool started = TryStartWaitToSpectateFromJoinSettings(screenContext, &errorMessage);
                if (started)
                {
                    g_debugOverlay.open = false;
                    mod::Log("DebugOverlay: Wait to Spectate started");
                }
                else
                {
                    mod::Log(
                        "DebugOverlay: Wait to Spectate failed -> %s",
                        errorMessage.empty() ? "Unknown error" : errorMessage.c_str());
                }
            }
            else if (sel == 5)
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
