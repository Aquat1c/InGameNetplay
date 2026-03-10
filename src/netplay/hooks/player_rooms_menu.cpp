#include "netplay/core/player_rooms_menu.h"

#include "logger.h"
#include "netplay/core/text_utils.h"
#include "netplay/core/validation.h"
#include "netplay/hooks/internal/shared.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace netplay::player_rooms
{
namespace
{
using NetplayMenuAction = netplay::menu::NetplayMenuAction;
using NetplayMenuEntry = netplay::menu::NetplayMenuEntry;
using NetplayMenuId = netplay::menu::NetplayMenuId;
using NetplayMenuSpec = netplay::menu::NetplayMenuSpec;
namespace hooks = netplay::hooks::internal;

constexpr int kBrowserFixedActionCount = 5;
constexpr int kBackRow = netplay::menu::RowToIndex(netplay::menu::NetplayObRow::Blank);
constexpr DWORD kStatusDisplayMs = 2000;

struct State
{
    std::string roomCode;
    bool createPublic = false; // Concerto defaults to Private.
    int listScrollOffset = 0;
    std::vector<netplay::lobby::PublicRoomSummary> publicRooms;
    std::string statusMessage;
    DWORD statusExpireTick = 0;
    netplay::lobby::LobbyJoinedRoom pendingJoinedRoom = {};
    bool hasPendingJoinedRoom = false;
    std::array<NetplayMenuEntry, kBrowserFixedActionCount + kVisiblePublicRoomSlots + 1> entries = {};
    NetplayMenuSpec spec = {};
};

State g_state = {};

void EnsureSpecInitialized()
{
    if (g_state.spec.entries != nullptr)
    {
        return;
    }

    g_state.spec.menuId = NetplayMenuId::PlayerRooms;
    g_state.spec.headerLabel = "PLAYER ROOMS";
    g_state.spec.entries = g_state.entries.data();
    g_state.spec.entryCount = 1;
    g_state.spec.defaultSelection = 0;
    g_state.entries[0] = {NetplayMenuAction::BackToMain, kBackRow, "BACK"};
}

bool HasStatusMessage()
{
    return !g_state.statusMessage.empty() && GetTickCount() < g_state.statusExpireTick;
}

void SetStatusMessage(const char* text)
{
    g_state.statusMessage = text != nullptr ? text : "";
    g_state.statusExpireTick = g_state.statusMessage.empty() ? 0 : (GetTickCount() + kStatusDisplayMs);
}

void ClearStatusMessage()
{
    g_state.statusMessage.clear();
    g_state.statusExpireTick = 0;
}

const char* GetRoomTypeLabel()
{
    return g_state.createPublic ? "Public" : "Private";
}

int GetTotalPublicRoomCount()
{
    return static_cast<int>(g_state.publicRooms.size());
}

int GetVisiblePublicRoomCount()
{
    const int total = GetTotalPublicRoomCount();
    if (total <= 0)
    {
        return 1; // single placeholder row
    }
    return (std::min)(total - g_state.listScrollOffset, kVisiblePublicRoomSlots);
}

int GetMaxListScroll()
{
    return (std::max)(0, GetTotalPublicRoomCount() - kVisiblePublicRoomSlots);
}

int GetRoomSlotBaseIndex()
{
    return kBrowserFixedActionCount;
}

int GetRoomSlotCountForMenu()
{
    return GetVisiblePublicRoomCount();
}

bool IsRoomSlotAction(NetplayMenuAction action)
{
    return action >= NetplayMenuAction::PlayerRoomsSlot0
        && action <= NetplayMenuAction::PlayerRoomsSlot2;
}

int GetRoomIndexForAction(NetplayMenuAction action)
{
    if (!IsRoomSlotAction(action))
    {
        return -1;
    }

    const int slot = static_cast<int>(action) - static_cast<int>(NetplayMenuAction::PlayerRoomsSlot0);
    return g_state.listScrollOffset + slot;
}

std::string BuildPlayerCountText(int playerCount)
{
    char buffer[32] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%d %s",
        playerCount,
        playerCount == 1 ? "player" : "players");
    return buffer;
}

std::string BuildRoomCodeDisplay(bool includePlaceholder)
{
    if (!g_state.roomCode.empty())
    {
        return g_state.roomCode;
    }
    return includePlaceholder ? "<enter code>" : std::string();
}

void RebuildMenuEntries()
{
    EnsureSpecInitialized();

    const int roomSlotCount = GetRoomSlotCountForMenu();
    int entryIndex = 0;
    g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsRefresh, kBackRow, "ROOMS_REFRESH"};
    g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsJoin, kBackRow, "ROOMS_JOIN"};
    g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsEditCode, kBackRow, "ROOMS_EDIT_CODE"};
    g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsCreate, kBackRow, "ROOMS_CREATE"};
    g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsRoomType, kBackRow, "ROOMS_TYPE"};

    for (int slot = 0; slot < roomSlotCount; ++slot)
    {
        g_state.entries[entryIndex++] = {
            netplay::menu::PlayerRoomsSlotAction(slot),
            kBackRow,
            "ROOMS_SLOT",
        };
    }

    g_state.entries[entryIndex++] = {NetplayMenuAction::BackToMain, kBackRow, "BACK"};

    g_state.spec.menuId = NetplayMenuId::PlayerRooms;
    g_state.spec.headerLabel = "PLAYER ROOMS";
    g_state.spec.entries = g_state.entries.data();
    g_state.spec.entryCount = entryIndex;
    g_state.spec.defaultSelection = 0;

    hooks::g_netplayMenuState.optionCount = g_state.spec.entryCount;
    hooks::g_netplayMenuState.backIndex = g_state.spec.entryCount > 0 ? (g_state.spec.entryCount - 1) : 0;
}

