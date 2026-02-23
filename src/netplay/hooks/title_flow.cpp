#include "netplay/hooks/internal/shared.h"
#include "netplay/bridge/session_bridge.h"

#include "logger.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace netplay::hooks::internal
{
using namespace netplay::constants;
using NetplayMenuId = netplay::menu::NetplayMenuId;
using NetplayMenuAction = netplay::menu::NetplayMenuAction;
using NetplayMenuEntry = netplay::menu::NetplayMenuEntry;
using netplay::menu::GetDefaultSelectionForMenu;
using netplay::menu::GetMenuEntries;
using netplay::menu::MenuActionToString;
using netplay::menu::MenuIdToString;
using netplay::menu::RebuildLobbyMenuEntries;
using netplay::menu::RowIndexToString;
using InlineEditInputResult = netplay::inline_edit::InputResult;
using NetbridgeRole = netplay::bridge::NetbridgeRole;
using NetbridgePhase = netplay::bridge::NetbridgePhase;

void EnterNetplayMenu(uint32_t screenContext)
{
    mod::Log("EnterNetplayMenu: request active=%d", g_netplayMenuState.active);
    if (g_netplayMenuState.active)
    {
        mod::Log("EnterNetplayMenu: already active, ignoring duplicate entry");
        return;
    }

    RunTransitionFadeOut(screenContext, 0, 0);

    if (!LoadNetplayAssets(screenContext))
    {
        mod::Log("EnterNetplayMenu: assets load failed, keeping title menu active");
        (void)LoadTitleAssets(screenContext);
        RunTransitionFadeIn(screenContext);
        return;
    }

    LoadNetplayMenuSettingsFromIni();
    ResetTitleMenuState(screenContext, 0);

    g_netplayMenuState.active = true;
    g_netplayMenuState.bgmActive = true;
    g_netplayMenuState.menuId = NetplayMenuId::Main;
    g_netplayMenuState.mainSelection = 0;
    ResetMenuSlideTransition();
    ResetInlineEditState();
    g_lastNetplayFrameLogTick = 0;
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    g_pendingVsHumanAutoConfirm = false;
    SwitchToMenu(screenContext, NetplayMenuId::Main, -1);
    InstallNetplayWindowHook(screenContext);

    auto const playBackgroundMusic = reinterpret_cast<PlayBackgroundMusicFn>(RuntimeAddress(kVaPlayBackgroundMusic));
    playBackgroundMusic(GetGameSystem(screenContext), kNetplayBgmTrack);
    RunTransitionFadeIn(screenContext);
    mod::Log(
        "EnterNetplayMenu: active menu=%s selection=%d bgmTrack=%u configStyle=%d optionCount=%d backIndex=%d",
        MenuIdToString(g_netplayMenuState.menuId),
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)),
        kNetplayBgmTrack,
        g_netplayMenuState.useConfigStyleRender,
        g_netplayMenuState.optionCount,
        g_netplayMenuState.backIndex);
}

