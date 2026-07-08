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
// True when the row is a non-selectable section header ("Match", "Replay",
// ...) in the flattened options list.
bool IsHeaderRowAction(netplay::menu::NetplayMenuAction action);
std::string BuildRowLabel(netplay::menu::NetplayMenuAction action);
std::string BuildRowPrimaryText(netplay::menu::NetplayMenuAction action);
std::string BuildRowSecondaryText(netplay::menu::NetplayMenuAction action);
std::string BuildFooterText(netplay::menu::NetplayMenuAction selectedAction);

bool HandleVerticalNavigation(int currentSelection, int delta, int* outNextSelection);
bool HandleInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter, bool* escapeDown);
bool ExecuteAction(uint32_t screenContext, netplay::menu::NetplayMenuAction action);

bool DrawSaveOverlayGdi(uint32_t screenContext, bool allowWindowDc);
bool IsSaveOverlayActive();
// Signed horizontal offset (logical 320-space px) for the category drill
// in/out slide; 0 when idle. Positive = content entering from the right.
int GetOptionsSlideOffsetX();
bool IsBusy();
uint8_t GetMenuDetailForStateExport();
bool UseTournamentModeForOfflineVsHuman();
}
