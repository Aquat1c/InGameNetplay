#include "netplay/core/player_rooms_menu.h"

#include "efz_netplay_state.h"
#include "logger.h"
#include "netplay/core/text_utils.h"
#include "netplay/core/validation.h"
#include "netplay/hooks/internal/shared.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
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

constexpr int kMaxBrowserEntries = 7;
constexpr int kBackRow = netplay::menu::RowToIndex(netplay::menu::NetplayObRow::Blank);
constexpr DWORD kStatusDisplayMs = 2000;

enum class BrowserView : uint8_t
{
    Root = 0,
    Join = 1,
    Create = 2,
};

struct State
{
    BrowserView view = BrowserView::Root;
    int rootSelection = 0;
    int joinSelection = 0;
    int createSelection = 0;
    std::string roomCode;
    bool createPublic = false; // Concerto defaults to Private.
    int listScrollOffset = 0;
    std::vector<netplay::lobby::PublicRoomSummary> publicRooms;
    std::string statusMessage;
    DWORD statusExpireTick = 0;
    netplay::lobby::LobbyJoinedRoom pendingJoinedRoom = {};
    bool hasPendingJoinedRoom = false;
    std::array<NetplayMenuEntry, kMaxBrowserEntries> entries = {};
    NetplayMenuSpec spec = {};
};

State g_state = {};
std::mutex g_refreshMutex;
std::atomic<bool> g_refreshInFlight{false};
std::atomic<uint32_t> g_refreshGeneration{1};
bool g_refreshResultReady = false;
bool g_refreshResultShowStatus = false;
bool g_refreshResultOk = false;
uint32_t g_refreshResultGeneration = 0;
std::vector<netplay::lobby::PublicRoomSummary> g_refreshResultRooms;
std::string g_refreshResultError;
bool g_lobbySessionShutdownInFlight = false;
bool g_refreshAfterLobbySessionShutdown = false;
std::array<int8_t, 2> g_lastHorizontalInput = {};

bool StartAsyncRefresh(bool showStatusMessage);

int* GetSelectionStorage(BrowserView view)
{
    switch (view)
    {
    case BrowserView::Root:
        return &g_state.rootSelection;
    case BrowserView::Join:
        return &g_state.joinSelection;
    case BrowserView::Create:
        return &g_state.createSelection;
    default:
        return &g_state.rootSelection;
    }
}

const char* GetHeaderLabel()
{
    switch (g_state.view)
    {
    case BrowserView::Root:
        return "PLAYER ROOMS";
    case BrowserView::Join:
        return "JOIN ROOM";
    case BrowserView::Create:
        return "CREATE ROOM";
    default:
        return "PLAYER ROOMS";
    }
}

void EnsureSpecInitialized()
{
    if (g_state.spec.entries != nullptr)
    {
        return;
    }

    g_state.spec.menuId = NetplayMenuId::PlayerRooms;
    g_state.spec.headerLabel = GetHeaderLabel();
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

void InvalidateRefreshResults()
{
    g_refreshGeneration.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lock(g_refreshMutex);
    g_refreshResultReady = false;
    g_refreshResultShowStatus = false;
    g_refreshResultOk = false;
    g_refreshResultGeneration = 0;
    g_refreshResultRooms.clear();
    g_refreshResultError.clear();
}

void ResetHorizontalInputState()
{
    g_lastHorizontalInput.fill(0);
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

int GetRoomSlotCountForMenu()
{
    if (g_state.view != BrowserView::Join)
    {
        return 0;
    }

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

int ClampSelectionForCurrentSpec(int selection)
{
    if (g_state.spec.entryCount <= 0)
    {
        return 0;
    }
    if (selection < 0)
    {
        return 0;
    }
    if (selection >= g_state.spec.entryCount)
    {
        return g_state.spec.entryCount - 1;
    }
    return selection;
}

void ApplySelectionToScreen(uint32_t screenContext, int selection)
{
    if (screenContext == 0)
    {
        return;
    }

    *reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection) =
        static_cast<int8_t>(selection);
    *reinterpret_cast<uint16_t*>(screenContext + netplay::constants::kOffsetMenuAnimCounter) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + netplay::constants::kOffsetInputLatchP1) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + netplay::constants::kOffsetInputLatchP2) = 0;
    *reinterpret_cast<uint32_t*>(screenContext + netplay::constants::kOffsetInactivityCounter) = 0;
    hooks::g_lastLoggedSelection = static_cast<int8_t>(selection);
}

