#pragma once

#include <cstdint>
#include <string>

#include "netplay/core/menu_model.h"

namespace netplay::options
{
constexpr int kVisibleRowCount = 8;

const netplay::menu::NetplayMenuSpec* GetMenuSpec();

void ResetState();
bool EnterMenu();
void LeaveMenu();

bool IsVisibleRowAction(netplay::menu::NetplayMenuAction action);
std::string BuildRowLabel(netplay::menu::NetplayMenuAction action);
std::string BuildRowPrimaryText(netplay::menu::NetplayMenuAction action);
std::string BuildRowSecondaryText(netplay::menu::NetplayMenuAction action);
std::string BuildFooterText(netplay::menu::NetplayMenuAction selectedAction);

bool HandleVerticalNavigation(int currentSelection, int delta, int* outNextSelection);
bool HandleInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter, bool* escapeDown);
bool ExecuteAction(uint32_t screenContext, netplay::menu::NetplayMenuAction action);

bool DrawSaveOverlayGdi(uint32_t screenContext, bool allowWindowDc);
bool IsSaveOverlayActive();
bool IsBusy();
}
