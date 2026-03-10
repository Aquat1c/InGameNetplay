#include "netplay/render/menu_overlay.h"

#include "netplay/core/constants.h"
#include "netplay/render/draw_surface.h"
#include "netplay/render/software_font.h"

#include <algorithm>
#include <windows.h>

namespace netplay::render
{
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

    constexpr int panelLeft = 150;
    constexpr int panelRight = 314;
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

            const std::string label = callbacks.buildRowLabel(entries[i]);
            const int textY = rowY + (rowHeight - 7) / 2; // center 5x7 glyph in row
            const uint8_t color = isSelected ? selectedTextColor : normalTextColor;
            netplay::font::DrawTextLeft5x7(sv, label, panelLeft + 2, panelRight - 2, textY, 1, 1, color);
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
    if (!hasDynamicField && !isLobby)
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
                if (isLobby)
                {
                    // BackToMain uses the ReturnToTitle sprite row which already
                    // has "RETURN TO TITLE" baked into the sprite sheet — skip it.
                    if (entries[i].action == netplay::menu::NetplayMenuAction::BackToMain)
                    {
                        continue;
                    }
                    // For the lobby, every other row gets a centered label from
                    // buildRowLabel (player name, "vs", etc.).
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
                const int leftX = highResSurface
                    ? (isLobby ? 8 : (drawAsLabel ? 152 : 182))
                    : MulDiv(isLobby ? 8 : (drawAsLabel ? 152 : 182), lockedSurface.width, 320);
                const int rightX = highResSurface ? 314 : MulDiv(314, lockedSurface.width, 320);
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

                if (isLobby || drawAsLabel)
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

    // Surface lock failed — nothing we can do without flickering.
    return false;
}
}
