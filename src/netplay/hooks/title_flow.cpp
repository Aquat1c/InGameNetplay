#include "netplay/hooks/internal/shared.h"
#include "netplay/bridge/session_bridge.h"

#include "logger.h"

#include <algorithm>
#include <cmath>
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

void HandoffConnectedSessionToVsHumanState(uint32_t screenContext);

namespace
{
constexpr int kDelaySelectionMin = 0;
constexpr int kDelaySelectionMax = 20;
constexpr int kConnectedPreHandoffDelayFrames = 18;
int g_pendingGlobalStateTransition = -1;
constexpr uint32_t kGameSystemOffsetCpuFlagP1 = 4931;
constexpr uint32_t kGameSystemOffsetCpuFlagP2 = 4932;
constexpr uint32_t kGameSystemOffsetRoundsCurrent = 4942;
constexpr uint32_t kGameSystemOffsetRoundsSetting = 4943;
constexpr uint32_t kGameSystemOffsetMode = 4964;
constexpr uint32_t kGameSystemOffsetSecondaryModeFlag = 4965;
constexpr uint32_t kGameSystemOffsetReplaySessionFlag = 82563;
constexpr uint8_t kGameModeVsHuman = 4;
constexpr uint8_t kSecondaryModeFlagVsHuman = 4;
constexpr uint8_t kReplaySessionFlagCleared = 0;

void CopyBoundedText(char* dst, size_t dstSize, const char* src)
{
    if (dst == nullptr || dstSize == 0)
    {
        return;
    }
    if (src == nullptr)
    {
        dst[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(dst, dstSize, src, _TRUNCATE);
#else
    std::snprintf(dst, dstSize, "%s", src);
#endif
}

void ResetDelaySetupOverlayState()
{
    g_delaySetupOverlay = {};
}

void ResetSpectateConfirmOverlayState()
{
    g_spectateConfirmOverlay = {};
}

void ActivateSpectateConfirmOverlay()
{
    ResetSpectateConfirmOverlayState();
    g_spectateConfirmOverlay.active = true;
    g_spectateConfirmOverlay.selectedOption = 0; // default to Yes
    mod::Log("SpectateConfirmOverlay: activated");
}

bool HandleSpectateConfirmOverlayInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter)
{
    if (!g_spectateConfirmOverlay.active || inputBytes == nullptr || inactivityCounter == nullptr)
    {
        return false;
    }

    bool cancelRequested = ConsumeNetplayEscapeEdge();
    bool confirmRequested = false;

    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch = reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1 + playerIndex);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);

        int step = 0;
        if (vertical > 0)
        {
            step = 1;
        }
        else if (vertical < 0)
        {
            step = -1;
        }

        if (step != 0)
        {
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                const int oldOption = g_spectateConfirmOverlay.selectedOption;
                int newOption = oldOption + step;
                if (newOption < 0) newOption = 1;
                if (newOption > 1) newOption = 0;
                if (newOption != oldOption)
                {
                    g_spectateConfirmOverlay.selectedOption = newOption;
                    PlayUiSound(screenContext, kSfxMove);
                }
                *inputLatch = 1;
            }
        }
        else
        {
            *inputLatch = 0;
        }

        if (inputBytes[playerIndex + 16] == 1)
        {
            confirmRequested = true;
        }
        if (inputBytes[playerIndex + 18] == 1)
        {
            cancelRequested = true;
        }
    }

    ++(*inactivityCounter);

    if (cancelRequested)
    {
        PlayUiSound(screenContext, kSfxConfirm);
        ResetSpectateConfirmOverlayState();
        netplay::bridge::AnswerSpectateConfirm(false);
        netplay::bridge::CancelSession("spectate_declined");
        mod::Log("SpectateConfirmOverlay: canceled (escape)");
        return true;
    }

    if (confirmRequested)
    {
        const bool accepted = (g_spectateConfirmOverlay.selectedOption == 0); // 0 = Yes
        PlayUiSound(screenContext, kSfxConfirm);

        const bool answered = netplay::bridge::AnswerSpectateConfirm(accepted);
        if (!answered)
        {
            const auto status = netplay::bridge::GetStatus();
            if (status.errorMsg[0] != '\0')
            {
                std::snprintf(
                    g_spectateConfirmOverlay.errorMessage,
                    sizeof(g_spectateConfirmOverlay.errorMessage),
                    "%s",
                    status.errorMsg);
            }
            else
            {
                std::snprintf(
                    g_spectateConfirmOverlay.errorMessage,
                    sizeof(g_spectateConfirmOverlay.errorMessage),
                    "Failed to send answer");
            }
            mod::Log("SpectateConfirmOverlay: answer failed accepted=%d", accepted ? 1 : 0);
            return true;
        }

        mod::Log("SpectateConfirmOverlay: answered accepted=%d", accepted ? 1 : 0);
        ResetSpectateConfirmOverlayState();

        if (!accepted)
        {
            netplay::bridge::CancelSession("spectate_declined");
        }
        // If accepted, the session continues — Revival will proceed to delay
        // setup or straight to game. The normal delay/connected flow handles it.
        return true;
    }

    return true;
}

