#include "netplay/core/menu_model.h"
#include "netplay/core/options_menu.h"

#include "logger.h"

#include <algorithm>
#include <array>

namespace netplay::menu
{
constexpr std::array<NetplayMenuEntry, 7> kMainMenuEntries = {{
    {NetplayMenuAction::OpenHost,     RowToIndex(NetplayObRow::Host),         "HOST"},
    {NetplayMenuAction::OpenJoin,     RowToIndex(NetplayObRow::Join),         "JOIN"},
    {NetplayMenuAction::OpenPlayerRooms, RowToIndex(NetplayObRow::PlayerRooms), "PLAYER_ROOMS"},
    {NetplayMenuAction::OpenLobby,    RowToIndex(NetplayObRow::Lobby),        "LOBBY"},
    {NetplayMenuAction::OpenBattleLog, RowToIndex(NetplayObRow::BattleLog),   "BATTLE_LOG"},
    {NetplayMenuAction::OpenOptions,  RowToIndex(NetplayObRow::Options),      "OPTIONS"},
    {NetplayMenuAction::LeaveNetplay, RowToIndex(NetplayObRow::ReturnToTitle),"RETURN_TO_TITLE"},
}};

constexpr std::array<NetplayMenuEntry, 3> kHostMenuEntries = {{
    {NetplayMenuAction::HostStart, RowToIndex(NetplayObRow::Host), "HOST_START"},
    {NetplayMenuAction::HostEditPort, RowToIndex(NetplayObRow::Port), "HOST_PORT"},
    {NetplayMenuAction::BackToMain, RowToIndex(NetplayObRow::ReturnToTitle), "BACK"},
}};

constexpr std::array<NetplayMenuEntry, 4> kJoinMenuEntries = {{
    {NetplayMenuAction::JoinConnect, RowToIndex(NetplayObRow::Join), "JOIN_CONNECT"},
    {NetplayMenuAction::JoinEditAddress, RowToIndex(NetplayObRow::Address), "JOIN_ADDRESS"},
    {NetplayMenuAction::JoinEditPort, RowToIndex(NetplayObRow::Port), "JOIN_PORT"},
    {NetplayMenuAction::BackToMain, RowToIndex(NetplayObRow::ReturnToTitle), "BACK"},
}};

// Lobby browser: dynamic entry list rebuilt per-frame by RebuildLobbyMenuEntries.
// Maximum size is kLobbyMaxDisplayPlayers slots + LobbyPlaying0 + BackToMain.
static NetplayMenuEntry s_lobbyDynEntries[kLobbyMaxDisplayPlayers + 2] = {};
static NetplayMenuSpec  s_lobbyDynSpec = {
    NetplayMenuId::Lobby, "LOBBY BROWSER", s_lobbyDynEntries, 0, 0
};

void RebuildLobbyMenuEntries(int idleCount, int playingCount)
{
    const int visSlots = std::min(idleCount, kLobbyMaxDisplayPlayers);
    // All player/playing rows use the dedicated blank bar from the sprite sheet.
    // The actual label text is drawn on top by DrawDynamicFieldValuesGdi.
    // BackToMain uses ReturnToTitle so the "RETURN TO TITLE" bar is shown.
    constexpr int kBlankRow  = RowToIndex(NetplayObRow::Blank);
    constexpr int kBackRow   = RowToIndex(NetplayObRow::ReturnToTitle);
    int idx = 0;
    for (int s = 0; s < visSlots; ++s)
    {
        s_lobbyDynEntries[idx++] = {LobbySlotAction(s), kBlankRow, "LOBBY_SLOT"};
    }
    // LobbyPlaying0 only added when there is at least one active match.
    if (playingCount > 0)
    {
        s_lobbyDynEntries[idx++] = {NetplayMenuAction::LobbyPlaying0, kBlankRow, "LOBBY_PLAYING_0"};
    }
    s_lobbyDynEntries[idx++] = {NetplayMenuAction::BackToMain, kBackRow, "BACK"};
    s_lobbyDynSpec.entryCount = idx;
}

const char* MenuIdToString(NetplayMenuId menuId)
{
    switch (menuId)
    {
    case NetplayMenuId::Main:
        return "Main";
    case NetplayMenuId::Host:
        return "Host";
    case NetplayMenuId::Join:
        return "Join";
    case NetplayMenuId::Options:
        return "Options";
    case NetplayMenuId::Lobby:
        return "Lobby";
    default:
        return "Unknown";
    }
}

const char* RowIndexToString(int rowIndex)
{
    switch (rowIndex)
    {
    case RowToIndex(NetplayObRow::Host):
        return "ROW_HOST";
    case RowToIndex(NetplayObRow::Join):
        return "ROW_JOIN";
    case RowToIndex(NetplayObRow::PlayerRooms):
        return "ROW_PLAYER_ROOMS";
    case RowToIndex(NetplayObRow::Lobby):
        return "ROW_LOBBY";
    case RowToIndex(NetplayObRow::BattleLog):
        return "ROW_BATTLE_LOG";
    case RowToIndex(NetplayObRow::Options):
        return "ROW_OPTIONS";
    case RowToIndex(NetplayObRow::Address):
        return "ROW_ADDRESS";
    case RowToIndex(NetplayObRow::Port):
        return "ROW_PORT";
    case RowToIndex(NetplayObRow::Blank):
        return "ROW_BLANK";
    case RowToIndex(NetplayObRow::ReturnToTitle):
        return "ROW_RETURN";
    default:
        return "ROW_UNKNOWN";
    }
}

const NetplayMenuSpec* GetMenuSpec(NetplayMenuId menuId)
{
    // Lobby uses a dynamically rebuilt spec (entry count changes with player
    // count) so it lives in mutable storage rather than in a const static array.
    if (menuId == NetplayMenuId::Lobby)
    {
        return &s_lobbyDynSpec;
    }

    static const std::array<NetplayMenuSpec, 4> specs = {{
        {NetplayMenuId::Main,     "NETPLAY SETTINGS", kMainMenuEntries.data(),     static_cast<int>(kMainMenuEntries.size()),     0},
        {NetplayMenuId::Host,     "HOST SETTINGS",    kHostMenuEntries.data(),     static_cast<int>(kHostMenuEntries.size()),     0},
        {NetplayMenuId::Join,     "JOIN SETTINGS",    kJoinMenuEntries.data(),     static_cast<int>(kJoinMenuEntries.size()),     0},
        {NetplayMenuId::Options,  "OPTIONS",          nullptr,                      0,                                              0},
    }};

    for (const NetplayMenuSpec& spec : specs)
    {
        if (spec.menuId == menuId)
        {
            if (menuId == NetplayMenuId::Options)
            {
                return netplay::options::GetMenuSpec();
            }
            return &spec;
        }
    }
    return nullptr;
}

const char* MenuActionToString(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::OpenHost:
        return "OpenHost";
    case NetplayMenuAction::OpenJoin:
        return "OpenJoin";
    case NetplayMenuAction::OpenPlayerRooms:
        return "OpenPlayerRooms";
    case NetplayMenuAction::OpenLobby:
        return "OpenLobby";
    case NetplayMenuAction::OpenBattleLog:
        return "OpenBattleLog";
    case NetplayMenuAction::OpenOptions:
        return "OpenOptions";
    case NetplayMenuAction::LeaveNetplay:
        return "LeaveNetplay";
    case NetplayMenuAction::HostStart:
        return "HostStart";
    case NetplayMenuAction::HostEditPort:
        return "HostEditPort";
    case NetplayMenuAction::BackToMain:
        return "BackToMain";
    case NetplayMenuAction::JoinConnect:
        return "JoinConnect";
    case NetplayMenuAction::JoinEditAddress:
        return "JoinEditAddress";
    case NetplayMenuAction::JoinEditPort:
        return "JoinEditPort";
    case NetplayMenuAction::NicknameEdit:
        return "NicknameEdit";
    case NetplayMenuAction::LobbySlot0:
        return "LobbySlot0";
    case NetplayMenuAction::LobbySlot1:
        return "LobbySlot1";
    case NetplayMenuAction::LobbySlot2:
        return "LobbySlot2";
    case NetplayMenuAction::LobbySlot3:
        return "LobbySlot3";
    case NetplayMenuAction::LobbySlot4:
        return "LobbySlot4";
    case NetplayMenuAction::LobbySlot5:
        return "LobbySlot5";
    case NetplayMenuAction::LobbyPlaying0:
        return "LobbyPlaying0";
    case NetplayMenuAction::OptionRow0:
        return "OptionRow0";
    case NetplayMenuAction::OptionRow1:
        return "OptionRow1";
    case NetplayMenuAction::OptionRow2:
        return "OptionRow2";
    case NetplayMenuAction::OptionRow3:
        return "OptionRow3";
    case NetplayMenuAction::OptionRow4:
        return "OptionRow4";
    case NetplayMenuAction::OptionRow5:
        return "OptionRow5";
    case NetplayMenuAction::OptionRow6:
        return "OptionRow6";
    case NetplayMenuAction::OptionRow7:
        return "OptionRow7";
    default:
        return "Unknown";
    }
}