void LeaveNetplayMenu(uint32_t screenContext)
{
    mod::Log("LeaveNetplayMenu: request active=%d bgmActive=%d", g_netplayMenuState.active, g_netplayMenuState.bgmActive);
    if (!g_netplayMenuState.active)
    {
        mod::Log("LeaveNetplayMenu: already inactive");
        return;
    }

    RunTransitionFadeOut(screenContext, 0, 0);

    if (g_netplayMenuState.bgmActive)
    {
        StopCurrentBgm(screenContext, "leave_netplay");
    }

    netplay::bridge::CancelSession("leave_menu");

    g_netplayMenuState.active = false;
    g_netplayMenuState.bgmActive = false;
    g_netplayMenuState.useConfigStyleRender = false;
    g_netplayMenuState.menuId = NetplayMenuId::Main;
    g_netplayMenuState.mainSelection = 0;
    g_netplayMenuState.optionCount = kNetplayDefaultOptionCount;
    g_netplayMenuState.backIndex = kNetplayDefaultBackIndex;
    g_netplayMenuState.renderLayout = {};
    g_netplayMenuState.lobbyScrollOffset = 0;
    ResetMenuSlideTransition();
    ResetInlineEditState();
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = false;
    g_pendingVsHumanAutoConfirm = false;
    RemoveNetplayWindowHook();

    (void)LoadTitleAssets(screenContext);
    ResetTitleMenuState(screenContext, 5);
    RunTransitionFadeIn(screenContext);
    mod::Log(
        "LeaveNetplayMenu: returned to title assets, titleSelection=%d",
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
}

void HandoffConnectedSessionToTitle(uint32_t screenContext)
{
    mod::Log("HandoffConnectedSessionToTitle: begin");

    RunTransitionFadeOut(screenContext, 0, 0);

    if (g_netplayMenuState.bgmActive)
    {
        StopCurrentBgm(screenContext, "handoff_connected_session");
    }

    g_netplayMenuState.active = false;
    g_netplayMenuState.bgmActive = false;
    g_netplayMenuState.useConfigStyleRender = false;
    g_netplayMenuState.menuId = NetplayMenuId::Main;
    g_netplayMenuState.mainSelection = 0;
    g_netplayMenuState.optionCount = kNetplayDefaultOptionCount;
    g_netplayMenuState.backIndex = kNetplayDefaultBackIndex;
    g_netplayMenuState.renderLayout = {};
    g_netplayMenuState.lobbyScrollOffset = 0;
    ResetMenuSlideTransition();
    ResetInlineEditState();
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = false;
    RemoveNetplayWindowHook();

    (void)LoadTitleAssets(screenContext);
    ResetTitleMenuState(screenContext, 2);
    g_pendingVsHumanAutoConfirm = true;

    RunTransitionFadeIn(screenContext);

    const uint8_t currentState = *reinterpret_cast<uint8_t*>(RuntimeAddress(kVaCurrentScreenIndex));
    mod::Log(
        "HandoffConnectedSessionToTitle: armed auto-confirm selection=%d state=%u",
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)),
        static_cast<unsigned>(currentState));
}

void SwitchToMenu(uint32_t screenContext, NetplayMenuId menuId, int selection)
{
    if (g_inlineEditState.active)
    {
        CancelInlineEdit();
    }
    // Lobby session lifecycle: destroy when navigating away, create when entering.
    if (g_netplayMenuState.menuId == NetplayMenuId::Lobby && menuId != NetplayMenuId::Lobby)
    {
        if (g_lobbySession)
        {
            mod::Log("SwitchToMenu: leaving Lobby, resetting lobby session");
            g_lobbySession.reset();
        }
        g_netplayMenuState.lobbyScrollOffset = 0;
    }
    g_netplayMenuState.menuId = menuId;
    if (menuId == NetplayMenuId::Lobby && !g_lobbySession)
    {
        // Seed the dynamic entry list with 0 idle players before any spec
        // queries so that GetCurrentMenuEntryCount() returns a valid count.
        RebuildLobbyMenuEntries(0, 0);
        g_netplayMenuState.lobbyScrollOffset = 0;
        mod::Log("SwitchToMenu: entering Lobby, creating session for '%s' port=%u",
            g_netplayMenuState.nickname.c_str(),
            static_cast<unsigned>(g_netplayMenuState.hostPort));
        g_lobbySession = std::make_unique<netplay::lobby::LobbySession>(
            g_netplayMenuState.nickname,
            g_netplayMenuState.hostPort);
    }
    int requestedSelection = selection;
    if (requestedSelection < 0)
    {
        requestedSelection = GetDefaultSelectionForMenu(menuId);
    }
    const int clamped = ClampSelectionToCurrentMenu(requestedSelection);
    g_netplayMenuState.optionCount = GetCurrentMenuEntryCount();
    g_netplayMenuState.backIndex = g_netplayMenuState.optionCount > 0 ? g_netplayMenuState.optionCount - 1 : 0;

    *reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection) = static_cast<int8_t>(clamped);
    *reinterpret_cast<uint16_t*>(screenContext + kOffsetMenuAnimCounter) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
    *reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter) = 0;
    g_lastLoggedSelection = static_cast<int8_t>(clamped);

    const NetplayMenuEntry* entry = GetCurrentMenuEntry(clamped);
    const int rowIndex = GetRenderRowForSelection(clamped);
    mod::Log(
        "NetplayMenuSwitch: menu=%s selection=%d count=%d row=%d(%s) entry=%s",
        MenuIdToString(menuId),
        clamped,
        g_netplayMenuState.optionCount,
        rowIndex,
        RowIndexToString(rowIndex),
        entry != nullptr ? entry->debugLabel : "none");
}

void ShowStubActionMessage(HWND owner, const std::string& message)
{
    MessageBoxA(owner, message.c_str(), "Netplay", MB_OK | MB_ICONINFORMATION);
}