bool RefreshPublicRooms(bool showStatusMessage)
{
    std::vector<netplay::lobby::PublicRoomSummary> rooms;
    std::string error;
    const bool ok = netplay::lobby::ListPublicRooms(&rooms, &error);
    if (ok)
    {
        g_state.publicRooms = std::move(rooms);
        if (g_state.listScrollOffset > GetMaxListScroll())
        {
            g_state.listScrollOffset = GetMaxListScroll();
        }
        if (showStatusMessage)
        {
            if (g_state.publicRooms.empty())
            {
                SetStatusMessage("No public rooms found.");
            }
            else
            {
                SetStatusMessage("Public room list refreshed.");
            }
        }
    }
    else
    {
        g_state.publicRooms.clear();
        g_state.listScrollOffset = 0;
        if (showStatusMessage)
        {
            SetStatusMessage(error.empty() ? "Public room list request failed." : error.c_str());
        }
    }

    RebuildMenuEntries();
    return ok;
}

void ToggleRoomType()
{
    g_state.createPublic = !g_state.createPublic;
    ClearStatusMessage();
}

bool JoinRoomAndQueue(const std::string& roomCode)
{
    netplay::lobby::LobbyJoinedRoom joinedRoom;
    std::string error;
    if (!netplay::lobby::JoinRoom(
            hooks::g_netplayMenuState.nickname,
            roomCode,
            hooks::g_netplayMenuState.hostPort,
            netplay::lobby::RoomOrigin::PlayerRooms,
            &joinedRoom,
            &error))
    {
        SetStatusMessage(error.empty() ? "Join room failed." : error.c_str());
        return false;
    }

    g_state.pendingJoinedRoom = std::move(joinedRoom);
    g_state.hasPendingJoinedRoom = true;
    ClearStatusMessage();
    return true;
}

bool CreateRoomAndQueue()
{
    netplay::lobby::LobbyJoinedRoom joinedRoom;
    std::string error;
    if (!netplay::lobby::CreateRoom(
            hooks::g_netplayMenuState.nickname,
            GetRoomTypeLabel(),
            hooks::g_netplayMenuState.hostPort,
            netplay::lobby::RoomOrigin::PlayerRooms,
            &joinedRoom,
            &error))
    {
        SetStatusMessage(error.empty() ? "Create room failed." : error.c_str());
        return false;
    }

    g_state.pendingJoinedRoom = std::move(joinedRoom);
    g_state.hasPendingJoinedRoom = true;
    ClearStatusMessage();
    return true;
}
} // namespace

const NetplayMenuSpec* GetMenuSpec()
{
    EnsureSpecInitialized();
    return &g_state.spec;
}

void ResetState()
{
    g_state = {};
    EnsureSpecInitialized();
}

bool EnterMenu()
{
    EnsureSpecInitialized();
    if (g_state.roomCode.empty())
    {
        g_state.roomCode.clear();
    }
    const bool ok = RefreshPublicRooms(false);
    RebuildMenuEntries();
    return ok;
}

void LeaveMenu()
{
    ClearStatusMessage();
}

