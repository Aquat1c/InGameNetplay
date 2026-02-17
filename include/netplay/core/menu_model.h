#pragma once

#include <cstdint>

namespace netplay::menu
{
constexpr int kConfigOptionCount = 8;

enum class NetplayObRow : uint8_t
{
    Host = 0,
    Join = 1,
    Nickname = 2,
    Address = 3,
    Port = 4,
    Reserved5 = 5,
    Reserved6 = 6,
    ReturnToTitle = 7,
};

constexpr int RowToIndex(NetplayObRow row)
{
    return static_cast<int>(row);
}

enum class NetplayMenuId : uint8_t
{
    Main = 0,
    Host = 1,
    Join = 2,
    Nickname = 3,
};

enum class NetplayMenuAction : uint8_t
{
    OpenHost = 0,
    OpenJoin = 1,
    OpenNickname = 2,
    LeaveNetplay = 3,
    HostStart = 4,
    HostEditPort = 5,
    BackToMain = 6,
    JoinConnect = 7,
    JoinEditAddress = 8,
    JoinEditPort = 9,
    NicknameEdit = 10,
};

struct NetplayMenuEntry
{
    NetplayMenuAction action = NetplayMenuAction::LeaveNetplay;
    int renderRow = 0;
    const char* debugLabel = "";
};

struct NetplayMenuSpec
{
    NetplayMenuId menuId = NetplayMenuId::Main;
    const char* headerLabel = "NETPLAY";
    const NetplayMenuEntry* entries = nullptr;
    int entryCount = 0;
    int defaultSelection = 0;
};

const char* MenuIdToString(NetplayMenuId menuId);
const char* MenuActionToString(NetplayMenuAction action);
const char* RowIndexToString(int rowIndex);
const NetplayMenuSpec* GetMenuSpec(NetplayMenuId menuId);
const NetplayMenuEntry* GetMenuEntries(NetplayMenuId menuId, int* outCount);
int GetDefaultSelectionForMenu(NetplayMenuId menuId);
bool ValidateMenuSpecs();
}

