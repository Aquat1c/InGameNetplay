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
    bool allowWindowDc)
{
    if (!state.useRuntimeTextOverlay || !state.netplayActive || !state.enableGdiFallbackOverlay)
    {
        return false;
    }
    if (!callbacks.clampSelectionToCurrentMenu || !callbacks.getMenuEntries || !callbacks.buildMenuHeaderText
        || !callbacks.buildRowLabel || !callbacks.buildFooterText || !callbacks.getMenuOverlayFont)
    {
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
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, callbacks.getMenuOverlayFont());
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

    const int panelLeft = scaleX(150);
    const int panelTop = scaleY(78);
    const int panelRight = scaleX(314);
    const int rowHeight = (std::max)(16, scaleY(16));
    const int rowStep = (std::max)(rowHeight + 2, scaleY(21));

    RECT headerRect = {panelLeft, scaleY(56), panelRight, scaleY(74)};
    SetTextColor(dc, RGB(32, 32, 32));
    const std::string header = callbacks.buildMenuHeaderText();
    DrawTextA(dc, header.c_str(), -1, &headerRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    const int selection =
        callbacks.clampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection)));
    int count = 0;
    const netplay::menu::NetplayMenuEntry* entries = callbacks.getMenuEntries(state.menuId, &count);
    if (entries != nullptr && count > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            RECT rowRect = {panelLeft, panelTop + i * rowStep, panelRight, panelTop + i * rowStep + rowHeight};
            const bool isSelected = (i == selection);
            if (isSelected)
            {
                HBRUSH highlightBrush = CreateSolidBrush(RGB(110, 225, 214));
                FillRect(dc, &rowRect, highlightBrush);
                DeleteObject(highlightBrush);
            }

            const std::string label = callbacks.buildRowLabel(entries[i]);
            COLORREF textColor = isSelected ? RGB(8, 8, 8) : RGB(108, 108, 108);
            if (callbacks.getRowTextColor)
            {
                const auto overrideColor = callbacks.getRowTextColor(entries[i], isSelected);
                if (overrideColor.has_value())
                {
                    textColor = overrideColor.value();
                }
            }
            SetTextColor(dc, textColor);
            DrawTextA(dc, label.c_str(), -1, &rowRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    RECT footerRect = {panelLeft, scaleY(224), panelRight, scaleY(238)};
    SetTextColor(dc, RGB(90, 90, 90));
    const std::string footer = callbacks.buildFooterText();
    DrawTextA(dc, footer.c_str(), -1, &footerRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (oldFont != nullptr)
    {
        SelectObject(dc, oldFont);
    }
    netplay::draw::ReleaseMenuDrawDc(dc, surface, window);
    return true;
}

bool DrawDynamicFieldValuesGdi(
    const OverlayCallbacks& callbacks,
    const DynamicFieldOverlayState& state,
    uint32_t screenContext,
    bool allowWindowDc)
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
            if (callbacks.isInlineEditableAction(entries[i].action))
            {
                hasDynamicField = true;
                break;
            }
        }
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
                if (isLobby)
                {
                    // For the lobby, every row gets a centered GDI label from
                    // buildRowLabel (player name, "--", "vs", "BACK", etc.).
                    if (!callbacks.buildRowLabel)
                    {
                        continue;
                    }
                    value = callbacks.buildRowLabel(entries[i]);
                }
                else
                {
                    if (!callbacks.getInlineEditDisplayValue(entries[i].action, &value, true))
                    {
                        continue;
                    }
                }

                const int slideY = callbacks.getScaledNativeSlideY(screenContext);
                const int rowTop = netplay::constants::kNetplayCompactMenuTopY + i * netplay::constants::kNetplayCompactMenuRowStep + slideY;
                const int rowBottom = rowTop + state.highlightHeight;
                const int leftX = highResSurface ? (isLobby ? 8 : 182) : MulDiv(isLobby ? 8 : 182, lockedSurface.width, 320);
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

                if (isLobby)
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

    HDC dc = nullptr;
    void* surface = nullptr;
    HWND window = nullptr;
    if (!netplay::draw::AcquireMenuDrawDc(screenContext, &dc, &surface, &window, allowWindowDc))
    {
        return false;
    }

    const bool useWindowDc = (window != nullptr);
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
    HGDIOBJ oldFont = SelectObject(dc, callbacks.getMenuOverlayFont());

    if (entries != nullptr && count > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            std::string value;
            DWORD textFlags = DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
            if (isLobby)
            {
                if (!callbacks.buildRowLabel)
                {
                    continue;
                }
                value = callbacks.buildRowLabel(entries[i]);
                textFlags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
            }
            else
            {
                if (!callbacks.getInlineEditDisplayValue(entries[i].action, &value, true))
                {
                    continue;
                }
            }

            const int slideY = callbacks.getScaledNativeSlideY(screenContext);
            RECT rowRect = {
                scaleX(isLobby ? 8 : 182),
                scaleY(netplay::constants::kNetplayCompactMenuTopY + i * netplay::constants::kNetplayCompactMenuRowStep + slideY),
                scaleX(314),
                scaleY(netplay::constants::kNetplayCompactMenuTopY + i * netplay::constants::kNetplayCompactMenuRowStep + state.highlightHeight + slideY),
            };

            COLORREF textColor = (i == selected) ? RGB(8, 8, 8) : RGB(186, 186, 186);
            if (!isLobby)
            {
                textColor = (i == selected) ? RGB(255, 255, 255) : RGB(186, 186, 186);
            }
            else if (callbacks.getRowTextColor)
            {
                const auto overrideColor = callbacks.getRowTextColor(entries[i], i == selected);
                if (overrideColor.has_value())
                {
                    textColor = overrideColor.value();
                }
            }
            SetTextColor(dc, textColor);
            DrawTextA(dc, value.c_str(), -1, &rowRect, textFlags);
        }
    }

    if (oldFont != nullptr)
    {
        SelectObject(dc, oldFont);
    }
    netplay::draw::ReleaseMenuDrawDc(dc, surface, window);
    return true;
}
}