void PersistCurrentSelection(uint32_t screenContext)
{
    if (screenContext == 0)
    {
        return;
    }

    const int selection =
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection));
    *GetSelectionStorage(g_state.view) = ClampSelectionForCurrentSpec(selection);
}

void RebuildMenuEntries()
{
    EnsureSpecInitialized();

    int entryIndex = 0;
    switch (g_state.view)
    {
    case BrowserView::Root:
        g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsOpenJoin, kBackRow, "ROOMS_OPEN_JOIN"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsOpenCreate, kBackRow, "ROOMS_OPEN_CREATE"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsRefresh, kBackRow, "ROOMS_REFRESH"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BackToMain, kBackRow, "BACK"};
        break;

    case BrowserView::Join:
        g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsJoin, kBackRow, "ROOMS_JOIN"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsEditCode, kBackRow, "ROOMS_EDIT_CODE"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsRefresh, kBackRow, "ROOMS_REFRESH"};
        for (int slot = 0; slot < GetRoomSlotCountForMenu(); ++slot)
        {
            g_state.entries[entryIndex++] = {
                netplay::menu::PlayerRoomsSlotAction(slot),
                kBackRow,
                "ROOMS_SLOT",
            };
        }
        g_state.entries[entryIndex++] = {NetplayMenuAction::BackToMain, kBackRow, "BACK"};
        break;

    case BrowserView::Create:
        g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsCreate, kBackRow, "ROOMS_CREATE"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::PlayerRoomsRoomType, kBackRow, "ROOMS_TYPE"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BackToMain, kBackRow, "BACK"};
        break;
    }

    g_state.spec.menuId = NetplayMenuId::PlayerRooms;
    g_state.spec.headerLabel = GetHeaderLabel();
    g_state.spec.entries = g_state.entries.data();
    g_state.spec.entryCount = entryIndex;
    g_state.spec.defaultSelection = ClampSelectionForCurrentSpec(*GetSelectionStorage(g_state.view));

    hooks::g_netplayMenuState.optionCount = g_state.spec.entryCount;
    hooks::g_netplayMenuState.backIndex = g_state.spec.entryCount > 0 ? (g_state.spec.entryCount - 1) : 0;
}

void SwitchView(uint32_t screenContext, BrowserView newView, int selection = -1)
{
    PersistCurrentSelection(screenContext);
    g_state.view = newView;
    RebuildMenuEntries();

    int nextSelection = selection;
    if (nextSelection < 0)
    {
        nextSelection = *GetSelectionStorage(newView);
    }
    nextSelection = ClampSelectionForCurrentSpec(nextSelection);
    *GetSelectionStorage(newView) = nextSelection;
    ApplySelectionToScreen(screenContext, nextSelection);
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

bool IsRefreshInFlight()
{
    return g_refreshInFlight.load(std::memory_order_acquire);
}

void PumpRefreshResult()
{
    bool ready = false;
    bool showStatusMessage = false;
    bool ok = false;
    uint32_t resultGeneration = 0;
    std::vector<netplay::lobby::PublicRoomSummary> rooms;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(g_refreshMutex);
        ready = g_refreshResultReady;
        if (!ready)
        {
            return;
        }

        showStatusMessage = g_refreshResultShowStatus;
        ok = g_refreshResultOk;
        resultGeneration = g_refreshResultGeneration;
        rooms = std::move(g_refreshResultRooms);
        error = std::move(g_refreshResultError);
        g_refreshResultRooms.clear();
        g_refreshResultError.clear();
        g_refreshResultReady = false;
        g_refreshResultShowStatus = false;
        g_refreshResultOk = false;
        g_refreshResultGeneration = 0;
    }

    g_refreshInFlight.store(false, std::memory_order_release);

    if (resultGeneration != g_refreshGeneration.load(std::memory_order_acquire))
    {
        mod::Log(
            "PlayerRooms::PumpRefreshResult: discarded stale refresh result generation=%u current=%u",
            static_cast<unsigned>(resultGeneration),
            static_cast<unsigned>(g_refreshGeneration.load(std::memory_order_relaxed)));
        if (g_refreshAfterLobbySessionShutdown && !g_lobbySessionShutdownInFlight && !IsRefreshInFlight())
        {
            g_refreshAfterLobbySessionShutdown = false;
            (void)StartAsyncRefresh(false);
        }
        return;
    }

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
        if (g_state.publicRooms.empty())
        {
            g_state.listScrollOffset = 0;
        }
        if (showStatusMessage)
        {
            SetStatusMessage(error.empty() ? "Public room list request failed." : error.c_str());
        }
    }

    RebuildMenuEntries();

    if (g_refreshAfterLobbySessionShutdown && !g_lobbySessionShutdownInFlight && !IsRefreshInFlight())
    {
        g_refreshAfterLobbySessionShutdown = false;
        (void)StartAsyncRefresh(false);
    }
}