int ClampDelaySelection(int value)
{
    if (value < kDelaySelectionMin)
    {
        return kDelaySelectionMin;
    }
    if (value > kDelaySelectionMax)
    {
        return kDelaySelectionMax;
    }
    return value;
}

// Mirrors Revival's CalculateDelayDelta.
int CalculateDelayDeltaLikeRevival(float rttMs, float currentDelayFrames)
{
    if (rttMs < 0.0f)
    {
        rttMs = 0.0f;
    }

    const double frameTimeMs = 1000.0 / 64.0;
    const double roundTripInFrames = static_cast<double>(rttMs) / (frameTimeMs * 2.0);
    const double framesNeeded = std::ceil(roundTripInFrames);
    const double delta = framesNeeded - static_cast<double>(currentDelayFrames);
    const double clamped = (delta > 0.0) ? delta : 0.0;
    return static_cast<int>(clamped);
}

// Mirrors Revival's CalculateRecommendedDelay.
int CalculateRecommendedDelayLikeRevival(int delayFloor, float rttMs, float currentDelayFrames)
{
    if (rttMs < 0.0f)
    {
        rttMs = 0.0f;
    }

    const float minDelay = (std::min)(3.0f, currentDelayFrames);
    const double roundTripMs = (1000.0 / 64.0) * 2.0;
    const double rttInFrames = static_cast<double>(rttMs) / roundTripMs;
    const double framesNeeded = std::ceil(rttInFrames);
    const double rawDelta = std::ceil(framesNeeded - static_cast<double>(minDelay));
    const int delta = static_cast<int>(rawDelta);
    int best = (std::max)(delayFloor, delta);
    if (currentDelayFrames == minDelay)
    {
        ++best;
    }
    return best;
}

