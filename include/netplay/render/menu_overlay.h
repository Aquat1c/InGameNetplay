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
    std::function<std::string()> buildFooterText;
    std::function<HFONT()> getMenuOverlayFont;
    std::function<bool(netplay::menu::NetplayMenuAction)> isInlineEditableAction;
    std::function<bool(netplay::menu::NetplayMenuAction, std::string*, bool)> getInlineEditDisplayValue;
    std::function<int(uint32_t)> getScaledNativeSlideY;
    // Optional: override text color per row. Return nullopt to use defaults.
    std::function<std::optional<COLORREF>(const netplay::menu::NetplayMenuEntry&, bool isSelected)> getRowTextColor;
};

struct RuntimeOverlayState
{
    bool useRuntimeTextOverlay = false;
    bool netplayActive = false;
    bool enableGdiFallbackOverlay = false;
    netplay::menu::NetplayMenuId menuId = netplay::menu::NetplayMenuId::Main;
};

struct DynamicFieldOverlayState
{
    bool netplayActive = false;
    bool useRuntimeTextOverlay = false;
    bool menuSlideActive = false;
    netplay::menu::NetplayMenuId menuId = netplay::menu::NetplayMenuId::Main;
    int highlightHeight = 14;
    uint8_t paletteStart = 193;
    uint8_t paletteCount = 48;
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


