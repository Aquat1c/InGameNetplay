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
    Lobby = 4,
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
    // Lobby browser
    OpenLobby = 11,
    LobbySlot0 = 12,
    LobbySlot1 = 13,
    LobbySlot2 = 14,
    LobbySlot3 = 15,
    LobbySlot4 = 16,
    LobbySlot5 = 17,
    LobbyPlaying0 = 18, // playing-pair display row (non-interactive)
};

// Returns the LobbySlotN action for a given zero-based slot index [0, kLobbyMaxDisplayPlayers).
constexpr NetplayMenuAction LobbySlotAction(int slot)
{
    return static_cast<NetplayMenuAction>(static_cast<int>(NetplayMenuAction::LobbySlot0) + slot);
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