void ActivateDelaySetupOverlay(const netplay::bridge::NetbridgeStatus& bridgeStatus)
{
    ResetDelaySetupOverlayState();
    g_delaySetupOverlay.active = true;
    g_delaySetupOverlay.waitingForRuntimeReady = false;
    g_delaySetupOverlay.maxDelay = kDelaySelectionMax;
    g_delaySetupOverlay.pingMs = bridgeStatus.pingMs;

    const int currentDelay = ClampDelaySelection((bridgeStatus.rollbackFrames >= 0) ? bridgeStatus.rollbackFrames : 0);
    g_delaySetupOverlay.currentDelay = currentDelay;

    const netplay::bridge::DelayPromptMetrics promptMetrics = netplay::bridge::GetDelayPromptMetrics();
    int minDelay = 0;
    int recommendedDelay = currentDelay;
    int maxDelay = kDelaySelectionMax;
    if (promptMetrics.serial > 0)
    {
        if (promptMetrics.averagePingMs >= 0)
        {
            g_delaySetupOverlay.pingMs = promptMetrics.averagePingMs;
        }
        if (promptMetrics.minDelay >= kDelaySelectionMin && promptMetrics.minDelay <= kDelaySelectionMax)
        {
            minDelay = promptMetrics.minDelay;
        }
        if (promptMetrics.maxDelay >= kDelaySelectionMin && promptMetrics.maxDelay <= kDelaySelectionMax)
        {
            maxDelay = promptMetrics.maxDelay;
        }
        if (promptMetrics.recommendedDelay >= kDelaySelectionMin && promptMetrics.recommendedDelay <= kDelaySelectionMax)
        {
            recommendedDelay = promptMetrics.recommendedDelay;
        }
    }
    if (bridgeStatus.pingMs >= 0)
    {
        const int fallbackMin = CalculateDelayDeltaLikeRevival(static_cast<float>(bridgeStatus.pingMs), static_cast<float>(currentDelay));
        const int fallbackRecommended =
            CalculateRecommendedDelayLikeRevival(fallbackMin, static_cast<float>(bridgeStatus.pingMs), static_cast<float>(currentDelay));
        if (promptMetrics.serial <= 0)
        {
            minDelay = fallbackMin;
            recommendedDelay = fallbackRecommended;
        }
    }

    if (maxDelay < minDelay)
    {
        maxDelay = minDelay;
    }

    g_delaySetupOverlay.minDelay = ClampDelaySelection(minDelay);
    g_delaySetupOverlay.maxDelay = ClampDelaySelection(maxDelay);
    g_delaySetupOverlay.recommendedDelay =
        (std::min)(g_delaySetupOverlay.maxDelay, ClampDelaySelection((std::max)(recommendedDelay, g_delaySetupOverlay.minDelay)));
    g_delaySetupOverlay.selectedDelay = g_delaySetupOverlay.recommendedDelay;
    if (bridgeStatus.p1Name[0] != '\0' && bridgeStatus.p2Name[0] != '\0')
    {
        CopyBoundedText(g_delaySetupOverlay.p1Name, sizeof(g_delaySetupOverlay.p1Name), bridgeStatus.p1Name);
        CopyBoundedText(g_delaySetupOverlay.p2Name, sizeof(g_delaySetupOverlay.p2Name), bridgeStatus.p2Name);
    }
    else
    {
        g_delaySetupOverlay.p1Name[0] = '\0';
        g_delaySetupOverlay.p2Name[0] = '\0';
    }

    mod::Log(
        "DelayOverlay: activated promptSerial=%d ping=%d current=%d min=%d max=%d recommended=%d names='%s' vs '%s'",
        promptMetrics.serial,
        g_delaySetupOverlay.pingMs,
        g_delaySetupOverlay.currentDelay,
        g_delaySetupOverlay.minDelay,
        g_delaySetupOverlay.maxDelay,
        g_delaySetupOverlay.recommendedDelay,
        g_delaySetupOverlay.p1Name,
        g_delaySetupOverlay.p2Name);
}

bool HandleDelaySetupOverlayInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter)
{
    if (!g_delaySetupOverlay.active || inputBytes == nullptr || inactivityCounter == nullptr)
    {
        return false;
    }

    if (g_delaySetupOverlay.waitingForRuntimeReady)
    {
        const auto statusWhileWaiting = netplay::bridge::GetStatus();
        const DWORD nowTick = GetTickCount();
        if (statusWhileWaiting.localInitApplied != 0
            && nowTick >= g_delaySetupOverlay.nextHandoffRetryTick
            && (!g_delaySetupOverlay.vsHumanSyncArmed || statusWhileWaiting.roleFlag != 2))
        {
            const bool wasArmed = g_delaySetupOverlay.vsHumanSyncArmed;
            const bool prepared = netplay::bridge::PrepareVsHumanHandoff();
            if (prepared)
            {
                g_delaySetupOverlay.vsHumanSyncArmed = true;
                if (!wasArmed)
                {
                    mod::Log(
                        "DelayOverlay: handoff armed localInit=%d role=%d phase=%s",
                        statusWhileWaiting.localInitApplied,
                        statusWhileWaiting.roleFlag,
                        netplay::bridge::PhaseToString(static_cast<NetbridgePhase>(statusWhileWaiting.phase)));
                }
            }
            g_delaySetupOverlay.nextHandoffRetryTick = nowTick + 1000;
        }

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
            ResetDelaySetupOverlayState();
            ResetSpectateConfirmOverlayState();
            netplay::bridge::CancelSession("user_cancel");
            mod::Log("DelayOverlay: canceled while waiting for runtime sync");
            return true;
        }

        ++(*inactivityCounter);
        return true;
    }

    bool cancelRequested = ConsumeNetplayEscapeEdge();
    bool confirmRequested = false;
    int confirmPlayer = -1;
    bool moved = false;

    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch = reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1 + playerIndex);
        const int8_t horizontal = static_cast<int8_t>(inputBytes[playerIndex + 12]);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);

        int step = 0;
        if (horizontal > 0 || vertical > 0)
        {
            step = 1;
        }
        else if (horizontal < 0 || vertical < 0)
        {
            step = -1;
        }

        if (step != 0)
        {
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                const int oldValue = g_delaySetupOverlay.selectedDelay;
                const int nextValue = oldValue + step;
                g_delaySetupOverlay.selectedDelay =
                    (std::max)(g_delaySetupOverlay.minDelay, (std::min)(g_delaySetupOverlay.maxDelay, nextValue));
                if (g_delaySetupOverlay.selectedDelay != oldValue)
                {
                    g_delaySetupOverlay.errorMessage[0] = '\0';
                    PlayUiSound(screenContext, kSfxMove);
                    moved = true;
                    mod::Log(
                        "DelayOverlay: selection player=%d from=%d to=%d step=%d",
                        playerIndex,
                        oldValue,
                        g_delaySetupOverlay.selectedDelay,
                        step);
                }
                *inputLatch = 1;
            }
        }
        else
        {
            *inputLatch = 0;
        }

        if (inputBytes[playerIndex + 16] == 1)
        {
            confirmRequested = true;
            confirmPlayer = playerIndex;
        }
        if (inputBytes[playerIndex + 18] == 1)
        {
            cancelRequested = true;
        }
    }

    if (!moved)
    {
        ++(*inactivityCounter);
    }

    if (cancelRequested)
    {
        PlayUiSound(screenContext, kSfxConfirm);
        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        netplay::bridge::CancelSession("user_cancel");
        mod::Log("DelayOverlay: canceled");
        return true;
    }

    if (confirmRequested)
    {
        const int selectedDelay = g_delaySetupOverlay.selectedDelay;
        const bool applied = netplay::bridge::ApplyInputDelay(selectedDelay);
        if (!applied)
        {
            PlayUiSound(screenContext, kSfxMove);
            const auto status = netplay::bridge::GetStatus();
            if (status.errorMsg[0] != '\0')
            {
                std::snprintf(
                    g_delaySetupOverlay.errorMessage,
                    sizeof(g_delaySetupOverlay.errorMessage),
                    "Delay apply failed: %s",
                    status.errorMsg);
            }
            else
            {
                std::snprintf(
                    g_delaySetupOverlay.errorMessage,
                    sizeof(g_delaySetupOverlay.errorMessage),
                    "Delay apply failed");
            }
            mod::Log("DelayOverlay: confirm failed selected=%d", selectedDelay);
            return true;
        }

        PlayUiSound(screenContext, kSfxConfirm);
        const auto statusAfterApply = netplay::bridge::GetStatus();
        const auto phaseAfterApply = static_cast<NetbridgePhase>(statusAfterApply.phase);
        mod::Log(
            "DelayOverlay: confirmed player=%d selected=%d recommended=%d min=%d max=%d ping=%d phase=%s syncReady=%d",
            confirmPlayer,
            selectedDelay,
            g_delaySetupOverlay.recommendedDelay,
            g_delaySetupOverlay.minDelay,
            g_delaySetupOverlay.maxDelay,
            g_delaySetupOverlay.pingMs,
            netplay::bridge::PhaseToString(phaseAfterApply),
            statusAfterApply.vsHumanSyncReady);

        const bool readyForHandoff =
            phaseAfterApply == NetbridgePhase::Connected || statusAfterApply.vsHumanSyncReady != 0;
        if (!readyForHandoff)
        {
            g_delaySetupOverlay.waitingForRuntimeReady = true;
            g_delaySetupOverlay.vsHumanSyncArmed = false;
            g_delaySetupOverlay.nextHandoffRetryTick = 0;
            g_delaySetupOverlay.errorMessage[0] = '\0';
            mod::Log(
                "DelayOverlay: waiting for runtime sync after delay selection phase=%s prompt=%d/%d",
                netplay::bridge::PhaseToString(phaseAfterApply),
                statusAfterApply.delayPromptSerial,
                statusAfterApply.delayPromptServedSerial);
            return true;
        }

        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        HandoffConnectedSessionToVsHumanState(screenContext);
        return true;
    }

    return true;
}

