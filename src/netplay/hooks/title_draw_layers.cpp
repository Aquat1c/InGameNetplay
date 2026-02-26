#include "netplay/hooks/internal/shared.h"

#include <cctype>
#include <string>

namespace netplay::hooks::internal
{
using namespace netplay::constants;
using SpriteGlyph = netplay::fontmap::SpriteGlyph;
using NetplayObRow = netplay::menu::NetplayObRow;
using NetplayMenuAction = netplay::menu::NetplayMenuAction;
using NetplayMenuEntry = netplay::menu::NetplayMenuEntry;
using NetplayMenuId = netplay::menu::NetplayMenuId;
using netplay::menu::GetMenuEntries;
using netplay::menu::RowToIndex;

void DrawSpriteText(
    uint32_t screenContext,
    int startX,
    int startY,
    const std::string& text,
    int maxWidth,
    bool upperCaseText)
{
    if (!g_spriteFont.loaded)
    {
        return;
    }

    auto const blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));
    auto* const graphicsContext = GetGraphicsContext(screenContext);
    const int objectsSurface = *reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface);
    const char transparentColor = static_cast<char>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor));

    int x = startX;
    int y = startY;
    for (char raw : text)
    {
        char c = raw;
        if (g_spriteFont.uppercaseInput || upperCaseText)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        if (c == '\n')
        {
            x = startX;
            y += g_spriteFont.lineHeight;
            continue;
        }

        if (c == ' ')
        {
            x += g_spriteFont.lineHeight / 2;
            continue;
        }

        auto it = g_spriteFont.glyphs.find(c);
        if (it == g_spriteFont.glyphs.end())
        {
            x += g_spriteFont.lineHeight / 2;
            continue;
        }

        const SpriteGlyph& glyph = it->second;
        if (maxWidth > 0 && x + glyph.width > startX + maxWidth)
        {
            break;
        }

        (void)blit(
            graphicsContext,
            x,
            y,
            x + glyph.width,
            y + glyph.height,
            objectsSurface,
            glyph.srcX,
            glyph.srcY,
            glyph.srcX + glyph.width,
            glyph.srcY + glyph.height,
            transparentColor,
            0);
        x += glyph.advance + g_spriteFont.letterSpacing;
    }
}

void DrawRuntimeSpriteOverlay(uint32_t screenContext)
{
    if (!g_useRuntimeTextOverlay || !g_netplayMenuState.active || !g_spriteFont.loaded)
    {
        return;
    }

    const int panelLeft = 152;
    const int panelRight = 316;
    const int titleY = 58;
    const int rowTextOffsetY = 1;

    DrawSpriteText(screenContext, panelLeft, titleY, BuildMenuHeaderText(), panelRight - panelLeft, true);

    const int selection = ClampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
    int count = 0;
    const NetplayMenuEntry* entries = GetMenuEntries(g_netplayMenuState.menuId, &count);
    if (entries != nullptr && count > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            const int rowIndex = entries[i].renderRow;
            if (rowIndex < 0 || rowIndex >= kNetplayConfigOptionCount)
            {
                continue;
            }

            // Main menu entries already have correct labels baked into the
            // sprite sheet; drawing sprite-font text on top would double-render.
            if (g_netplayMenuState.menuId == NetplayMenuId::Main)
            {
                continue;
            }

            // Lobby player/playing slots display arbitrary network strings that may
            // not exist in the sprite sheet -- skip here and let GDI handle them.
            const bool isLobbyNicknameRow =
                g_netplayMenuState.menuId == NetplayMenuId::Lobby
                && entries[i].action >= NetplayMenuAction::LobbySlot0
                && entries[i].action <= NetplayMenuAction::LobbyPlaying0;
            if (isLobbyNicknameRow)
            {
                continue;
            }

            const int y = g_netplayMenuState.renderLayout.highlightDestY[static_cast<size_t>(rowIndex)] + rowTextOffsetY;
            DrawSpriteText(screenContext, panelLeft, y, BuildRowLabel(entries[i]), panelRight - panelLeft, false);
        }
    }

    std::string footer = BuildFooterText();
    if (!g_inlineEditState.active && selection >= 0 && selection < count)
    {
        const NetplayMenuEntry& selected = entries[selection];
        if (selected.action == NetplayMenuAction::HostEditPort)
        {
            footer = "CONFIRM TO EDIT HOST PORT";
        }
        else if (selected.action == NetplayMenuAction::JoinEditAddress)
        {
            footer = "CONFIRM TO EDIT SERVER ADDRESS";
        }
        else if (selected.action == NetplayMenuAction::JoinEditPort)
        {
            footer = "CONFIRM TO EDIT SERVER PORT";
        }
        else if (selected.action == NetplayMenuAction::NicknameEdit)
        {
            footer = "CONFIRM TO EDIT NICKNAME";
        }
    }
    DrawSpriteText(screenContext, panelLeft, 224, footer, panelRight - panelLeft, false);
}

