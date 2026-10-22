#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <windows.h>

#include "netplay/core/menu_model.h"

namespace netplay::render
{
struct OverlayCallbacks
{
    std::function<int(int)> clampSelectionToCurrentMenu;
    std::function<const netplay::menu::NetplayMenuEntry*(netplay::menu::NetplayMenuId, int*)> getMenuEntries;
    std::function<std::string()> buildMenuHeaderText;
    std::function<std::string(const netplay::menu::NetplayMenuEntry&)> buildRowLabel;
    std::function<std::string(const netplay::menu::NetplayMenuEntry&)> buildRowPrimaryText;
    std::function<std::string(const netplay::menu::NetplayMenuEntry&)> buildRowSecondaryText;
    std::function<std::string()> buildFooterText;
    std::function<HFONT()> getMenuOverlayFont;
    std::function<bool(netplay::menu::NetplayMenuAction)> isInlineEditableAction;
    std::function<bool(netplay::menu::NetplayMenuAction, std::string*, bool)> getInlineEditDisplayValue;
    std::function<int(uint32_t)> getScaledNativeSlideY;
    std::function<std::optional<COLORREF>(const netplay::menu::NetplayMenuEntry&, bool isSelected)> getRowTextColor;
    std::function<bool(const netplay::menu::NetplayMenuEntry&)> isHeaderRow;
    std::function<std::string(const netplay::menu::NetplayMenuEntry&)> buildRowBadgeText;
};

struct RuntimeOverlayState
{
    bool useRuntimeTextOverlay = false;
    bool netplayActive = false;
    bool enableGdiFallbackOverlay = false;
    // RT text draws above indexed fills, so rows must not submit TTF text
    // while a modal panel covers them (it would bleed through the panel).
    bool suppressRtText = false;
    netplay::menu::NetplayMenuId menuId = netplay::menu::NetplayMenuId::Main;
    // Slide offset in logical px; 0 = none, positive enters from the right.
    int contentSlideOffsetX = 0;
};

struct DynamicFieldOverlayState
{
    bool netplayActive = false;
    bool useRuntimeTextOverlay = false;
    bool menuSlideActive = false;
    bool suppressRtText = false;
    netplay::menu::NetplayMenuId menuId = netplay::menu::NetplayMenuId::Main;
    int highlightHeight = 14;
    uint8_t paletteStart = 193;
    uint8_t paletteCount = 48;
    // Slide offset in logical px; 0 = none, positive enters from the right.
    int contentSlideOffsetX = 0;
};

bool DrawRuntimeTextOverlayGdi(
    const OverlayCallbacks& callbacks,
    const RuntimeOverlayState& state,
    uint32_t screenContext,
    bool allowWindowDc);

bool DrawDynamicFieldValuesGdi(
    const OverlayCallbacks& callbacks,
    const DynamicFieldOverlayState& state,
    uint32_t screenContext,
    bool allowWindowDc);
}