const NetplayMenuEntry* GetMenuEntries(NetplayMenuId menuId, int* outCount)
{
    const NetplayMenuSpec* spec = GetMenuSpec(menuId);
    if (spec == nullptr)
    {
        if (outCount != nullptr)
        {
            *outCount = 0;
        }
        return nullptr;
    }

    if (outCount != nullptr)
    {
        *outCount = spec->entryCount;
    }
    return spec->entries;
}

int GetDefaultSelectionForMenu(NetplayMenuId menuId)
{
    const NetplayMenuSpec* spec = GetMenuSpec(menuId);
    if (spec == nullptr)
    {
        return 0;
    }
    return spec->defaultSelection;
}

bool ValidateMenuSpecs()
{
    constexpr std::array<NetplayMenuId, 5> kMenus = {
        NetplayMenuId::Main,
        NetplayMenuId::Host,
        NetplayMenuId::Join,
        NetplayMenuId::Options,
        NetplayMenuId::Lobby,
    };

    for (NetplayMenuId menuId : kMenus)
    {
        // The lobby spec is rebuilt dynamically at runtime; seed it so
        // validation can inspect a minimal valid layout.
        if (menuId == NetplayMenuId::Lobby)
        {
            RebuildLobbyMenuEntries(0, 0);
        }
        else if (menuId == NetplayMenuId::Options)
        {
            netplay::options::ResetState();
        }

        const NetplayMenuSpec* spec = GetMenuSpec(menuId);
        if (spec == nullptr || spec->entries == nullptr || spec->entryCount <= 0)
        {
            mod::Log("ValidateMenuSpecs: invalid spec for menu=%s", MenuIdToString(menuId));
            return false;
        }

        if (spec->defaultSelection < 0 || spec->defaultSelection >= spec->entryCount)
        {
            mod::Log(
                "ValidateMenuSpecs: invalid default selection menu=%s default=%d count=%d",
                MenuIdToString(menuId),
                spec->defaultSelection,
                spec->entryCount);
            return false;
        }

        std::array<uint8_t, kConfigOptionCount> usedRows = {};
        for (int i = 0; i < spec->entryCount; ++i)
        {
            const NetplayMenuEntry& entry = spec->entries[i];
            if (entry.renderRow < 0 || entry.renderRow >= kConfigOptionCount)
            {
                mod::Log(
                    "ValidateMenuSpecs: row out of range menu=%s entry=%d row=%d label=%s",
                    MenuIdToString(menuId),
                    i,
                    entry.renderRow,
                    entry.debugLabel);
                return false;
            }

            if (usedRows[static_cast<size_t>(entry.renderRow)] != 0)
            {
                mod::Log(
                    "ValidateMenuSpecs: duplicate row menu=%s row=%d(%s) entry=%s",
                    MenuIdToString(menuId),
                    entry.renderRow,
                    RowIndexToString(entry.renderRow),
                    entry.debugLabel);
                return false;
            }
            usedRows[static_cast<size_t>(entry.renderRow)] = 1u;
        }
    }

    mod::Log("ValidateMenuSpecs: row-slot map valid");
    return true;
}
}
