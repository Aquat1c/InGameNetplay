#include "netplay/render/menu_overlay.h"

#include "netplay/core/constants.h"
#include "netplay/render/draw_surface.h"
#include "netplay/render/software_font.h"

#include <algorithm>
#include <windows.h>

namespace netplay::render
{
namespace
{
constexpr int kOptionsTextLeft = 8;
constexpr int kOptionsTextRight = 314;

void DrawSplitRowText(
    const OverlayCallbacks& callbacks,
    const netplay::font::IndexedSurfaceView& surface,
    const netplay::menu::NetplayMenuEntry& entry,
    int leftX,
    int rightX,
    int y,
    int scaleX,
    int scaleY,
    uint8_t color)
{
    const std::string primary =
        callbacks.buildRowPrimaryText
        ? callbacks.buildRowPrimaryText(entry)
        : callbacks.buildRowLabel(entry);
    const std::string secondary =
        callbacks.buildRowSecondaryText
        ? callbacks.buildRowSecondaryText(entry)
        : std::string();
    if (secondary.empty())
    {
        netplay::font::DrawTextLeft5x7(surface, primary, leftX, rightX, y, scaleX, scaleY, color);
        return;
    }

    constexpr int kColumnGap = 8;
    constexpr int kMinPrimaryWidth = 86;
    const int secondaryWidth = netplay::font::MeasureText5x7Width(secondary, scaleX);
    const int secondaryLeft = (std::max)(leftX + kMinPrimaryWidth, rightX - secondaryWidth);
    const int primaryRight = (std::max)(leftX, secondaryLeft - kColumnGap);

    netplay::font::DrawTextLeft5x7(surface, primary, leftX, primaryRight, y, scaleX, scaleY, color);
    netplay::font::DrawTextRight5x7(surface, secondary, secondaryLeft, rightX, y, scaleX, scaleY, color);
}
} // namespace

bool DrawRuntimeTextOverlayGdi(
    const OverlayCallbacks& callbacks,
    const RuntimeOverlayState& state,
    uint32_t screenContext,
    bool /*allowWindowDc*/)
{
    if (!state.useRuntimeTextOverlay || !state.netplayActive || !state.enableGdiFallbackOverlay)
    {
        return false;
    }
    if (!callbacks.clampSelectionToCurrentMenu || !callbacks.getMenuEntries || !callbacks.buildMenuHeaderText
        || !callbacks.buildRowLabel || !callbacks.getMenuOverlayFont)
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
    const uint8_t headerColor = netplay::draw::ResolveBestPaletteColor(screenContext, 32, 32, 32);
    const uint8_t selectedTextColor = netplay::draw::ResolveBestPaletteColor(screenContext, 8, 8, 8);
    const uint8_t normalTextColor = netplay::draw::ResolveBestPaletteColor(screenContext, 108, 108, 108);
    const uint8_t highlightColor = netplay::draw::ResolveBestPaletteColor(screenContext, 110, 225, 214);

    const bool wideDynamicMenu =
        state.menuId == netplay::menu::NetplayMenuId::Options
        || state.menuId == netplay::menu::NetplayMenuId::PlayerRooms;
    const int panelLeft = wideDynamicMenu ? kOptionsTextLeft : 150;
    const int panelRight = wideDynamicMenu ? kOptionsTextRight : 314;
    constexpr int rowHeight = netplay::constants::kNetplayDefaultHighlightHeight;
    constexpr int rowStep = netplay::constants::kNetplayCompactMenuRowStep;

    // Header
    const std::string header = callbacks.buildMenuHeaderText();
    netplay::font::DrawTextLeft5x7(sv, header, panelLeft, panelRight, 18, 1, 1, headerColor);

    // Rows
    const int selection =
        callbacks.clampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection)));
    int count = 0;
    const netplay::menu::NetplayMenuEntry* entries = callbacks.getMenuEntries(state.menuId, &count);
    if (entries != nullptr && count > 0)
    {
        constexpr int panelTop = netplay::constants::kNetplayCompactMenuTopY;
        for (int i = 0; i < count; ++i)
        {
            const int rowY = panelTop + i * rowStep;
            const bool isSelected = (i == selection);
            if (isSelected)
            {
                netplay::font::FillIndexedSurfaceRect(sv, panelLeft, rowY, panelRight - panelLeft, rowHeight, highlightColor);
            }

            const int textY = rowY + (rowHeight - 7) / 2; // center 5x7 glyph in row
            const uint8_t color = isSelected ? selectedTextColor : normalTextColor;
            if (wideDynamicMenu)
            {
                DrawSplitRowText(callbacks, sv, entries[i], panelLeft + 2, panelRight - 2, textY, 1, 1, color);
            }
            else
            {
                const std::string label = callbacks.buildRowLabel(entries[i]);
                netplay::font::DrawTextLeft5x7(sv, label, panelLeft + 2, panelRight - 2, textY, 1, 1, color);
            }
        }
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

bool DrawDynamicFieldValuesGdi(
    const OverlayCallbacks& callbacks,
    const DynamicFieldOverlayState& state,
    uint32_t screenContext,
    bool /*allowWindowDc*/)
{
    if (!state.netplayActive || state.useRuntimeTextOverlay || state.menuSlideActive)
    {
        return false;
    }
    if (!callbacks.clampSelectionToCurrentMenu || !callbacks.getMenuEntries || !callbacks.getMenuOverlayFont
        || !callbacks.isInlineEditableAction || !callbacks.getInlineEditDisplayValue || !callbacks.getScaledNativeSlideY)
    {
        return false;
    }

    int count = 0;
    const netplay::menu::NetplayMenuEntry* entries = callbacks.getMenuEntries(state.menuId, &count);
    const int selected =
        callbacks.clampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection)));

    bool hasDynamicField = false;
    if (entries != nullptr && count > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            if (callbacks.isInlineEditableAction(entries[i].action)
                || entries[i].action == netplay::menu::NetplayMenuAction::OpenLobby)
            {
                hasDynamicField = true;
                break;
            }
        }
    }
    // Main menu labels (HOST, JOIN, LOBBY, etc.) are baked into the sprite sheet,
    // so no dynamic text is needed there.
    if (state.menuId == netplay::menu::NetplayMenuId::Main)
    {
        return true;
    }
    // Lobby rows all carry dynamic GDI labels (player names, playing pair, Back).
    const bool isLobby = (state.menuId == netplay::menu::NetplayMenuId::Lobby);
    const bool isOptions = (state.menuId == netplay::menu::NetplayMenuId::Options);
    const bool isPlayerRooms = (state.menuId == netplay::menu::NetplayMenuId::PlayerRooms);
    if (!hasDynamicField && !isLobby && !isOptions && !isPlayerRooms)
    {
        return true;
    }

    netplay::draw::LockedMenuSurface lockedSurface;
    if (netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        bool drewText = false;
        const bool highResSurface = (lockedSurface.width >= 640 && lockedSurface.height >= 480);
        const int fontScaleX = 1;
        const int fontScaleY = 1;
        const int glyphHeight = 7 * fontScaleY;
        uint8_t colorSelected = state.paletteStart;
        uint8_t colorNormal = state.paletteStart;
        netplay::draw::ResolveOverlayTextPaletteColors(
            screenContext,
            state.paletteStart,
            state.paletteCount,
            &colorSelected,
            &colorNormal);

        if (entries != nullptr && count > 0)
        {
            for (int i = 0; i < count; ++i)
            {
                std::string value;
                bool drawAsLabel = false;
                if (isLobby || isOptions || isPlayerRooms)
                {
                    // For the lobby/options/player-rooms menus, rows get their label from
                    // buildRowLabel (player names, setting names, values, etc.).
                    if (!callbacks.buildRowLabel)
                    {
                        continue;
                    }
                    value = callbacks.buildRowLabel(entries[i]);
                }
                else
                {
                    if (entries[i].action == netplay::menu::NetplayMenuAction::OpenLobby)
                    {
                        value = callbacks.buildRowLabel(entries[i]);
                        drawAsLabel = true;
                    }
                    else if (!callbacks.getInlineEditDisplayValue(entries[i].action, &value, true))
                    {
                        continue;
                    }
                }

                const int slideY = callbacks.getScaledNativeSlideY(screenContext);
                const int rowTop = netplay::constants::kNetplayCompactMenuTopY + i * netplay::constants::kNetplayCompactMenuRowStep + slideY;
                const int rowBottom = rowTop + state.highlightHeight;
                const bool drawLeftAligned = isLobby || isOptions || drawAsLabel;
                const int leftBase = (isOptions || isPlayerRooms) ? kOptionsTextLeft : (isLobby ? 8 : (drawLeftAligned ? 152 : 182));
                const int leftX = highResSurface
                    ? leftBase
                    : MulDiv(leftBase, lockedSurface.width, 320);
                const int rightBase = (isOptions || isPlayerRooms) ? kOptionsTextRight : 314;
                const int rightX = highResSurface ? rightBase : MulDiv(rightBase, lockedSurface.width, 320);
                const int topY = highResSurface ? rowTop : MulDiv(rowTop, lockedSurface.height, 240);
                const int bottomY = highResSurface ? rowBottom : MulDiv(rowBottom, lockedSurface.height, 240);
                const int rowHeight = bottomY - topY;
                const int textY = topY + ((rowHeight > glyphHeight) ? ((rowHeight - glyphHeight) / 2) : 0);
                const uint8_t color = (i == selected) ? colorSelected : colorNormal;
                const netplay::font::IndexedSurfaceView surfaceView = {
                    lockedSurface.pixels,
                    lockedSurface.width,
                    lockedSurface.height,
                    lockedSurface.pitch,
                };

                if (isOptions || isPlayerRooms)
                {
                    DrawSplitRowText(
                        callbacks,
                        surfaceView,
                        entries[i],
                        leftX,
                        rightX - fontScaleX,
                        textY,
                        fontScaleX,
                        fontScaleY,
                        color);
                }
                else if (drawLeftAligned)
                {
                    netplay::font::DrawTextLeft5x7(
                        surfaceView,
                        value,
                        leftX,
                        rightX - fontScaleX,
                        textY,
                        fontScaleX,
                        fontScaleY,
                        color);
                }
                else
                {
                    netplay::font::DrawTextRight5x7(
                        surfaceView,
                        value,
                        leftX,
                        rightX - fontScaleX,
                        textY,
                        fontScaleX,
                        fontScaleY,
                        color);
                }
                drewText = true;
            }
        }

        netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
        if (drewText)
        {
            return true;
        }
    }

    // Surface lock failed - nothing we can do without flickering.
    return false;
}
}