void ExecuteNetplayAction(uint32_t screenContext, NetplayMenuAction action, int logicalSelection)
{
    const HWND owner = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
    const int selectedRow = GetRenderRowForSelection(logicalSelection);
    mod::Log(
        "NetplayAction: menu=%s selection=%d row=%d(%s) action=%s",
        MenuIdToString(g_netplayMenuState.menuId),
        logicalSelection,
        selectedRow,
        RowIndexToString(selectedRow),
        MenuActionToString(action));

    switch (action)
    {
    case NetplayMenuAction::OpenHost:
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Host, -1, +1);
        break;
    case NetplayMenuAction::OpenJoin:
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Join, -1, +1);
        break;
    case NetplayMenuAction::OpenNickname:
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Nickname, -1, +1);
        break;
    case NetplayMenuAction::OpenLobby:
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Lobby, -1, +1);
        break;
    case NetplayMenuAction::BackToMain:
        StartMenuSlideTransition(screenContext, NetplayMenuId::Main, g_netplayMenuState.mainSelection, -1);
        break;
    case NetplayMenuAction::LeaveNetplay:
        LeaveNetplayMenu(screenContext);
        break;
    case NetplayMenuAction::HostEditPort:
        BeginInlineEdit(NetplayMenuAction::HostEditPort);
        break;
    case NetplayMenuAction::JoinEditAddress:
        BeginInlineEdit(NetplayMenuAction::JoinEditAddress);
        break;
    case NetplayMenuAction::JoinEditPort:
        BeginInlineEdit(NetplayMenuAction::JoinEditPort);
        break;
    case NetplayMenuAction::NicknameEdit:
        BeginInlineEdit(NetplayMenuAction::NicknameEdit);
        break;
    case NetplayMenuAction::HostStart:
    {
        const bool started = netplay::bridge::StartSession(
            NetbridgeRole::Host,
            g_netplayMenuState.hostPort,
            "",
            g_netplayMenuState.nickname.c_str());
        if (!started)
        {
            const netplay::bridge::NetbridgeStatus status = netplay::bridge::GetStatus();
            char text[320] = {};
            snprintf(
                text,
                sizeof(text),
                "Host start failed.\n\n%s",
                status.errorMsg[0] != '\0' ? status.errorMsg : "Unknown error");
            ShowStubActionMessage(owner, text);
        }
        break;
    }
    case NetplayMenuAction::JoinConnect:
    {
        const bool started = netplay::bridge::StartSession(
            NetbridgeRole::Join,
            g_netplayMenuState.joinPort,
            g_netplayMenuState.joinAddress.c_str(),
            g_netplayMenuState.nickname.c_str());
        if (!started)
        {
            const netplay::bridge::NetbridgeStatus status = netplay::bridge::GetStatus();
            char text[320] = {};
            snprintf(
                text,
                sizeof(text),
                "Join start failed.\n\n%s",
                status.errorMsg[0] != '\0' ? status.errorMsg : "Unknown error");
            ShowStubActionMessage(owner, text);
        }
        break;
    }
    case NetplayMenuAction::LobbyPlaying0:
    {
        if (!g_lobbySession)
        {
            ShowStubActionMessage(owner, "Lobby status unavailable.");
            break;
        }

        const auto status = g_lobbySession->GetStatus();
        if (status.playing.empty() || status.playing[0].hostIp.empty())
        {
            ShowStubActionMessage(owner, "No active match host information is available for spectating yet.");
            break;
        }

        const bool started = netplay::bridge::StartSession(
            NetbridgeRole::Spectate,
            g_netplayMenuState.joinPort,
            status.playing[0].hostIp.c_str(),
            "");
        if (!started)
        {
            const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
            char text[320] = {};
            snprintf(
                text,
                sizeof(text),
                "Spectate start failed.\n\n%s",
                bridgeStatus.errorMsg[0] != '\0' ? bridgeStatus.errorMsg : "Unknown error");
            ShowStubActionMessage(owner, text);
        }
        break;
    }
    case NetplayMenuAction::LobbySlot0:
    case NetplayMenuAction::LobbySlot1:
    case NetplayMenuAction::LobbySlot2:
    case NetplayMenuAction::LobbySlot3:
    case NetplayMenuAction::LobbySlot4:
    case NetplayMenuAction::LobbySlot5:
    {
        const int visSlot = static_cast<int>(action) - static_cast<int>(NetplayMenuAction::LobbySlot0);
        const int realSlot = visSlot + g_netplayMenuState.lobbyScrollOffset;
        std::string targetName;
        if (g_lobbySession)
        {
            const auto status = g_lobbySession->GetStatus();
            if (realSlot < static_cast<int>(status.idlePlayers.size()))
            {
                targetName = status.idlePlayers[realSlot].name;
            }
        }

        const bool started = netplay::bridge::StartSession(
            NetbridgeRole::Join,
            g_netplayMenuState.joinPort,
            g_netplayMenuState.joinAddress.c_str(),
            g_netplayMenuState.nickname.c_str());
        if (!started)
        {
            const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
            char text[320] = {};
            if (targetName.empty())
            {
                snprintf(
                    text,
                    sizeof(text),
                    "Challenge start failed.\n\n%s",
                    bridgeStatus.errorMsg[0] != '\0' ? bridgeStatus.errorMsg : "Unknown error");
            }
            else
            {
                snprintf(
                    text,
                    sizeof(text),
                    "Challenge start failed for '%s'.\n\n%s",
                    targetName.c_str(),
                    bridgeStatus.errorMsg[0] != '\0' ? bridgeStatus.errorMsg : "Unknown error");
            }
            ShowStubActionMessage(owner, text);
        }
        break;
    }
    default:
        break;
    }
}