bool StartAsyncRefresh(bool showStatusMessage)
{
    PumpRefreshResult();
    if (IsRefreshInFlight())
    {
        if (showStatusMessage)
        {
            SetStatusMessage("Public room list is already refreshing.");
        }
        return false;
    }

    const uint32_t generation = g_refreshGeneration.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(g_refreshMutex);
        g_refreshResultReady = false;
        g_refreshResultShowStatus = false;
        g_refreshResultOk = false;
        g_refreshResultGeneration = 0;
        g_refreshResultRooms.clear();
        g_refreshResultError.clear();
    }
    g_refreshInFlight.store(true, std::memory_order_release);

    if (showStatusMessage)
    {
        SetStatusMessage("Refreshing public room list...");
    }

    std::thread([showStatusMessage, generation]() {
        std::vector<netplay::lobby::PublicRoomSummary> rooms;
        std::string error;
        const bool ok = netplay::lobby::ListPublicRooms(&rooms, &error);
        {
            std::lock_guard<std::mutex> lock(g_refreshMutex);
            g_refreshResultReady = true;
            g_refreshResultShowStatus = showStatusMessage;
            g_refreshResultOk = ok;
            g_refreshResultGeneration = generation;
            g_refreshResultRooms = std::move(rooms);
            g_refreshResultError = std::move(error);
        }
    }).detach();

    return true;
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
    g_lobbySessionShutdownInFlight = false;
    g_refreshAfterLobbySessionShutdown = false;
    g_refreshInFlight.store(false, std::memory_order_release);
    g_refreshGeneration.store(1, std::memory_order_release);
    g_refreshResultReady = false;
    g_refreshResultShowStatus = false;
    g_refreshResultOk = false;
    g_refreshResultGeneration = 0;
    ResetHorizontalInputState();
    EnsureSpecInitialized();
}

bool EnterMenu()
{
    EnsureSpecInitialized();
    PumpRefreshResult();
    ResetHorizontalInputState();
    if (g_state.roomCode.empty())
    {
        g_state.roomCode.clear();
    }
    if (g_lobbySessionShutdownInFlight)
    {
        SetStatusMessage("Leaving room...");
        g_refreshAfterLobbySessionShutdown = true;
    }
    else
    {
        (void)StartAsyncRefresh(false);
    }
    RebuildMenuEntries();
    return true;
}

void LeaveMenu()
{
    PumpRefreshResult();
    InvalidateRefreshResults();
    ResetHorizontalInputState();
    g_state.view = BrowserView::Root;
    g_state.rootSelection = 0;
    g_state.joinSelection = 0;
    g_state.createSelection = 0;
    RebuildMenuEntries();
    ClearStatusMessage();
}

void NotifyLobbySessionShutdownStarted()
{
    g_lobbySessionShutdownInFlight = true;
    g_refreshAfterLobbySessionShutdown = true;
    InvalidateRefreshResults();
    if (!HasStatusMessage())
    {
        SetStatusMessage("Leaving room...");
    }
}

void NotifyLobbySessionShutdownCompleted()
{
    g_lobbySessionShutdownInFlight = false;
    if (!g_refreshAfterLobbySessionShutdown)
    {
        return;
    }

    ClearStatusMessage();
    if (IsRefreshInFlight())
    {
        return;
    }

    g_refreshAfterLobbySessionShutdown = false;
    (void)StartAsyncRefresh(false);
}