void BlitMenuRowClipped(
    BlitSurfaceWithTransparencyFn blit,
    void** graphicsContext,
    int objectsSurface,
    char transparentColor,
    int srcX,
    int srcY,
    int width,
    int height,
    int destX,
    int destY)
{
    int clippedLeft = destX;
    int clippedRight = destX + width;
    if (clippedLeft < 0)
    {
        clippedLeft = 0;
    }
    if (clippedRight > 320)
    {
        clippedRight = 320;
    }
    if (clippedRight <= clippedLeft || height <= 0)
    {
        return;
    }

    const int clippedWidth = clippedRight - clippedLeft;
    const int srcLeft = srcX + (clippedLeft - destX);
    const int srcRight = srcLeft + clippedWidth;
    (void)blit(
        graphicsContext,
        clippedLeft,
        destY,
        clippedRight,
        destY + height,
        objectsSurface,
        srcLeft,
        srcY,
        srcRight,
        srcY + height,
        transparentColor,
        0);
}

void DrawCompactMenuRows(
    uint32_t screenContext,
    NetplayMenuId menuId,
    int logicalSelection,
    int offsetX)
{
    auto const blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));
    auto* const graphicsContext = GetGraphicsContext(screenContext);
    const int objectsSurface = *reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface);
    const char transparentColor = static_cast<char>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor));

    int count = 0;
    const NetplayMenuEntry* entries = GetMenuEntries(menuId, &count);
    if (entries == nullptr || count <= 0)
    {
        return;
    }

    const int clampedSelection = (logicalSelection < 0) ? 0 : ((logicalSelection >= count) ? (count - 1) : logicalSelection);
    const int height = g_netplayMenuState.renderLayout.highlightHeight;
    const int srcX = g_netplayMenuState.renderLayout.highlightSourceX;
    const int widthDefault = 320;
    const int slideY = GetScaledNativeSlideY(screenContext);

    for (int i = 0; i < count; ++i)
    {
        const int rowIndex = entries[i].renderRow;
        if (rowIndex < 0 || rowIndex >= kNetplayConfigOptionCount)
        {
            continue;
        }

        const bool isSelected = (i == clampedSelection);
        const int srcY = isSelected
            ? g_netplayMenuState.renderLayout.highlightSourceY[static_cast<size_t>(rowIndex)]
            : g_netplayMenuState.renderLayout.highlightDestY[static_cast<size_t>(rowIndex)];
        const int width = g_netplayMenuState.renderLayout.highlightWidth[static_cast<size_t>(rowIndex)] > 0
            ? g_netplayMenuState.renderLayout.highlightWidth[static_cast<size_t>(rowIndex)]
            : widthDefault;
        const int destY = kNetplayCompactMenuTopY + i * kNetplayCompactMenuRowStep + slideY;

        BlitMenuRowClipped(
            blit,
            graphicsContext,
            objectsSurface,
            transparentColor,
            srcX,
            srcY,
            width,
            height,
            offsetX,
            destY);
    }
}

void DrawCompactMenuTitle(uint32_t screenContext)
{
    auto const blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));
    auto* const graphicsContext = GetGraphicsContext(screenContext);
    const int objectsSurface = *reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface);
    const char transparentColor = static_cast<char>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor));
    constexpr int titleY = 0;
    BlitMenuRowClipped(
        blit,
        graphicsContext,
        objectsSurface,
        transparentColor,
        0,
        0,
        320,
        14,
        0,
        titleY);
}

bool DrawDynamicFieldValuesGdi(uint32_t screenContext, bool allowWindowDc)
{
    const netplay::render::DynamicFieldOverlayState state = {
        g_netplayMenuState.active,
        g_useRuntimeTextOverlay,
        IsMenuSlideTransitionActive(),
        g_netplayMenuState.menuId,
        g_netplayMenuState.renderLayout.highlightHeight,
        g_netplayMenuState.paletteStart,
        g_netplayMenuState.paletteCount,
    };
    return netplay::render::DrawDynamicFieldValuesGdi(GetOverlayCallbacks(), state, screenContext, allowWindowDc);
}

void DrawAnimatedCompactMenuLayer(uint32_t screenContext)
{
    auto const blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));
    auto* const graphicsContext = GetGraphicsContext(screenContext);
    const int backgroundSurface = *reinterpret_cast<int*>(screenContext + kOffsetBackgroundSurface);

    (void)blit(graphicsContext, 0, 0, 320, 240, backgroundSurface, 0, 0, 320, 240, 0, 0);
    DrawCompactMenuTitle(screenContext);
    const int logicalSelection = ClampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
    DrawCompactMenuRows(screenContext, g_netplayMenuState.menuId, logicalSelection, 0);
}

