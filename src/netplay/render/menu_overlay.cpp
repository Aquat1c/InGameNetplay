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

// Selection-highlight glide. The native EFZ menus ease the cursor between
// rows instead of snapping; our overlay draws its own highlight bar, so we
// reproduce that here by easing the bar's Y toward the selected row. Text
// stays at its fixed row so nothing needs clipping - only the bar moves.
// Per menu id so switching menus (or entering a submenu) snaps cleanly.
float AdvanceHighlightGlide(netplay::menu::NetplayMenuId menuId, float targetY)
{
    static int s_menuId = -1;
    static float s_animY = 0.0f;
    const int id = static_cast<int>(menuId);

    const float delta = targetY - s_animY;
    if (id != s_menuId || (delta < 0.75f && delta > -0.75f) || delta > 96.0f || delta < -96.0f)
    {
        // New menu, already settled, or a jump too large to read as a glide
        // (e.g. a page scroll) - snap.
        s_menuId = id;
        s_animY = targetY;
        return s_animY;
    }
    // Exponential ease: ~5-6 frames at 60fps, matching the native feel.
    s_animY += delta * 0.34f;
    return s_animY;
}

// The options/settings rows draw through the game-RT TTF layer when it is
// live (same submit-first bootstrap as the footer/battle log); the 5x7 path
// below stays as the per-frame fallback.
bool RtRowTextActive()
{
    return netplay::mod_settings::IsMenuTtfTextEnabled()
        && netplay::debug_overlay::IsRtTextAvailable();
}

// Row text colors (IM_COL32 packing).
constexpr uint32_t kRtRowSelectedColor = 0xFFFFFFFFu;
constexpr uint32_t kRtRowNormalColor = 0xFFD0D0D0u;
// Section headers: muted warm gold, matching the lobby's playing-pair accent.
constexpr uint32_t kRtHeaderColor = 0xFF8CD0E8u;

// Category drill in/out slide, set by DrawRuntimeTextOverlayGdi for the rows it
// draws and read by SubmitRowRt (RT) + DrawSplitRowText's 5x7 path. When
// g_rowSlideOffsetX is 0 these are inert and rendering is the plain path.
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
    const size_t bytes = (std::min)(text.size(), sizeof(item.text) - 1);
    std::memcpy(item.text, text.data(), bytes);
    item.text[bytes] = '\0';
    netplay::debug_overlay::SubmitRtText(item);
}

// Trim to the field width using TTF metrics (the RT layer has no right-edge
// clipping), popping whole UTF-8 code points.
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

// Split-row (label left, value right) via the RT layer.  Returns false when
// the caller should draw the 5x7 fallback instead.
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

    // 5x7 fallback: apply the same slide offset + panel clamp as the RT path.
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

    // Resolve palette colors
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

        // Category drill in/out slide: shift the rows + highlight horizontally
        // and clip them to the panel. Rows are drawn via SubmitRowRt / the 5x7
        // path which read these globals; reset to 0 after the loop so other
        // menus (and the footer) are unaffected.
        const int slideX = state.contentSlideOffsetX;
        g_rowSlideOffsetX = slideX;
        g_rowClipLeft = slideX != 0 ? panelLeft : 0;
        g_rowClipRight = slideX != 0 ? panelRight : 0;

        // Draw the highlight once. Normally its Y glides toward the selected
        // row (native cursor feel); during a category slide it snaps (the row
        // content is what moves) and shifts/clips with the rows.
        if (selection >= 0 && selection < count)
        {
            const float targetY = static_cast<float>(panelTop + selection * rowStep);
            // Always advance the glide so its state stays in sync; during a
            // slide the bar snaps (the rows are what move) but the glide keeps
            // converging so it doesn't jump when the slide ends.
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
                // Options, Player Rooms and the Lobby all draw crisp TTF rows
                // (MenuRow profile) with the 5x7 fallback.
                DrawSplitRowText(
                    callbacks, sv, entries[i], panelLeft + 2, panelRight - 2, textY, 1, 1, color,
                    !state.suppressRtText,
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
    // Main menu labels (HOST, JOIN, LOBBY, etc.) are baked into the sprite sheet,
    // so the only dynamic text there is an optional right-edge badge (the
    // update-available "[!]" on OPTIONS).
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
    // Lobby rows all carry dynamic GDI labels (player names, playing pair, Back).
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

        // Category drill in/out slide for the options rows (this is the render
        // path actually used when the sprite font is loaded). SubmitRowRt / the
        // 5x7 path read these; reset to 0 after the loop.
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
                    if (state.menuId == netplay::menu::NetplayMenuId::Main)
                    {
                        // Sprite-sheet rows (labels baked in, LOBBY included):
                        // only the optional badge is dynamic; it renders
                        // right-aligned like the inline edit values.
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
                    // Options, Player Rooms and the Lobby all render their rows
                    // through the crisp TTF layer (MenuRow profile) with the 5x7
                    // fallback - lobby rows are single labels (empty secondary).
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
                        !state.suppressRtText,
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

    // Surface lock failed - nothing we can do without flickering.
    return false;
}
}