std::string BuildRowPrimaryText(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::PlayerRoomsRefresh:
        return "Refresh Rooms";
    case NetplayMenuAction::PlayerRoomsJoin:
        return "Join Room";
    case NetplayMenuAction::PlayerRoomsEditCode:
        return "Room Code";
    case NetplayMenuAction::PlayerRoomsCreate:
        return "Create Room";
    case NetplayMenuAction::PlayerRoomsRoomType:
        return "Room Type";
    case NetplayMenuAction::BackToMain:
        return "Back";
    case NetplayMenuAction::PlayerRoomsSlot0:
    case NetplayMenuAction::PlayerRoomsSlot1:
    case NetplayMenuAction::PlayerRoomsSlot2:
    {
        const int roomIndex = GetRoomIndexForAction(action);
        if (roomIndex < 0 || roomIndex >= static_cast<int>(g_state.publicRooms.size()))
        {
            return "No Public Rooms";
        }
        return "ID " + g_state.publicRooms[static_cast<size_t>(roomIndex)].roomCode;
    }
    default:
        return {};
    }
}

std::string BuildRowSecondaryText(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::PlayerRoomsEditCode:
        if (hooks::g_inlineEditState.active
            && hooks::g_inlineEditState.action == NetplayMenuAction::PlayerRoomsEditCode)
        {
            std::string value = hooks::g_inlineEditState.buffer;
            if (hooks::g_inlineEditState.caretVisible)
            {
                value.push_back('_');
            }
            if (value.empty())
            {
                return "<enter code>";
            }
            return value;
        }
        return BuildRoomCodeDisplay(true);
    case NetplayMenuAction::PlayerRoomsRoomType:
        return GetRoomTypeLabel();
    case NetplayMenuAction::PlayerRoomsSlot0:
    case NetplayMenuAction::PlayerRoomsSlot1:
    case NetplayMenuAction::PlayerRoomsSlot2:
    {
        const int roomIndex = GetRoomIndexForAction(action);
        if (roomIndex < 0 || roomIndex >= static_cast<int>(g_state.publicRooms.size()))
        {
            return {};
        }
        return BuildPlayerCountText(g_state.publicRooms[static_cast<size_t>(roomIndex)].playerCount);
    }
    default:
        return {};
    }
}

std::string BuildRowLabel(NetplayMenuAction action)
{
    const std::string primary = BuildRowPrimaryText(action);
    const std::string secondary = BuildRowSecondaryText(action);
    if (secondary.empty())
    {
        return primary;
    }
    return primary + ": " + secondary;
}

std::string BuildFooterText(NetplayMenuAction selectedAction)
{
    if (HasStatusMessage())
    {
        return g_state.statusMessage;
    }

    switch (selectedAction)
    {
    case NetplayMenuAction::PlayerRoomsRefresh:
        return "Refresh the public room list.";
    case NetplayMenuAction::PlayerRoomsJoin:
        return "Join the room code shown below.";
    case NetplayMenuAction::PlayerRoomsEditCode:
        return "Set the room code or alias to join.\nEnter=Edit";
    case NetplayMenuAction::PlayerRoomsCreate:
        return "Create a new room with the selected visibility.";
    case NetplayMenuAction::PlayerRoomsRoomType:
        return "Choose whether new rooms are Private or Public.\nLeft/Right=Toggle";
    case NetplayMenuAction::PlayerRoomsSlot0:
    case NetplayMenuAction::PlayerRoomsSlot1:
    case NetplayMenuAction::PlayerRoomsSlot2:
    {
        const int roomIndex = GetRoomIndexForAction(selectedAction);
        if (roomIndex < 0 || roomIndex >= static_cast<int>(g_state.publicRooms.size()))
        {
            return "No public rooms are currently listed.";
        }

        const bool canScrollLeft = g_state.listScrollOffset > 0;
        const bool canScrollRight = g_state.listScrollOffset < GetMaxListScroll();
        if (canScrollLeft || canScrollRight)
        {
            return "Join the highlighted public room.\nLeft/Right=Scroll room list";
        }
        return "Join the highlighted public room.";
    }
    case NetplayMenuAction::BackToMain:
        return "Return to the netplay main menu.";
    default:
        return {};
    }
}

bool HandleVerticalNavigation(int currentSelection, int delta, int* outNextSelection)
{
    (void)currentSelection;
    (void)delta;
    (void)outNextSelection;
    return false;
}

