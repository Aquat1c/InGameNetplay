#pragma once

#include <cstdint>

namespace netplay::menu
{
constexpr int kConfigOptionCount = 10;

enum class NetplayObRow : uint8_t
{
    Host = 0,
    Join = 1,
    PlayerRooms = 2,
    Lobby = 3,
    BattleLog = 4,
    Options = 5,
    Address = 6,
    Port = 7,
    Blank = 8,
    ReturnToTitle = 9,
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
    PlayerRooms = 3,
    Options = 4,
    Lobby = 5,
};

enum class NetplayMenuAction : uint8_t
{
    OpenHost = 0,
    OpenJoin = 1,
    OpenPlayerRooms = 2,
    OpenLobby = 3,
    OpenBattleLog = 4,
    OpenOptions = 5,
    LeaveNetplay = 6,
    HostStart = 7,
    HostEditPort = 8,
    BackToMain = 9,
    JoinConnect = 10,
    JoinEditAddress = 11,
    JoinEditPort = 12,
    NicknameEdit = 13,
    // Player Rooms browser
    PlayerRoomsRefresh = 14,
    PlayerRoomsJoin = 15,
    PlayerRoomsEditCode = 16,
    PlayerRoomsCreate = 17,
    PlayerRoomsRoomType = 18,
    PlayerRoomsSlot0 = 19,
    PlayerRoomsSlot1 = 20,
    PlayerRoomsSlot2 = 21,
    // Active room browser
    LobbySlot0 = 22,
    LobbySlot1 = 23,
    LobbySlot2 = 24,
    LobbySlot3 = 25,
    LobbySlot4 = 26,
    LobbySlot5 = 27,
    LobbyPlaying0 = 28, // playing-pair display row (non-interactive)
    // Dynamic options browser rows.
    OptionRow0 = 29,
    OptionRow1 = 30,
    OptionRow2 = 31,
    OptionRow3 = 32,
    OptionRow4 = 33,
    OptionRow5 = 34,
    OptionRow6 = 35,
    OptionRow7 = 36,
};

constexpr NetplayMenuAction PlayerRoomsSlotAction(int slot)
{
    return static_cast<NetplayMenuAction>(
        static_cast<int>(NetplayMenuAction::PlayerRoomsSlot0) + slot);
}

// Returns the LobbySlotN action for a given zero-based slot index [0, kLobbyMaxDisplayPlayers).
constexpr NetplayMenuAction LobbySlotAction(int slot)
{
    return static_cast<NetplayMenuAction>(static_cast<int>(NetplayMenuAction::LobbySlot0) + slot);
}

constexpr NetplayMenuAction OptionVisibleAction(int slot)
{
    return static_cast<NetplayMenuAction>(static_cast<int>(NetplayMenuAction::OptionRow0) + slot);
}

constexpr int kLobbyMaxDisplayPlayers = 6;
constexpr int kLobbyMaxPlayingPairs   = 1;

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

// Rebuilds the dynamic lobby entry list based on the current number of idle
// players visible on screen (after applying scroll offset).  This must be
// called once when entering the lobby menu and then once per update frame so
// that unused rows are hidden correctly.
// |idleCount| is the total number of idle players presently in the lobby
// (NOT the number visible in the window – the function clamps internally).
// |playingCount| is the number of active playing pairs; LobbyPlaying0 is
// only included in the entry list when this is greater than zero.
void RebuildLobbyMenuEntries(int idleCount, int playingCount);
}
