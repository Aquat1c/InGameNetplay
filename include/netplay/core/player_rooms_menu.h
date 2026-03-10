#pragma once

#include <cstdint>
#include <string>

#include "netplay/core/lobby_client.h"
#include "netplay/core/menu_model.h"

namespace netplay::player_rooms
{
constexpr int kVisiblePublicRoomSlots = 3;

const netplay::menu::NetplayMenuSpec* GetMenuSpec();

void ResetState();
bool EnterMenu();
void LeaveMenu();

std::string BuildRowLabel(netplay::menu::NetplayMenuAction action);
std::string BuildRowPrimaryText(netplay::menu::NetplayMenuAction action);
std::string BuildRowSecondaryText(netplay::menu::NetplayMenuAction action);
std::string BuildFooterText(netplay::menu::NetplayMenuAction selectedAction);

bool HandleVerticalNavigation(int currentSelection, int delta, int* outNextSelection);
bool HandleInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter);
bool ExecuteAction(uint32_t screenContext, netplay::menu::NetplayMenuAction action);

bool GetInlineEditDisplayValue(
    netplay::menu::NetplayMenuAction action,
    std::string* outValue,
    bool includeCaret);
std::string GetRoomCode();
void SetRoomCode(std::string value);

bool ConsumePendingJoinedRoom(netplay::lobby::LobbyJoinedRoom* outJoinedRoom);
}
