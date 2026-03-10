#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <windows.h>

#include "netplay/core/menu_model.h"

namespace netplay::inline_edit
{
struct Values
{
    uint16_t hostPort = 7500;
    std::string joinAddress = "127.0.0.1";
    uint16_t joinPort = 7500;
    std::string nickname = "Player";
    std::string playerRoomsRoomCode;
};

struct State
{
    bool active = false;
    netplay::menu::NetplayMenuAction action = netplay::menu::NetplayMenuAction::LeaveNetplay;
    std::string buffer;
    std::array<uint8_t, 256> keyDown = {};
    bool caretVisible = true;
    DWORD lastCaretTick = 0;
    std::string errorMessage;
    DWORD errorExpireTick = 0;
};

enum class InputResult
{
    NotEditing = 0,
    Consumed,
    Cancelled,
    CommitSuccess,
    CommitFailed,
};

bool IsInlineEditableAction(netplay::menu::NetplayMenuAction action);
void ResetState(State* state);
void BeginEdit(State* state, netplay::menu::NetplayMenuAction action, const Values& values);
void CancelEdit(State* state);
bool GetDisplayValue(
    const State& state,
    const Values& values,
    netplay::menu::NetplayMenuAction action,
    std::string* outValue,
    bool includeCaret);

InputResult HandleInput(
    State* state,
    Values* values,
    HWND owner,
    const uint8_t* inputBytes,
    bool* escapeDown);
}