void DrawNetplayBaseLayer(uint32_t screenContext)
{
    auto const blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));

    auto* const graphicsContext = GetGraphicsContext(screenContext);
    const int backgroundSurface = *reinterpret_cast<int*>(screenContext + kOffsetBackgroundSurface);
    const int objectsSurface = *reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface);
    const char transparentColor = static_cast<char>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor));

    (void)blit(graphicsContext, 0, 0, 320, 240, backgroundSurface, 0, 0, 320, 240, 0, 0);
    (void)blit(graphicsContext, 0, 0, 320, 240, objectsSurface, 0, 0, 320, 240, transparentColor, 0);

    {
        // Always stamp unused sprite-sheet rows with a blank black bar so
        // labels from other menus (ADDRESS, PORT, etc.) don't bleed through.
        // Reserved6 (row 6) is intentionally blank in the sheet; never use
        // Reserved5 here because that row carries the "LOBBY" label.
        constexpr int kBlankRowIndex = RowToIndex(NetplayObRow::Reserved6);
        if (kBlankRowIndex >= 0 && kBlankRowIndex < kNetplayConfigOptionCount)
        {
            const int blankSrcY = g_netplayMenuState.renderLayout.highlightDestY[static_cast<size_t>(kBlankRowIndex)];
            const int blankSrcX = g_netplayMenuState.renderLayout.highlightDestX;
            const int blankHeight = g_netplayMenuState.renderLayout.highlightHeight;
            const int blankWidth = g_netplayMenuState.renderLayout.highlightWidth[static_cast<size_t>(kBlankRowIndex)];

            for (int row = 0; row < kNetplayConfigOptionCount; ++row)
            {
                if (IsRenderRowUsedByMenu(g_netplayMenuState.menuId, row))
                {
                    continue;
                }

                const int dstY = g_netplayMenuState.renderLayout.highlightDestY[static_cast<size_t>(row)];
                const int dstX = g_netplayMenuState.renderLayout.highlightDestX;
                const int rowWidth = g_netplayMenuState.renderLayout.highlightWidth[static_cast<size_t>(row)];
                const int drawWidth = (rowWidth < blankWidth) ? rowWidth : blankWidth;

                (void)blit(
                    graphicsContext,
                    dstX,
                    dstY,
                    dstX + drawWidth,
                    dstY + blankHeight,
                    objectsSurface,
                    blankSrcX,
                    blankSrcY,
                    blankSrcX + drawWidth,
                    blankSrcY + blankHeight,
                    transparentColor,
                    0);
            }
        }
    }

    const int logicalSelection = ClampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
    const int rowIndex = GetRenderRowForSelection(logicalSelection);
    if (rowIndex >= 0 && rowIndex < static_cast<int>(g_netplayMenuState.renderLayout.highlightSourceY.size()))
    {
        const int srcY = g_netplayMenuState.renderLayout.highlightSourceY[rowIndex];
        const int dstY = g_netplayMenuState.renderLayout.highlightDestY[rowIndex];
        const int width = g_netplayMenuState.renderLayout.highlightWidth[rowIndex];
        const int srcX = g_netplayMenuState.renderLayout.highlightSourceX;
        const int dstX = g_netplayMenuState.renderLayout.highlightDestX;
        const int height = g_netplayMenuState.renderLayout.highlightHeight;
        (void)blit(
            graphicsContext,
            dstX,
            dstY,
            dstX + width,
            dstY + height,
            objectsSurface,
            srcX,
            srcY,
            srcX + width,
            srcY + height,
            transparentColor,
            0);
    }
}

BOOL RenderNetplayMenuRuntimeText(uint32_t screenContext)
{
    auto const present = reinterpret_cast<PresentFrameToScreenFn>(RuntimeAddress(kVaPresentFrameToScreen));
    DrawNetplayBaseLayer(screenContext);
    DrawRuntimeSpriteOverlay(screenContext);
    // All overlays now draw via surface lock + pixel writes before present,
    // so there is no post-present window DC fallback needed.
    const bool forceGdi = (g_netplayMenuState.menuId == NetplayMenuId::Lobby);
    if (!g_spriteFont.loaded || forceGdi)
    {
        (void)DrawRuntimeTextOverlayGdi(screenContext, false);
    }
    (void)DrawDelaySetupOverlayGdi(screenContext, false);
    (void)DrawSpectateConfirmOverlayGdi(screenContext, false);
    (void)DrawDebugOverlay(screenContext);
    return present(*reinterpret_cast<int*>(screenContext + kOffsetGraphicsContext));
}

BOOL RenderNetplayMenuConfigStyle(uint32_t screenContext)
{
    auto const present = reinterpret_cast<PresentFrameToScreenFn>(RuntimeAddress(kVaPresentFrameToScreen));
    DrawAnimatedCompactMenuLayer(screenContext);
    (void)DrawDynamicFieldValuesGdi(screenContext, false);
    (void)DrawDelaySetupOverlayGdi(screenContext, false);
    (void)DrawSpectateConfirmOverlayGdi(screenContext, false);
    (void)DrawDebugOverlay(screenContext);
    return present(*reinterpret_cast<int*>(screenContext + kOffsetGraphicsContext));
}
} // namespace netplay::hooks::internal
