#include "netplay/core/menu_model.h"

#include "logger.h"

#include <array>

namespace netplay::menu
{
constexpr std::array<NetplayMenuEntry, 4> kMainMenuEntries = {{
    {NetplayMenuAction::OpenHost, RowToIndex(NetplayObRow::Host), "HOST"},
    {NetplayMenuAction::OpenJoin, RowToIndex(NetplayObRow::Join), "JOIN"},
    {NetplayMenuAction::OpenNickname, RowToIndex(NetplayObRow::Nickname), "CHANGE_NICKNAME"},
    {NetplayMenuAction::LeaveNetplay, RowToIndex(NetplayObRow::ReturnToTitle), "RETURN_TO_TITLE"},
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

constexpr std::array<NetplayMenuEntry, 2> kNicknameMenuEntries = {{
    {NetplayMenuAction::NicknameEdit, RowToIndex(NetplayObRow::Nickname), "NICKNAME_EDIT"},
    {NetplayMenuAction::BackToMain, RowToIndex(NetplayObRow::ReturnToTitle), "BACK"},
}};

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
    case NetplayMenuId::Nickname:
        return "Nickname";
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
    case RowToIndex(NetplayObRow::Nickname):
        return "ROW_NICKNAME";
    case RowToIndex(NetplayObRow::Address):
        return "ROW_ADDRESS";
    case RowToIndex(NetplayObRow::Port):
        return "ROW_PORT";
    case RowToIndex(NetplayObRow::Reserved5):
        return "ROW_RESERVED5";
    case RowToIndex(NetplayObRow::Reserved6):
        return "ROW_RESERVED6";
    case RowToIndex(NetplayObRow::ReturnToTitle):
        return "ROW_RETURN";
    default:
        return "ROW_UNKNOWN";
    }
}

const NetplayMenuSpec* GetMenuSpec(NetplayMenuId menuId)
{
    static const std::array<NetplayMenuSpec, 4> specs = {{
        {NetplayMenuId::Main, "NETPLAY SETTINGS", kMainMenuEntries.data(), static_cast<int>(kMainMenuEntries.size()), 0},
        {NetplayMenuId::Host, "HOST SETTINGS", kHostMenuEntries.data(), static_cast<int>(kHostMenuEntries.size()), 0},
        {NetplayMenuId::Join, "JOIN SETTINGS", kJoinMenuEntries.data(), static_cast<int>(kJoinMenuEntries.size()), 0},
        {NetplayMenuId::Nickname, "NICKNAME", kNicknameMenuEntries.data(), static_cast<int>(kNicknameMenuEntries.size()), 0},
    }};

    for (const NetplayMenuSpec& spec : specs)
    {
        if (spec.menuId == menuId)
        {
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
    case NetplayMenuAction::OpenNickname:
        return "OpenNickname";
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
    constexpr std::array<NetplayMenuId, 4> kMenus = {
        NetplayMenuId::Main,
        NetplayMenuId::Host,
        NetplayMenuId::Join,
        NetplayMenuId::Nickname,
    };

    for (NetplayMenuId menuId : kMenus)
    {
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