bool HandleInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter)
{
    if (inputBytes == nullptr)
    {
        return false;
    }

    auto* const selectionPtr = reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection);
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch =
            reinterpret_cast<uint8_t*>(screenContext + netplay::constants::kOffsetInputLatchP1 + playerIndex);
        const int8_t horizontal = static_cast<int8_t>(inputBytes[playerIndex + 12]);
        if (horizontal == 0)
        {
            continue;
        }

        *inactivityCounter = 0;
        if (*inputLatch != 0)
        {
            return false;
        }

        const int selection = static_cast<int>(*selectionPtr);
        const NetplayMenuEntry* entry =
            selection >= 0 && selection < g_state.spec.entryCount ? &g_state.entries[selection] : nullptr;
        if (entry == nullptr)
        {
            return false;
        }

        bool handled = false;
        if (entry->action == NetplayMenuAction::PlayerRoomsRoomType)
        {
            ToggleRoomType();
            handled = true;
        }
        else if (IsRoomSlotAction(entry->action))
        {
            const int maxScroll = GetMaxListScroll();
            if (horizontal > 0 && g_state.listScrollOffset < maxScroll)
            {
                ++g_state.listScrollOffset;
                RebuildMenuEntries();
                handled = true;
            }
            else if (horizontal < 0 && g_state.listScrollOffset > 0)
            {
                --g_state.listScrollOffset;
                RebuildMenuEntries();
                handled = true;
            }
        }

        if (handled)
        {
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
            *reinterpret_cast<uint16_t*>(screenContext + netplay::constants::kOffsetMenuAnimCounter) = 0;
            *inputLatch = 1;
            hooks::g_lastLoggedSelection = *selectionPtr;
            return true;
        }
    }

    return false;
}

bool ExecuteAction(uint32_t screenContext, NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::PlayerRoomsRefresh:
        RefreshPublicRooms(true);
        return true;
    case NetplayMenuAction::PlayerRoomsJoin:
        if (!netplay::validation::IsValidLobbyRoomCode(g_state.roomCode))
        {
            SetStatusMessage("Enter a valid room code first.");
            return true;
        }
        if (JoinRoomAndQueue(g_state.roomCode))
        {
            hooks::StartMenuSlideTransition(screenContext, NetplayMenuId::Lobby, -1, +1);
        }
        return true;
    case NetplayMenuAction::PlayerRoomsEditCode:
        hooks::BeginInlineEdit(NetplayMenuAction::PlayerRoomsEditCode);
        return true;
    case NetplayMenuAction::PlayerRoomsCreate:
        if (CreateRoomAndQueue())
        {
            hooks::StartMenuSlideTransition(screenContext, NetplayMenuId::Lobby, -1, +1);
        }
        return true;
    case NetplayMenuAction::PlayerRoomsRoomType:
        ToggleRoomType();
        return true;
    case NetplayMenuAction::PlayerRoomsSlot0:
    case NetplayMenuAction::PlayerRoomsSlot1:
    case NetplayMenuAction::PlayerRoomsSlot2:
    {
        const int roomIndex = GetRoomIndexForAction(action);
        if (roomIndex < 0 || roomIndex >= static_cast<int>(g_state.publicRooms.size()))
        {
            SetStatusMessage("No public room is available in this slot.");
            return true;
        }
        if (JoinRoomAndQueue(g_state.publicRooms[static_cast<size_t>(roomIndex)].roomCode))
        {
            hooks::StartMenuSlideTransition(screenContext, NetplayMenuId::Lobby, -1, +1);
        }
        return true;
    }
    default:
        return false;
    }
}

bool GetInlineEditDisplayValue(NetplayMenuAction action, std::string* outValue, bool includeCaret)
{
    if (action != NetplayMenuAction::PlayerRoomsEditCode || outValue == nullptr)
    {
        return false;
    }

    if (hooks::g_inlineEditState.active && hooks::g_inlineEditState.action == action)
    {
        *outValue = hooks::g_inlineEditState.buffer;
        if (includeCaret && hooks::g_inlineEditState.caretVisible)
        {
            outValue->push_back('_');
        }
    }
    else
    {
        *outValue = BuildRoomCodeDisplay(false);
    }
    return true;
}

std::string GetRoomCode()
{
    return g_state.roomCode;
}

void SetRoomCode(std::string value)
{
    g_state.roomCode = netplay::text::TrimAscii(std::move(value));
}

bool ConsumePendingJoinedRoom(netplay::lobby::LobbyJoinedRoom* outJoinedRoom)
{
    if (!g_state.hasPendingJoinedRoom || outJoinedRoom == nullptr)
    {
        return false;
    }

    *outJoinedRoom = g_state.pendingJoinedRoom;
    g_state.pendingJoinedRoom = {};
    g_state.hasPendingJoinedRoom = false;
    return true;
}
}