void PrepareVsHumanGameState(uint32_t screenContext)
{
    const int gameSystem = GetGameSystem(screenContext);
    if (gameSystem == 0)
    {
        mod::Log("PrepareVsHumanGameState: skipped (gameSystem=null)");
        return;
    }

    auto* const state = reinterpret_cast<uint8_t*>(gameSystem);
    const uint8_t roundsSetting = state[kGameSystemOffsetRoundsSetting];

    // Mirror the native title "VS Human" branch:
    // 4931/4932 = human-vs-human, 4964 = mode 4, 4942 = configured round count.
    // Also set 4965 and 82563 which ConfigureModeFlags(0) would set via the
    // nav hooks at 0x763F04/0x763E50.  We bypass those hooks by forcing
    // g_pendingGlobalStateTransition=1, so the flags must be set here.
    state[kGameSystemOffsetCpuFlagP1] = 0;
    state[kGameSystemOffsetCpuFlagP2] = 0;
    state[kGameSystemOffsetMode] = kGameModeVsHuman;
    state[kGameSystemOffsetSecondaryModeFlag] = kSecondaryModeFlagVsHuman;
    state[kGameSystemOffsetRoundsCurrent] = roundsSetting;
    state[kGameSystemOffsetReplaySessionFlag] = kReplaySessionFlagCleared;

    mod::Log(
        "PrepareVsHumanGameState: mode=%u secondaryMode=%u replaySession=%u rounds=%u cpuFlags=%u/%u",
        static_cast<unsigned>(state[kGameSystemOffsetMode]),
        static_cast<unsigned>(state[kGameSystemOffsetSecondaryModeFlag]),
        static_cast<unsigned>(state[kGameSystemOffsetReplaySessionFlag]),
        static_cast<unsigned>(state[kGameSystemOffsetRoundsCurrent]),
        static_cast<unsigned>(state[kGameSystemOffsetCpuFlagP1]),
        static_cast<unsigned>(state[kGameSystemOffsetCpuFlagP2]));
}
} // namespace

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
    g_pendingVsHumanAutoConfirmTick = 0;
    g_pendingVsHumanAutoConfirmLastLogTick = 0;
    g_pendingGlobalStateTransition = -1;
    g_returnToNetplayAfterMatch = false;
    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
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
    g_pendingVsHumanAutoConfirmTick = 0;
    g_pendingVsHumanAutoConfirmLastLogTick = 0;
    g_pendingGlobalStateTransition = -1;
    g_returnToNetplayAfterMatch = false;
    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
    RemoveNetplayWindowHook();

    (void)LoadTitleAssets(screenContext);
    ResetTitleMenuState(screenContext, 5);
    RunTransitionFadeIn(screenContext);
    mod::Log(
        "LeaveNetplayMenu: returned to title assets, titleSelection=%d",
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
}