std::string BuildRowPrimaryText(NetplayMenuAction action)
{
    PumpRefreshResult();
    switch (action)
    {
    case NetplayMenuAction::PlayerRoomsOpenJoin:
        return "Join Room";
    case NetplayMenuAction::PlayerRoomsOpenCreate:
        return "Create Room";
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
    PumpRefreshResult();
    switch (action)
    {
    case NetplayMenuAction::PlayerRoomsOpenJoin:
        if (!g_state.publicRooms.empty())
        {
            return std::to_string(static_cast<int>(g_state.publicRooms.size())) + " listed";
        }
        return {};
    case NetplayMenuAction::PlayerRoomsOpenCreate:
        return GetRoomTypeLabel();
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
    PumpRefreshResult();
    if (HasStatusMessage())
    {
        return g_state.statusMessage;
    }

    if (IsRefreshInFlight())
    {
        if (g_state.publicRooms.empty())
        {
            return "Loading public room list...";
        }
        return "Refreshing public room list...";
    }

    switch (selectedAction)
    {
    case NetplayMenuAction::PlayerRoomsOpenJoin:
        return "Browse public rooms or join by room code.";
    case NetplayMenuAction::PlayerRoomsOpenCreate:
        return "Open room creation settings.";
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
        if (g_state.view == BrowserView::Root)
        {
            return "Return to the netplay main menu.";
        }
        return "Return to the Player Rooms menu.";
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
    PumpRefreshResult();
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
            g_lastHorizontalInput[static_cast<size_t>(playerIndex)] = 0;
            continue;
        }

        if (horizontal == g_lastHorizontalInput[static_cast<size_t>(playerIndex)])
        {
            continue;
        }
        g_lastHorizontalInput[static_cast<size_t>(playerIndex)] = horizontal;

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
        if (g_state.view == BrowserView::Create
            && entry->action == NetplayMenuAction::PlayerRoomsRoomType)
        {
            ToggleRoomType();
            handled = true;
        }
        else if (g_state.view == BrowserView::Join && IsRoomSlotAction(entry->action))
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

bool HandleCancel(uint32_t screenContext)
{
    PumpRefreshResult();
    if (g_state.view == BrowserView::Root)
    {
        return false;
    }

    SwitchView(screenContext, BrowserView::Root);
    return true;
}

bool ExecuteAction(uint32_t screenContext, NetplayMenuAction action)
{
    PumpRefreshResult();

    const bool requiresStableRoomState =
        action == NetplayMenuAction::PlayerRoomsOpenJoin
        || action == NetplayMenuAction::PlayerRoomsOpenCreate
        || action == NetplayMenuAction::PlayerRoomsRefresh
        || action == NetplayMenuAction::PlayerRoomsJoin
        || action == NetplayMenuAction::PlayerRoomsEditCode
        || action == NetplayMenuAction::PlayerRoomsCreate
        || action == NetplayMenuAction::PlayerRoomsRoomType
        || action == NetplayMenuAction::PlayerRoomsSlot0
        || action == NetplayMenuAction::PlayerRoomsSlot1
        || action == NetplayMenuAction::PlayerRoomsSlot2;
    if (g_lobbySessionShutdownInFlight && requiresStableRoomState)
    {
        SetStatusMessage("Leaving room...");
        return true;
    }

    switch (action)
    {
    case NetplayMenuAction::PlayerRoomsOpenJoin:
        SwitchView(screenContext, BrowserView::Join);
        return true;
    case NetplayMenuAction::PlayerRoomsOpenCreate:
        SwitchView(screenContext, BrowserView::Create);
        return true;
    case NetplayMenuAction::PlayerRoomsRefresh:
        StartAsyncRefresh(true);
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
    case NetplayMenuAction::BackToMain:
        if (g_state.view != BrowserView::Root)
        {
            SwitchView(screenContext, BrowserView::Root);
            return true;
        }
        return false;
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

uint8_t GetMenuDetailForStateExport()
{
    switch (g_state.view)
    {
    case BrowserView::Join:
        return static_cast<uint8_t>(EFZ_MENU_DETAIL_PLAYER_ROOMS_JOIN);
    case BrowserView::Create:
        return static_cast<uint8_t>(EFZ_MENU_DETAIL_PLAYER_ROOMS_CREATE);
    case BrowserView::Root:
    default:
        return static_cast<uint8_t>(EFZ_MENU_DETAIL_PLAYER_ROOMS_ROOT);
    }
}
}