char UpdateNetplayMenu(uint32_t screenContext)
{
    ++g_netplayUpdateCallCount;
    auto const render = GetOriginalTitleRender();
    auto const processInput = reinterpret_cast<ProcessPlayerInputFn>(RuntimeAddress(kVaProcessPlayerInput));

    AdvanceMenuSlideTransition(screenContext);
    if (g_useRuntimeTextOverlay)
    {
        (void)RenderNetplayMenuRuntimeText(screenContext);
    }
    else if (g_netplayMenuState.useConfigStyleRender)
    {
        (void)RenderNetplayMenuConfigStyle(screenContext);
    }
    else
    {
        (void)render(screenContext);
    }

    const int gameSystem = GetGameSystem(screenContext);
    processInput(reinterpret_cast<int*>(gameSystem));
    netplay::bridge::Tick();
    auto* const inputBytes = reinterpret_cast<uint8_t*>(gameSystem);
    InputSnapshot currentSnapshot = {
        static_cast<int8_t>(inputBytes[12]),
        static_cast<int8_t>(inputBytes[14]),
        inputBytes[16],
        inputBytes[18],
        static_cast<int8_t>(inputBytes[13]),
        static_cast<int8_t>(inputBytes[15]),
        inputBytes[17],
        inputBytes[19],
    };

    auto* const selectionPtr = reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection);
    auto* const inactivityCounter = reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter);

    if (!g_hasLoggedInputSnapshot
        || std::memcmp(&currentSnapshot, &g_lastInputSnapshot, sizeof(InputSnapshot)) != 0)
    {
        mod::Log(
            "NetplayInput: P1(h=%d v=%d c=%u b=%u) P2(h=%d v=%d c=%u b=%u)",
            static_cast<int>(currentSnapshot.p1Horizontal),
            static_cast<int>(currentSnapshot.p1Vertical),
            static_cast<unsigned>(currentSnapshot.p1Confirm),
            static_cast<unsigned>(currentSnapshot.p1Cancel),
            static_cast<int>(currentSnapshot.p2Horizontal),
            static_cast<int>(currentSnapshot.p2Vertical),
            static_cast<unsigned>(currentSnapshot.p2Confirm),
            static_cast<unsigned>(currentSnapshot.p2Cancel));
        g_lastInputSnapshot = currentSnapshot;
        g_hasLoggedInputSnapshot = true;
    }

    const DWORD nowTick = GetTickCount();
    if (g_lastNetplayFrameLogTick == 0 || nowTick - g_lastNetplayFrameLogTick >= kNetplayFrameLogIntervalMs)
    {
        const int currentRow = GetRenderRowForSelection(static_cast<int>(*selectionPtr));
        const int nativeSlideYRaw = *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY);
        const int nativeSlideYScaled = GetScaledNativeSlideY(screenContext);
        mod::Log(
            "NetplayFrame: updates=%llu menu=%s selection=%d row=%d(%s) inactivity=%u slide=%d nativeSlideRaw=%d nativeSlideScaled=%d",
            static_cast<unsigned long long>(g_netplayUpdateCallCount),
            MenuIdToString(g_netplayMenuState.menuId),
            static_cast<int>(*selectionPtr),
            currentRow,
            RowIndexToString(currentRow),
            *inactivityCounter,
            IsMenuSlideTransitionActive() ? 1 : 0,
            nativeSlideYRaw,
            nativeSlideYScaled);
        g_lastNetplayFrameLogTick = nowTick;
    }

    if (IsMenuSlideTransitionActive())
    {
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    // Lobby: rebuild dynamic entries each frame so that the visible row count
    // tracks the actual number of idle players.  Also clamp the scroll offset
    // and sync optionCount / backIndex.
    if (g_netplayMenuState.menuId == NetplayMenuId::Lobby)
    {
        int idleCount = 0;
        int playingCount = 0;
        if (g_lobbySession)
        {
            const auto lobSt = g_lobbySession->GetStatus();
            if (lobSt.pollState == netplay::lobby::PollState::Polling)
            {
                idleCount    = static_cast<int>(lobSt.idlePlayers.size());
                playingCount = static_cast<int>(lobSt.playing.size());
            }
        }
        RebuildLobbyMenuEntries(idleCount, playingCount);

        // Clamp scroll so we never point past the end of the player list.
        const int visSlots = std::min(idleCount, netplay::menu::kLobbyMaxDisplayPlayers);
        const int maxScroll = std::max(0, idleCount - visSlots);
        if (g_netplayMenuState.lobbyScrollOffset > maxScroll)
        {
            g_netplayMenuState.lobbyScrollOffset = maxScroll;
        }

        // Sync optionCount / backIndex from the freshly rebuilt spec.
        const int newCount = GetCurrentMenuEntryCount();
        if (newCount != g_netplayMenuState.optionCount && newCount > 0)
        {
            g_netplayMenuState.optionCount = newCount;
            g_netplayMenuState.backIndex   = newCount - 1;
            const int clampedSel = ClampSelectionToCurrentMenu(static_cast<int>(*selectionPtr));
            *selectionPtr = static_cast<int8_t>(clampedSel);
        }
    }

    // Lobby: allow R key to trigger an immediate poll refresh.
    if (g_netplayMenuState.menuId == NetplayMenuId::Lobby
        && g_lobbySession
        && (GetAsyncKeyState('R') & 0x0001) != 0)
    {
        g_lobbySession->RequestRefresh();
    }

    const int entryCount = GetCurrentMenuEntryCount();
    if (entryCount <= 0)
    {
        SwitchToMenu(screenContext, NetplayMenuId::Main, -1);
        return 0;
    }

    if (HandleInlineEditInput(screenContext, inputBytes))
    {
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        *inactivityCounter = 0;
        return 0;
    }

    const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
    const NetbridgePhase bridgePhase = static_cast<NetbridgePhase>(bridgeStatus.phase);
    if (bridgePhase == NetbridgePhase::Connected)
    {
        mod::Log("NetplayBridge: connected; handing off to title for character select transition");
        HandoffConnectedSessionToTitle(screenContext);
        return 0;
    }

    if (bridgePhase == NetbridgePhase::Connecting)
    {
        bool cancelRequested = ConsumeNetplayEscapeEdge();
        for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
        {
            if (inputBytes[playerIndex + 18] == 1)
            {
                cancelRequested = true;
                break;
            }
        }

        if (cancelRequested)
        {
            PlayUiSound(screenContext, kSfxConfirm);
            netplay::bridge::CancelSession("user_cancel");
            mod::Log("NetplayBridge: cancel requested during connecting");
        }

        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    if (ConsumeNetplayEscapeEdge())
    {
        PlayUiSound(screenContext, kSfxConfirm);
        mod::Log(
            "NetplayCancel: keyboard=ESC menu=%s selection=%d",
            MenuIdToString(g_netplayMenuState.menuId),
            static_cast<int>(*selectionPtr));

        if (g_netplayMenuState.menuId == NetplayMenuId::Main)
        {
            LeaveNetplayMenu(screenContext);
        }
        else
        {
            StartMenuSlideTransition(screenContext, NetplayMenuId::Main, g_netplayMenuState.mainSelection, -1);
        }
        return 0;
    }

    bool hadDirectionalInput = false;

    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch = reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1 + playerIndex);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);

        if (vertical != 0)
        {
            hadDirectionalInput = true;
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                PlayUiSound(screenContext, kSfxMove);
                const int current = ClampSelectionToCurrentMenu(static_cast<int>(*selectionPtr));
                const int delta = vertical > 0 ? 1 : -1;
                int next = (current + delta + entryCount) % entryCount;

                // Lobby: intercept boundary movement to scroll the player list
                // instead of wrapping when more entries exist off-screen.
                if (g_netplayMenuState.menuId == NetplayMenuId::Lobby && g_lobbySession)
                {
                    const auto lobSt = g_lobbySession->GetStatus();
                    const int idleCount = static_cast<int>(lobSt.idlePlayers.size());
                    const int visSlots  = std::min(idleCount, netplay::menu::kLobbyMaxDisplayPlayers);

                    if (delta > 0 && current == visSlots - 1 && next == visSlots)
                    {
                        // Moving down from the last visible slot: scroll if more
                        // players are below, otherwise fall through to LobbyPlaying0.
                        const int maxScroll = std::max(0, idleCount - visSlots);
                        if (g_netplayMenuState.lobbyScrollOffset < maxScroll)
                        {
                            g_netplayMenuState.lobbyScrollOffset++;
                            next = current; // stay at last visible slot
                        }
                    }
                    else if (delta < 0 && current == 0 && next == entryCount - 1)
                    {
                        // Moving up from the first slot: scroll back if possible,
                        // otherwise fall through to BackToMain (wrap).
                        if (g_netplayMenuState.lobbyScrollOffset > 0)
                        {
                            g_netplayMenuState.lobbyScrollOffset--;
                            next = current; // stay at first slot
                        }
                    }
                }

                const NetplayMenuEntry* nextEntry = GetCurrentMenuEntry(next);
                mod::Log(
                    "NetplaySelection: player=%d menu=%s from=%d to=%d row=%d(%s) label=%s inputV=%d",
                    playerIndex,
                    MenuIdToString(g_netplayMenuState.menuId),
                    current,
                    next,
                    nextEntry != nullptr ? nextEntry->renderRow : -1,
                    nextEntry != nullptr ? RowIndexToString(nextEntry->renderRow) : "ROW_UNKNOWN",
                    nextEntry != nullptr ? nextEntry->debugLabel : "none",
                    static_cast<int>(vertical));
                *selectionPtr = static_cast<int8_t>(next);
                *reinterpret_cast<uint16_t*>(screenContext + kOffsetMenuAnimCounter) = 0;
                *inputLatch = 1;
                g_lastLoggedSelection = *selectionPtr;
            }
        }
        else
        {
            *inputLatch = 0;
        }

        if (inputBytes[playerIndex + 16] == 1)
        {
            const int logicalSelection = ClampSelectionToCurrentMenu(static_cast<int>(*selectionPtr));
            const NetplayMenuEntry* selectedEntry = GetCurrentMenuEntry(logicalSelection);
            if (selectedEntry == nullptr)
            {
                return 0;
            }

            PlayUiSound(screenContext, kSfxConfirm);
            mod::Log(
                "NetplayConfirm: player=%d menu=%s selection=%d row=%d(%s) label=%s action=%s",
                playerIndex,
                MenuIdToString(g_netplayMenuState.menuId),
                logicalSelection,
                selectedEntry->renderRow,
                RowIndexToString(selectedEntry->renderRow),
                selectedEntry->debugLabel,
                MenuActionToString(selectedEntry->action));
            ExecuteNetplayAction(screenContext, selectedEntry->action, logicalSelection);
            return 0;
        }

        if (inputBytes[playerIndex + 18] == 1)
        {
            PlayUiSound(screenContext, kSfxConfirm);
            mod::Log(
                "NetplayCancel: player=%d menu=%s selection=%d",
                playerIndex,
                MenuIdToString(g_netplayMenuState.menuId),
                static_cast<int>(*selectionPtr));

            if (g_netplayMenuState.menuId == NetplayMenuId::Main)
            {
                LeaveNetplayMenu(screenContext);
            }
            else
            {
                StartMenuSlideTransition(screenContext, NetplayMenuId::Main, g_netplayMenuState.mainSelection, -1);
            }
            return 0;
        }
    }

    if (!hadDirectionalInput)
    {
        ++(*inactivityCounter);
    }

    return 0;
}

void TriggerNetplayMenuEntry(uint32_t screenContext)
{
    mod::Log(
        "TriggerNetplayMenuEntry: titleSelection=%d screenContext=0x%08X",
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)),
        screenContext);
    PlayUiSound(screenContext, kSfxConfirm);
    EnterNetplayMenu(screenContext);
}
} // namespace netplay::hooks::internal