void HandoffConnectedSessionToVsHumanState(uint32_t screenContext)
{
    const bool prepared = netplay::bridge::PrepareVsHumanHandoff();
    const netplay::bridge::NetbridgeStatus status = netplay::bridge::GetStatus();
    mod::Log(
        "HandoffConnectedSessionToVsHumanState: begin prepared=%d sync(mode=%d flag1084=%d session=%d flags=%d/%d)",
        prepared ? 1 : 0,
        status.syncGameMode,
        status.syncMode0Flag1084,
        status.syncSessionByte,
        status.syncGlobalFlag4964,
        status.syncGlobalFlag4965);

    RunTransitionFadeOut(screenContext, 0, 0);
    PrepareVsHumanGameState(screenContext);

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
    g_pendingVsHumanAutoConfirm = false;
    g_pendingVsHumanAutoConfirmTick = 0;
    g_pendingVsHumanAutoConfirmLastLogTick = 0;
    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
    RemoveNetplayWindowHook();
    g_returnToNetplayAfterMatch = true;
    g_pendingGlobalStateTransition = 1;

    mod::Log(
        "HandoffConnectedSessionToVsHumanState: queued global transition nextState=%d returnToNetplay=%d",
        g_pendingGlobalStateTransition,
        g_returnToNetplayAfterMatch ? 1 : 0);
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
            NetbridgeRole::JoinSpectate,
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

    // --- Debug overlay (D key) ---
    // Always poll the D key toggle; if the overlay is open, consume input.
    if (HandleDebugOverlayInput(screenContext, inputBytes, inactivityCounter))
    {
        // Don't clear input latches — the debug overlay manages them internally
        // to prevent axis repeat.
        return 0;
    }

    const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
    const NetbridgePhase bridgePhase = static_cast<NetbridgePhase>(bridgeStatus.phase);
    const bool bridgeDelaySetupReady = bridgeStatus.delaySetupReady != 0;

    // --- Fast handoff ---
    // If the Revival rollback engine is already in sync state (vsHumanSyncReady)
    // and the delay overlay has been submitted (waitingForRuntimeReady) or delay
    // setup was never shown, transition immediately.  This prevents a multi-frame
    // window where the remote side is at State 1 (charselect) while we're still
    // at State 0 (title), which causes a non-rollback desync freeze.
    if (bridgeStatus.vsHumanSyncReady != 0
        && (bridgePhase == NetbridgePhase::Connected
            || bridgePhase == NetbridgePhase::DelaySetup)
        && (!g_delaySetupOverlay.active || g_delaySetupOverlay.waitingForRuntimeReady))
    {
        mod::Log(
            "FastHandoff: vsHumanSyncReady detected, immediate transition phase=%s sync(mode=%d flag1084=%d session=%d)",
            netplay::bridge::PhaseToString(bridgePhase),
            bridgeStatus.syncGameMode,
            bridgeStatus.syncMode0Flag1084,
            bridgeStatus.syncSessionByte);
        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        HandoffConnectedSessionToVsHumanState(screenContext);
        if (g_pendingGlobalStateTransition >= 0)
        {
            const int nextState = g_pendingGlobalStateTransition;
            g_pendingGlobalStateTransition = -1;
            return static_cast<char>(nextState);
        }
        return 0;
    }

    // --- Spectate confirm overlay ---
    // The spectate confirm prompt fires during Connecting when the host is
    // already mid-match.  We must handle it BEFORE the delay-setup check so
    // that the user can accept/decline before Revival continues.
    const bool spectateConfirmPending =
        bridgeStatus.spectateConfirmPromptSerial > 0
        && bridgeStatus.spectateConfirmPromptServedSerial < bridgeStatus.spectateConfirmPromptSerial;
    if (spectateConfirmPending && !g_spectateConfirmOverlay.active)
    {
        ActivateSpectateConfirmOverlay();
    }
    if (g_spectateConfirmOverlay.active)
    {
        (void)HandleSpectateConfirmOverlayInput(screenContext, inputBytes, inactivityCounter);
        return 0;
    }

    if ((bridgePhase == NetbridgePhase::DelaySetup
            || bridgePhase == NetbridgePhase::Connected
            || bridgePhase == NetbridgePhase::Connecting)
        && bridgeDelaySetupReady)
    {
        const netplay::bridge::DelayPromptMetrics promptMetrics = netplay::bridge::GetDelayPromptMetrics();
        if (!g_delaySetupOverlay.active)
        {
            ActivateDelaySetupOverlay(bridgeStatus);
        }
        else
        {
            if (bridgeStatus.pingMs >= 0)
            {
                g_delaySetupOverlay.pingMs = bridgeStatus.pingMs;
            }
            if (bridgeStatus.p1Name[0] != '\0' && bridgeStatus.p2Name[0] != '\0')
            {
                CopyBoundedText(g_delaySetupOverlay.p1Name, sizeof(g_delaySetupOverlay.p1Name), bridgeStatus.p1Name);
                CopyBoundedText(g_delaySetupOverlay.p2Name, sizeof(g_delaySetupOverlay.p2Name), bridgeStatus.p2Name);
            }
            else
            {
                g_delaySetupOverlay.p1Name[0] = '\0';
                g_delaySetupOverlay.p2Name[0] = '\0';
            }
            if (promptMetrics.serial > 0)
            {
                if (promptMetrics.averagePingMs >= 0)
                {
                    g_delaySetupOverlay.pingMs = promptMetrics.averagePingMs;
                }
                if (promptMetrics.minDelay >= kDelaySelectionMin && promptMetrics.minDelay <= kDelaySelectionMax)
                {
                    g_delaySetupOverlay.minDelay = promptMetrics.minDelay;
                }
                if (promptMetrics.maxDelay >= kDelaySelectionMin && promptMetrics.maxDelay <= kDelaySelectionMax)
                {
                    g_delaySetupOverlay.maxDelay = promptMetrics.maxDelay;
                }
                if (g_delaySetupOverlay.maxDelay < g_delaySetupOverlay.minDelay)
                {
                    g_delaySetupOverlay.maxDelay = g_delaySetupOverlay.minDelay;
                }
                if (promptMetrics.recommendedDelay >= kDelaySelectionMin && promptMetrics.recommendedDelay <= kDelaySelectionMax)
                {
                    const int clampedRecommended =
                        (std::max)(g_delaySetupOverlay.minDelay, (std::min)(g_delaySetupOverlay.maxDelay, promptMetrics.recommendedDelay));
                    g_delaySetupOverlay.recommendedDelay = clampedRecommended;
                    if (!g_delaySetupOverlay.waitingForRuntimeReady)
                    {
                        g_delaySetupOverlay.selectedDelay =
                            (std::max)(g_delaySetupOverlay.minDelay, (std::min)(g_delaySetupOverlay.maxDelay, g_delaySetupOverlay.selectedDelay));
                    }
                }
            }
            g_delaySetupOverlay.selectedDelay =
                (std::max)(g_delaySetupOverlay.minDelay, (std::min)(g_delaySetupOverlay.maxDelay, g_delaySetupOverlay.selectedDelay));
        }

        if (g_delaySetupOverlay.waitingForRuntimeReady
            && (bridgePhase == NetbridgePhase::Connected || bridgeStatus.vsHumanSyncReady != 0))
        {
            if (bridgeStatus.vsHumanSyncReady == 0 && bridgePhase == NetbridgePhase::Connected)
            {
                if (!g_delaySetupOverlay.connectedHandoffDelayActive)
                {
                    g_delaySetupOverlay.connectedHandoffDelayActive = true;
                    g_delaySetupOverlay.connectedHandoffDelayFramesRemaining = kConnectedPreHandoffDelayFrames;
                    mod::Log(
                        "DelayOverlay: connected pre-handoff delay started frames=%d sync(mode=%d flag1084=%d session=%d)",
                        g_delaySetupOverlay.connectedHandoffDelayFramesRemaining,
                        bridgeStatus.syncGameMode,
                        bridgeStatus.syncMode0Flag1084,
                        bridgeStatus.syncSessionByte);
                }

                if (g_delaySetupOverlay.connectedHandoffDelayFramesRemaining > 0)
                {
                    --g_delaySetupOverlay.connectedHandoffDelayFramesRemaining;
                    return 0;
                }
            }

            mod::Log(
                "DelayOverlay: runtime sync ready after delay selection phase=%s sync(mode=%d flag1084=%d session=%d)",
                netplay::bridge::PhaseToString(bridgePhase),
                bridgeStatus.syncGameMode,
                bridgeStatus.syncMode0Flag1084,
                bridgeStatus.syncSessionByte);
            ResetDelaySetupOverlayState();
            ResetSpectateConfirmOverlayState();
            HandoffConnectedSessionToVsHumanState(screenContext);
            if (g_pendingGlobalStateTransition >= 0)
            {
                const int nextState = g_pendingGlobalStateTransition;
                g_pendingGlobalStateTransition = -1;
                mod::Log("NetplayTransition: returning global state=%d from netplay menu", nextState);
                return static_cast<char>(nextState);
            }
            return 0;
        }

        (void)HandleDelaySetupOverlayInput(screenContext, inputBytes, inactivityCounter);
        if (g_pendingGlobalStateTransition >= 0)
        {
            const int nextState = g_pendingGlobalStateTransition;
            g_pendingGlobalStateTransition = -1;
            mod::Log("NetplayTransition: returning global state=%d from netplay menu", nextState);
            return static_cast<char>(nextState);
        }
        return 0;
    }
    else
    {
        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
    }

    // Some sessions can complete delay negotiation inside Revival without
    // presenting a prompt we can mirror. In that case, go straight to VS Human.
    if (bridgePhase == NetbridgePhase::Connected
        && !bridgeDelaySetupReady
        && bridgeStatus.vsHumanSyncReady != 0)
    {
        mod::Log(
            "NetplayTransition: connected without delay prompt; auto handoff sync(mode=%d flag1084=%d session=%d)",
            bridgeStatus.syncGameMode,
            bridgeStatus.syncMode0Flag1084,
            bridgeStatus.syncSessionByte);
        HandoffConnectedSessionToVsHumanState(screenContext);
        if (g_pendingGlobalStateTransition >= 0)
        {
            const int nextState = g_pendingGlobalStateTransition;
            g_pendingGlobalStateTransition = -1;
            mod::Log("NetplayTransition: returning global state=%d from netplay menu", nextState);
            return static_cast<char>(nextState);
        }
        return 0;
    }

    if (bridgePhase == NetbridgePhase::Connecting || bridgePhase == NetbridgePhase::DelaySetup)
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
