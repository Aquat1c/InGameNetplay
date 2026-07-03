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
    BattleLog = 6,
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
    PlayerRoomsOpenJoin = 14,
    PlayerRoomsOpenCreate = 15,
    PlayerRoomsRefresh = 16,
    PlayerRoomsJoin = 17,
    PlayerRoomsEditCode = 18,
    PlayerRoomsCreate = 19,
    PlayerRoomsRoomType = 20,
    PlayerRoomsSlot0 = 21,
    PlayerRoomsSlot1 = 22,
    PlayerRoomsSlot2 = 23,
    // Active room browser
    LobbySlot0 = 24,
    LobbySlot1 = 25,
    LobbySlot2 = 26,
    LobbySlot3 = 27,
    LobbySlot4 = 28,
    LobbySlot5 = 29,
    LobbyPlaying0 = 30, // playing-pair display row (non-interactive)
    // Dynamic options browser rows.
    OptionRow0 = 31,
    OptionRow1 = 32,
    OptionRow2 = 33,
    OptionRow3 = 34,
    OptionRow4 = 35,
    OptionRow5 = 36,
    OptionRow6 = 37,
    OptionRow7 = 38,
    // Battle Log summary/browser/filter/detail rows.
    BattleLogBrowseMine = 39,
    BattleLogSearchFilters = 40,
    BattleLogBrowseAll = 41,
    BattleLogRefresh = 42,
    BattleLogSession0 = 43,
    BattleLogSession1 = 44,
    BattleLogSession2 = 45,
    BattleLogSession3 = 46,
    BattleLogSession4 = 47,
    BattleLogSession5 = 48,
    BattleLogEditPlayerName = 49,
    BattleLogEditOpponentName = 50,
    BattleLogPlayerCharacter = 51,
    BattleLogOpponentCharacter = 52,
    BattleLogSetStatus = 53,
    BattleLogGameCount = 54,
    BattleLogCharacterSwitches = 55,
    BattleLogApplyFilters = 56,
    BattleLogResetFilters = 57,
    BattleLogBrowserPrevPage = 58,
    BattleLogBrowserNextPage = 59,
    BattleLogBrowserFilters = 60,
    BattleLogGame0 = 61,
    BattleLogGame1 = 62,
    BattleLogGame2 = 63,
    BattleLogGame3 = 64,
    BattleLogGame4 = 65,
    BattleLogGame5 = 66,
    BattleLogGame6 = 67,
    BattleLogDetailPrevPage = 68,
    BattleLogDetailNextPage = 69,
    BattleLogBack = 70,
    JoinSpectateIp = 71,
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

constexpr NetplayMenuAction BattleLogSessionAction(int slot)
{
    return static_cast<NetplayMenuAction>(
        static_cast<int>(NetplayMenuAction::BattleLogSession0) + slot);
}

constexpr NetplayMenuAction BattleLogGameAction(int slot)
{
    return static_cast<NetplayMenuAction>(
        static_cast<int>(NetplayMenuAction::BattleLogGame0) + slot);
}

constexpr int kLobbyMaxDisplayPlayers = 6;
constexpr int kLobbyMaxPlayingPairs   = 1;
constexpr int kBattleLogVisibleSessionRows = 6;
constexpr int kBattleLogVisibleGameRows = 7;

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
