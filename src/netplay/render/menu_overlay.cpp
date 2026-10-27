#include "netplay/render/menu_overlay.h"

#include "netplay/core/constants.h"
#include "netplay/core/mod_settings.h"
#include "netplay/hooks/debug_overlay.h"
#include "netplay/render/draw_surface.h"
#include "netplay/render/software_font.h"

#include <algorithm>
#include <cstring>
#include <windows.h>

namespace netplay::render
{
namespace
{
constexpr int kOptionsTextLeft = 8;
constexpr int kOptionsTextRight = 314;

float AdvanceHighlightGlide(netplay::menu::NetplayMenuId menuId, float targetY)
{
    static int s_menuId = -1;
    static float s_animY = 0.0f;
    const int id = static_cast<int>(menuId);

    const float delta = targetY - s_animY;
    if (id != s_menuId || (delta < 0.75f && delta > -0.75f) || delta > 96.0f || delta < -96.0f)
    {
        s_menuId = id;
        s_animY = targetY;
        return s_animY;
    }
    s_animY += delta * 0.34f;
    return s_animY;
}

bool RtRowTextActive()
{
    return netplay::mod_settings::IsMenuTtfTextEnabled()
        && netplay::debug_overlay::IsRtTextAvailable();
}

constexpr uint32_t kRtRowSelectedColor = 0xFFFFFFFFu;
constexpr uint32_t kRtRowNormalColor = 0xFFD0D0D0u;
constexpr uint32_t kRtHeaderColor = 0xFF8CD0E8u;

int g_rowSlideOffsetX = 0;
int g_rowClipLeft = 0;
int g_rowClipRight = 0;

void SubmitRowRt(
    const std::string& text,
    int x0,
    int x1,
    int y,
    netplay::debug_overlay::RtTextAlign align,
    netplay::debug_overlay::RtTextProfile profile,
    uint32_t rgba)
{
    netplay::debug_overlay::RtTextItem item;
    item.x0 = static_cast<int16_t>(x0 + g_rowSlideOffsetX);
    item.x1 = static_cast<int16_t>(x1 + g_rowSlideOffsetX);
    item.y = static_cast<int16_t>(y);
    item.align = align;
    item.profile = profile;
    item.rgba = rgba;
    if (g_rowClipRight > g_rowClipLeft)
    {
        item.clipX0 = static_cast<int16_t>(g_rowClipLeft);
        item.clipX1 = static_cast<int16_t>(g_rowClipRight);
    }
    netplay::debug_overlay::SetRtText(item, text.data(), text.size());
    netplay::debug_overlay::SubmitRtText(item);
}

std::string TrimToRtWidth(
    netplay::debug_overlay::RtTextProfile profile,
    const std::string& text,
    int availableWidth)
{
    namespace ov = netplay::debug_overlay;
    std::string trimmed = text;
    while (!trimmed.empty()
        && ov::MeasureRtTextWidth(profile, trimmed.c_str()) > availableWidth)
    {
        while (!trimmed.empty()
            && (static_cast<unsigned char>(trimmed.back()) & 0xC0u) == 0x80u)
        {
            trimmed.pop_back();
        }
        if (!trimmed.empty())
        {
            trimmed.pop_back();
        }
    }
    return trimmed;
}

bool SubmitSplitRowRt(
    const std::string& primary,
    const std::string& secondary,
    int leftX,
    int rightX,
    int y,
    bool selected,
    bool header)
{
    if (!RtRowTextActive())
    {
        return false;
    }

    namespace ov = netplay::debug_overlay;
    if (header)
    {
        SubmitRowRt(
            TrimToRtWidth(ov::RtTextProfile::MenuSection, primary, rightX - leftX),
            leftX, rightX, y, ov::RtTextAlign::Left,
            ov::RtTextProfile::MenuSection, kRtHeaderColor);
        return true;
    }

    const uint32_t rgba = selected ? kRtRowSelectedColor : kRtRowNormalColor;
    if (secondary.empty())
    {
        SubmitRowRt(
            TrimToRtWidth(ov::RtTextProfile::MenuRow, primary, rightX - leftX),
            leftX, rightX, y, ov::RtTextAlign::Left,
            ov::RtTextProfile::MenuRow, rgba);
        return true;
    }

    constexpr int kColumnGap = 8;
    constexpr int kMinPrimaryWidth = 86;
    const int secondaryWidth =
        ov::MeasureRtTextWidth(ov::RtTextProfile::MenuRow, secondary.c_str());
    const int secondaryLeft =
        (std::max)(leftX + kMinPrimaryWidth, rightX - (std::max)(secondaryWidth, 0));
    const int primaryRight = (std::max)(leftX, secondaryLeft - kColumnGap);

    SubmitRowRt(
        TrimToRtWidth(ov::RtTextProfile::MenuRow, primary, primaryRight - leftX),
        leftX, primaryRight, y, ov::RtTextAlign::Left,
        ov::RtTextProfile::MenuRow, rgba);
    SubmitRowRt(
        TrimToRtWidth(ov::RtTextProfile::MenuRow, secondary, rightX - secondaryLeft),
        secondaryLeft, rightX, y, ov::RtTextAlign::Right,
        ov::RtTextProfile::MenuRow, rgba);
    return true;
}

void DrawSplitRowText(
    const OverlayCallbacks& callbacks,
    const netplay::font::IndexedSurfaceView& surface,
    const netplay::menu::NetplayMenuEntry& entry,
    int leftX,
    int rightX,
    int y,
    int scaleX,
    int scaleY,
    uint8_t color,
    bool tryRtText,
    bool selected)
{
    const bool isHeader = callbacks.isHeaderRow && callbacks.isHeaderRow(entry);
    const std::string primary =
        callbacks.buildRowPrimaryText
        ? callbacks.buildRowPrimaryText(entry)
        : callbacks.buildRowLabel(entry);
    const std::string secondary =
        isHeader
            ? std::string()
            : (callbacks.buildRowSecondaryText
                ? callbacks.buildRowSecondaryText(entry)
                : std::string());

    if (tryRtText
        && SubmitSplitRowRt(primary, secondary, leftX, rightX, y, selected, isHeader))
    {
        return;
    }

    auto slideX = [](int v) -> int
    {
        v += g_rowSlideOffsetX;
        if (g_rowClipRight > g_rowClipLeft)
        {
            if (v < g_rowClipLeft) { v = g_rowClipLeft; }
            if (v > g_rowClipRight) { v = g_rowClipRight; }
        }
        return v;
    };

    if (secondary.empty())
    {
        netplay::font::DrawTextLeft5x7(surface, primary, slideX(leftX), slideX(rightX), y, scaleX, scaleY, color);
        return;
    }

    constexpr int kColumnGap = 8;
    constexpr int kMinPrimaryWidth = 86;
    const int secondaryWidth = netplay::font::MeasureText5x7Width(secondary, scaleX);
    const int secondaryLeft = (std::max)(leftX + kMinPrimaryWidth, rightX - secondaryWidth);
    const int primaryRight = (std::max)(leftX, secondaryLeft - kColumnGap);

    netplay::font::DrawTextLeft5x7(surface, primary, slideX(leftX), slideX(primaryRight), y, scaleX, scaleY, color);
    netplay::font::DrawTextRight5x7(surface, secondary, slideX(secondaryLeft), slideX(rightX), y, scaleX, scaleY, color);
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

    const uint8_t headerColor = netplay::draw::ResolveBestPaletteColor(screenContext, 32, 32, 32);
    const uint8_t selectedTextColor = netplay::draw::ResolveBestPaletteColor(screenContext, 8, 8, 8);
    const uint8_t normalTextColor = netplay::draw::ResolveBestPaletteColor(screenContext, 108, 108, 108);
    const uint8_t highlightColor = netplay::draw::ResolveBestPaletteColor(screenContext, 110, 225, 214);

    const bool wideDynamicMenu =
        state.menuId == netplay::menu::NetplayMenuId::Options
        || state.menuId == netplay::menu::NetplayMenuId::PlayerRooms
        || state.menuId == netplay::menu::NetplayMenuId::Lobby;
    const int panelLeft = wideDynamicMenu ? kOptionsTextLeft : 150;
    const int panelRight = wideDynamicMenu ? kOptionsTextRight : 314;
    constexpr int rowHeight = netplay::constants::kNetplayDefaultHighlightHeight;
    constexpr int rowStep = netplay::constants::kNetplayCompactMenuRowStep;

    const std::string header = callbacks.buildMenuHeaderText();
    netplay::font::DrawTextLeft5x7(sv, header, panelLeft, panelRight, 18, 1, 1, headerColor);

    const int selection =
        callbacks.clampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection)));
    int count = 0;
    const netplay::menu::NetplayMenuEntry* entries = callbacks.getMenuEntries(state.menuId, &count);
    if (entries != nullptr && count > 0)
    {
        constexpr int panelTop = netplay::constants::kNetplayCompactMenuTopY;

        const int slideX = state.contentSlideOffsetX;
        g_rowSlideOffsetX = slideX;
        g_rowClipLeft = slideX != 0 ? panelLeft : 0;
        g_rowClipRight = slideX != 0 ? panelRight : 0;

        if (selection >= 0 && selection < count)
        {
            const float targetY = static_cast<float>(panelTop + selection * rowStep);
            const float glidedY = AdvanceHighlightGlide(state.menuId, targetY);
            const int hlY = (slideX != 0)
                ? static_cast<int>(targetY)
                : static_cast<int>(glidedY + 0.5f);
            int hlLeft = panelLeft + slideX;
            int hlRight = panelRight + slideX;
            if (hlLeft < panelLeft) { hlLeft = panelLeft; }
            if (hlRight > panelRight) { hlRight = panelRight; }
            if (hlRight > hlLeft)
            {
                netplay::font::FillIndexedSurfaceRect(sv, hlLeft, hlY, hlRight - hlLeft, rowHeight, highlightColor);
            }
        }

        for (int i = 0; i < count; ++i)
        {
            const int rowY = panelTop + i * rowStep;
            const bool isSelected = (i == selection);

            const int textY = rowY + (rowHeight - 7) / 2; // center 5x7 glyph in row
            const uint8_t color = isSelected ? selectedTextColor : normalTextColor;
            if (wideDynamicMenu)
            {
                DrawSplitRowText(
                    callbacks, sv, entries[i], panelLeft + 2, panelRight - 2, textY, 1, 1, color,
                    true,
                    isSelected);
            }
            else
            {
                const std::string label = callbacks.buildRowLabel(entries[i]);
                netplay::font::DrawTextLeft5x7(sv, label, panelLeft + 2, panelRight - 2, textY, 1, 1, color);
            }
        }

        g_rowSlideOffsetX = 0;
        g_rowClipLeft = 0;
        g_rowClipRight = 0;
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
    bool mainHasBadge = false;
    if (state.menuId == netplay::menu::NetplayMenuId::Main && callbacks.buildRowBadgeText
        && entries != nullptr)
    {
        for (int i = 0; i < count; ++i)
        {
            if (!callbacks.buildRowBadgeText(entries[i]).empty())
            {
                mainHasBadge = true;
                break;
            }
        }
    }
    if (state.menuId == netplay::menu::NetplayMenuId::Main && !mainHasBadge)
    {
        return true;
    }
    const bool isLobby = (state.menuId == netplay::menu::NetplayMenuId::Lobby);
    const bool isOptions = (state.menuId == netplay::menu::NetplayMenuId::Options);
    const bool isPlayerRooms = (state.menuId == netplay::menu::NetplayMenuId::PlayerRooms);
    if (!hasDynamicField && !isLobby && !isOptions && !isPlayerRooms && !mainHasBadge)
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

        const int slideX = isOptions ? state.contentSlideOffsetX : 0;
        g_rowSlideOffsetX = slideX;
        g_rowClipLeft = slideX != 0 ? kOptionsTextLeft : 0;
        g_rowClipRight = slideX != 0 ? kOptionsTextRight : 0;

        if (entries != nullptr && count > 0)
        {
            for (int i = 0; i < count; ++i)
            {
                std::string value;
                bool drawAsLabel = false;
                if (isLobby || isOptions || isPlayerRooms)
                {
                    if (!callbacks.buildRowLabel)
                    {
                        continue;
                    }
                    value = callbacks.buildRowLabel(entries[i]);
                }
                else
                {
                    if (state.menuId == netplay::menu::NetplayMenuId::Main)
                    {
                        if (!callbacks.buildRowBadgeText)
                        {
                            continue;
                        }
                        value = callbacks.buildRowBadgeText(entries[i]);
                        if (value.empty())
                        {
                            continue;
                        }
                    }
                    else if (entries[i].action == netplay::menu::NetplayMenuAction::OpenLobby)
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

                if (isOptions || isPlayerRooms || isLobby)
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
                        color,
                        true,
                        i == selected);
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

        g_rowSlideOffsetX = 0;
        g_rowClipLeft = 0;
        g_rowClipRight = 0;

        netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
        if (drewText)
        {
            return true;
        }
    }

    return false;
}
}
