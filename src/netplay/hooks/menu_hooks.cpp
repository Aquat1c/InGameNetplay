#include "netplay/hooks/menu_hooks.h"
#include "netplay/hooks/internal/shared.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/bridge/takeover_internal.h"

#include "crash_handler.h"
#include "logger.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace netplay::hooks::internal
{
std::mutex g_patchMutex;
std::atomic<bool> g_hooksInstalled{false};
uintptr_t g_exeBase = 0;
std::vector<netplay::patch::PatchRecord> g_appliedPatches;
uint32_t g_customDispatchTable[8] = {};
uint32_t g_replayCaseDispatchAddress = 0;
extern "C" uint32_t g_titleCaseReturnAddress = 0;
std::string g_moduleDirectory;
bool g_netplayAssetsAvailable = false;
bool g_titleAssetsOverrideApplied = false;

NetplayMenuState g_netplayMenuState;
MenuSlideTransition g_menuSlideTransition;
netplay::inline_edit::State g_inlineEditState;
DWORD g_lastNetplayFrameLogTick = 0;
uint64_t g_titleUpdateCallCount = 0;
uint64_t g_netplayUpdateCallCount = 0;
int8_t g_lastLoggedSelection = -1;
bool g_hasLoggedInputSnapshot = false;
netplay::fontmap::SpriteFont g_spriteFont = {};
bool g_useRuntimeTextOverlay = true;
bool g_enableGdiFallbackOverlay = false;
HFONT g_menuOverlayFont = nullptr;
HWND g_hookedWindow = nullptr;
WNDPROC g_originalWindowProc = nullptr;
bool g_netplayEscapeDown = false;
std::string g_netplayStatusMessage;
DWORD g_netplayStatusExpireTick = 0;
bool g_restoreReplaySelectionOnNextTitleUpdate = false;
uint32_t g_replaySelectionGuardFramesRemaining = 0;
int8_t g_replaySelectionRestoreTarget = -1;
bool g_pendingVsHumanAutoConfirm = false;
DWORD g_pendingVsHumanAutoConfirmTick = 0;
DWORD g_pendingVsHumanAutoConfirmLastLogTick = 0;
bool g_returnToNetplayAfterMatch = false;
bool g_charSelectEntryHoldArmed = false;
InputSnapshot g_lastInputSnapshot = {};
DelaySetupOverlayState g_delaySetupOverlay = {};
SpectateConfirmOverlayState g_spectateConfirmOverlay = {};
HostingOverlayState g_hostingOverlay = {};
JoiningOverlayState g_joiningOverlay = {};
DebugOverlayState g_debugOverlay = {};
std::unique_ptr<netplay::lobby::LobbySession> g_lobbySession;
bool g_titleConfirmDown = false;
// Number of frames to keep calling ClearRevivalText after an ExitProcess
// interception.  Ensures stale tournament text is removed even if transient
// state (e.g. init(2,102) resetting dword_100A0778) re-adds it briefly.
static int g_postExitTextClearFrames = 0;
static bool g_charSelectEntryHoldActive = false;
static int g_charSelectEntryHoldFramesRemaining = 0;
static uint32_t g_charSelectUpdateSlotAddress = 0;
static TitleUpdateFn g_originalCharSelectUpdate = nullptr;
static constexpr int kCharSelectEntryHoldFrames = 8;

extern "C" char __cdecl HookedCharSelectUpdateImpl(uint32_t screenContext);
extern "C" void HookedCharSelectUpdateThunk();

// ---- Replay screen hook (spectate bypass) ----
// When spectating, the title flow transitions to screen 8 (Replay) so that
// the Revival DLL's mode-transition detector creates the spectator watcher.
// However, the native replay screen shows an interactive file-selection UI.
// This hook intercepts the replay update function and, when the spectate
// bypass is armed, skips the file selection entirely by returning 1 (go to
// charselect) after a short delay to let the DLL detect the 0→8 transition.
static uint32_t g_replayUpdateSlotAddress = 0;
static TitleUpdateFn g_originalReplayUpdate = nullptr;
static bool g_spectateReplayBypassActive = false;
static int g_spectateReplayBypassCountdown = 0;
static constexpr int kSpectateReplayBypassFrames = 3;

extern "C" char __cdecl HookedReplayScreenUpdateImpl(uint32_t screenContext);
extern "C" void HookedReplayScreenUpdateThunk();

bool EnsureCharSelectEntryHoldHook()
{
    if (g_charSelectUpdateSlotAddress != 0)
    {
        return g_originalCharSelectUpdate != nullptr;
    }

    constexpr uintptr_t kVaScreenObjectTable = 0x00790110;
    const uintptr_t tableAddress = RuntimeAddress(kVaScreenObjectTable);
    if (tableAddress == 0)
    {
        return false;
    }

    uint32_t charSelectObject = 0;
    uint32_t charSelectVtable = 0;
    __try
    {
        auto* const screenTable = reinterpret_cast<uint32_t*>(tableAddress);
        charSelectObject = screenTable[1];
        if (charSelectObject == 0)
        {
            return false;
        }
        charSelectVtable = *reinterpret_cast<uint32_t*>(charSelectObject);
        if (charSelectVtable == 0)
        {
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    auto* const updateSlot = reinterpret_cast<uint32_t*>(charSelectVtable + 4);
    uint32_t originalUpdateAddress = 0;
    __try
    {
        originalUpdateAddress = *updateSlot;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    const uint32_t hookAddress = reinterpret_cast<uint32_t>(&HookedCharSelectUpdateThunk);
    if (originalUpdateAddress == hookAddress)
    {
        g_charSelectUpdateSlotAddress = reinterpret_cast<uint32_t>(updateSlot);
        return g_originalCharSelectUpdate != nullptr;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(updateSlot, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        return false;
    }

    *updateSlot = hookAddress;
    DWORD ignored = 0;
    (void)VirtualProtect(updateSlot, sizeof(uint32_t), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), updateSlot, sizeof(uint32_t));

    g_originalCharSelectUpdate = reinterpret_cast<TitleUpdateFn>(originalUpdateAddress);
    g_charSelectUpdateSlotAddress = reinterpret_cast<uint32_t>(updateSlot);
    mod::Log(
        "InstallHooks: charselect update hook installed slot=0x%08X original=0x%08X",
        g_charSelectUpdateSlotAddress,
        originalUpdateAddress);
    return true;
}

void ArmCharSelectEntryHold()
{
    if (!EnsureCharSelectEntryHoldHook())
    {
        mod::Log("CharSelectHold: failed to arm (charselect hook unavailable)");
        return;
    }

    g_charSelectEntryHoldArmed = true;
    g_charSelectEntryHoldActive = false;
    g_charSelectEntryHoldFramesRemaining = 0;
    mod::Log("CharSelectHold: armed frames=%d", kCharSelectEntryHoldFrames);
}

// ---------------------------------------------------------------------------
// Replay screen hook — spectate bypass
// ---------------------------------------------------------------------------
bool EnsureReplayScreenHook()
{
    if (g_replayUpdateSlotAddress != 0)
    {
        return g_originalReplayUpdate != nullptr;
    }

    constexpr uintptr_t kVaScreenObjectTable = 0x00790110;
    const uintptr_t tableAddress = RuntimeAddress(kVaScreenObjectTable);
    if (tableAddress == 0)
    {
        return false;
    }

    uint32_t replayScreenObject = 0;
    uint32_t replayVtable = 0;
    __try
    {
        auto* const screenTable = reinterpret_cast<uint32_t*>(tableAddress);
        replayScreenObject = screenTable[8]; // screen index 8 = replay
        if (replayScreenObject == 0)
        {
            return false;
        }
        replayVtable = *reinterpret_cast<uint32_t*>(replayScreenObject);
        if (replayVtable == 0)
        {
            return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    auto* const updateSlot = reinterpret_cast<uint32_t*>(replayVtable + 4);
    uint32_t originalUpdateAddress = 0;
    __try
    {
        originalUpdateAddress = *updateSlot;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    const uint32_t hookAddress = reinterpret_cast<uint32_t>(&HookedReplayScreenUpdateThunk);
    if (originalUpdateAddress == hookAddress)
    {
        g_replayUpdateSlotAddress = reinterpret_cast<uint32_t>(updateSlot);
        return g_originalReplayUpdate != nullptr;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(updateSlot, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        return false;
    }

    *updateSlot = hookAddress;
    DWORD ignored = 0;
    (void)VirtualProtect(updateSlot, sizeof(uint32_t), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), updateSlot, sizeof(uint32_t));

    g_originalReplayUpdate = reinterpret_cast<TitleUpdateFn>(originalUpdateAddress);
    g_replayUpdateSlotAddress = reinterpret_cast<uint32_t>(updateSlot);
    mod::Log(
        "InstallHooks: replay screen update hook installed slot=0x%08X original=0x%08X",
        g_replayUpdateSlotAddress,
        originalUpdateAddress);
    return true;
}

void ArmSpectateReplayBypass()
{
    if (!EnsureReplayScreenHook())
    {
        mod::Log("SpectateReplayBypass: failed to arm (replay hook unavailable)");
        return;
    }

    g_spectateReplayBypassActive = true;
    g_spectateReplayBypassCountdown = kSpectateReplayBypassFrames;
    mod::Log("SpectateReplayBypass: armed countdown=%d", kSpectateReplayBypassFrames);
}

void DisarmSpectateReplayBypass()
{
    if (g_spectateReplayBypassActive)
    {
        mod::Log("SpectateReplayBypass: disarmed (was active, countdown=%d)",
            g_spectateReplayBypassCountdown);
    }
    g_spectateReplayBypassActive = false;
    g_spectateReplayBypassCountdown = 0;
}

void ObserveOfflineSelectionConfirm(uint32_t screenContext)
{
    const int gameSystem = GetGameSystem(screenContext);
    const auto* const inputBytes = reinterpret_cast<const uint8_t*>(gameSystem);
    const bool confirmDown = (inputBytes[16] == 1) || (inputBytes[17] == 1);
    if (!IsScreenWindowFocused(screenContext))
    {
        g_titleConfirmDown = confirmDown;
        return;
    }

    const bool confirmEdge = confirmDown && !g_titleConfirmDown;
    g_titleConfirmDown = confirmDown;

    if (!confirmEdge)
    {
        return;
    }

    const auto* const selectionPtr = reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection);
    const int selection = static_cast<int>(*selectionPtr);
    mod::Log("HookedTitleUpdateImpl: title confirm edge selection=%d", selection);
    netplay::bridge::OnTitleSelectionConfirmed(selection);
}

// ---------------------------------------------------------------------------
// HookedTitleUpdateImplBody — the real title-screen update logic.
// Called from HookedTitleUpdateImpl which wraps it in setjmp/longjmp
// protection so NeutralizeExitProcess can safely escape.
// ---------------------------------------------------------------------------
static char HookedTitleUpdateImplBody(uint32_t screenContext)
{
    ++g_titleUpdateCallCount;
    if ((g_titleUpdateCallCount % 300ull) == 0ull)
    {
        mod::Log(
            "HookedTitleUpdateImpl: calls=%llu netplayActive=%d",
            static_cast<unsigned long long>(g_titleUpdateCallCount),
            g_netplayMenuState.active);
    }

    // One-shot (Wine/Proton only): the game's own title screen init loads
    // vanilla title_ob.dat before our hook runs.  Under Wine the DLL module
    // path can resolve to "." which causes vanilla to be picked instead of
    // the mod override.  Reload assets on the first hooked title update so
    // ResolveTitleObjectsPath can probe the correct paths.
    // On native Windows the game's own init already resolves the mod file
    // correctly, so this is unnecessary.
    if (!g_titleAssetsOverrideApplied && !g_netplayMenuState.active)
    {
        g_titleAssetsOverrideApplied = true;
        if (netplay::bridge::IsRunningUnderWine())
        {
            mod::Log("HookedTitleUpdateImpl: Wine detected — applying one-shot title assets override");
            (void)LoadTitleAssets(screenContext);
        }
    }

    // Post-exit text clearing: keep issuing ClearRevivalText for a few
    // frames after an ExitProcess interception to guarantee stale
    // tournament text (nicknames, win counts) is removed even if
    // init(2,102) or session tick re-adds it transiently.
    if (g_postExitTextClearFrames > 0)
    {
        --g_postExitTextClearFrames;
        netplay::bridge::takeover::ClearRevivalText();
        netplay::bridge::takeover::DisableRevivalTextRendering();
    }

    if (!g_netplayMenuState.active)
    {
        // When the tournament match ends and the game returns to mode 0 (title
        // screen), ExitProcess is blocked by Jcc patches so NeutralizeExitProcess
        // and ConsumeRevivalExitInterception never fire on the normal path.
        // Detect this proactively: if we're still flagged as tournament but the
        // game mode is 0, clean up immediately and schedule text clearing.
        if (netplay::bridge::NotifyTitleScreenActive())
        {
            g_postExitTextClearFrames = 5;
            mod::Log("HookedTitleUpdateImpl: tournament return to title detected, clearing text");
        }

        // Check for Revival DLL ExitProcess interception BEFORE ticking.
        // If ExitProcess was intercepted, the session vtable has been
        // neutralized and we need to clean up and route the user to the
        // appropriate screen:  netplay menu for online modes, title screen
        // for tournament / offline modes.
        int exitMode = -1;
        if (netplay::bridge::ConsumeRevivalExitInterception(&exitMode))
        {
            mod::ResetCrashRecoveryState();
            g_returnToNetplayAfterMatch = false;
            DisarmSpectateReplayBypass();
            g_pendingVsHumanAutoConfirm = false;
            g_pendingVsHumanAutoConfirmTick = 0;
            g_pendingVsHumanAutoConfirmLastLogTick = 0;

            if (exitMode == 0 || exitMode == 1)
            {
                // Online or spectate netplay — return to the netplay menu.
                // Schedule multi-frame text clearing to ensure any DLL-side
                // text overlays (nicknames, ping, delay) are fully purged.
                g_postExitTextClearFrames = 5;
                if (g_lobbySession)
                {
                    if (exitMode == netplay::bridge::takeover::kLocalRoleSpectate)
                    {
                        g_lobbySession->NotifyEndSpectate(true);
                    }
                    else
                    {
                        g_lobbySession->NotifyEndMatch();
                    }
                }
                mod::Log(
                    "HookedTitleUpdateImpl: exit intercepted (netplay mode=%d), re-entering netplay menu (skipFadeOut)",
                    exitMode);
                EnterNetplayMenu(screenContext, /*skipFadeOut=*/true);
                return 0;
            }

            // Tournament or other — stay on the normal title screen.
            // Schedule a few frames of ClearRevivalText to ensure stale
            // tournament text overlays are fully cleared.
            g_postExitTextClearFrames = 5;
            mod::Log(
                "HookedTitleUpdateImpl: exit intercepted (mode=%d), staying on title screen",
                exitMode);
            return GetOriginalTitleUpdate()(screenContext);
        }

        netplay::bridge::Tick();

        // After a netplay-initiated VS Human match ends, the game returns to
        // the title screen (state 0).  Re-enter the netplay menu automatically
        // instead of showing the normal title screen.
        if (g_returnToNetplayAfterMatch)
        {
            g_returnToNetplayAfterMatch = false;
            mod::ResetCrashRecoveryState();
            DisarmSpectateReplayBypass();
            const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
            const bool wasSpectate = (bridgeStatus.roleFlag == netplay::bridge::takeover::kLocalRoleSpectate);

            // The active online/spectate session ended and the game naturally
            // returned to the title screen.
            // Cancel the session to terminate the peer process, restore DLL
            // patches, clear stale delay/nickname state, and reset the bridge
            // phase to Idle.  Without this, the old session's delayPromptSerial
            // persists and the delay overlay re-activates immediately.
            mod::Log("HookedTitleUpdateImpl: post-match return, cancelling session and re-entering netplay menu");
            netplay::bridge::CancelSession("match_ended");
            if (g_lobbySession)
            {
                if (wasSpectate)
                {
                    g_lobbySession->NotifyEndSpectate(true);
                }
                else
                {
                    g_lobbySession->NotifyEndMatch();
                }
            }
            // Keep clearing DLL text rendering for several frames, just as
            // the tournament-mode exit path does.  init(3,102) or a transient
            // session tick can re-add text after the first clear.
            g_postExitTextClearFrames = 5;
            EnterNetplayMenu(screenContext, /*skipFadeOut=*/true);
            return 0;
        }

        if (g_pendingVsHumanAutoConfirm)
        {
            if (g_pendingVsHumanAutoConfirmTick == 0)
            {
                g_pendingVsHumanAutoConfirmTick = GetTickCount();
                g_pendingVsHumanAutoConfirmLastLogTick = 0;
            }

            const auto bridgeStatus = netplay::bridge::GetStatus();
            const auto bridgePhase = static_cast<netplay::bridge::NetbridgePhase>(bridgeStatus.phase);
            if (bridgePhase != netplay::bridge::NetbridgePhase::Connected
                && bridgePhase != netplay::bridge::NetbridgePhase::DelaySetup)
            {
                mod::Log(
                    "HookedTitleUpdateImpl: canceled pending VS Human auto-confirm phase=%s",
                    netplay::bridge::PhaseToString(bridgePhase));
                g_pendingVsHumanAutoConfirm = false;
                g_pendingVsHumanAutoConfirmTick = 0;
                g_pendingVsHumanAutoConfirmLastLogTick = 0;
            }
            else
            {
                const bool syncReady =
                    bridgeStatus.syncMode0Flag1084 == 4 &&
                    (bridgeStatus.syncSessionByte == 0 ||
                     bridgeStatus.syncSessionByte == 1 ||
                     bridgeStatus.syncSessionByte == 2);

                const DWORD nowTick = GetTickCount();
                const DWORD elapsed = nowTick - g_pendingVsHumanAutoConfirmTick;
                if (syncReady)
                {
                    (void)netplay::bridge::PrepareVsHumanHandoff();
                    const int gameSystem = GetGameSystem(screenContext);
                    auto* const inputBytes = reinterpret_cast<uint8_t*>(gameSystem);
                    inputBytes[12] = 0;
                    inputBytes[13] = 0;
                    inputBytes[14] = 0;
                    inputBytes[15] = 0;
                    inputBytes[16] = 1;
                    inputBytes[17] = 0;
                    inputBytes[18] = 0;
                    inputBytes[19] = 0;
                    mod::Log(
                        "HookedTitleUpdateImpl: injecting one-shot VS Human confirm syncReady=%d elapsed=%lums sync(mode=%d flag1084=%d session=%d)",
                        syncReady ? 1 : 0,
                        static_cast<unsigned long>(elapsed),
                        bridgeStatus.syncGameMode,
                        bridgeStatus.syncMode0Flag1084,
                        bridgeStatus.syncSessionByte);
                    g_pendingVsHumanAutoConfirm = false;
                    g_pendingVsHumanAutoConfirmTick = 0;
                    g_pendingVsHumanAutoConfirmLastLogTick = 0;
                }
                else
                {
                    if (g_pendingVsHumanAutoConfirmLastLogTick == 0 ||
                        nowTick - g_pendingVsHumanAutoConfirmLastLogTick >= 1000)
                    {
                        mod::Log(
                            "HookedTitleUpdateImpl: waiting VS Human sync elapsed=%lums sync(mode=%d flag1084=%d session=%d flags=%d/%d)",
                            static_cast<unsigned long>(elapsed),
                            bridgeStatus.syncGameMode,
                            bridgeStatus.syncMode0Flag1084,
                            bridgeStatus.syncSessionByte,
                            bridgeStatus.syncGlobalFlag4964,
                            bridgeStatus.syncGlobalFlag4965);
                        g_pendingVsHumanAutoConfirmLastLogTick = nowTick;
                    }
                }
            }
        }

        const char result = GetOriginalTitleUpdate()(screenContext);

        ObserveOfflineSelectionConfirm(screenContext);

        if (g_restoreReplaySelectionOnNextTitleUpdate)
        {
            auto* const selectionPtr = reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection);
            if (g_replaySelectionRestoreTarget >= 0 && *selectionPtr == 4)
            {
                *selectionPtr = g_replaySelectionRestoreTarget;
                mod::Log(
                    "HookedTitleUpdateImpl: guarded replay title selection 4 -> %d (guardFrames=%u)",
                    static_cast<int>(g_replaySelectionRestoreTarget),
                    static_cast<unsigned>(g_replaySelectionGuardFramesRemaining));
            }

            if (g_replaySelectionGuardFramesRemaining > 0)
            {
                --g_replaySelectionGuardFramesRemaining;
                if (g_replaySelectionGuardFramesRemaining == 0)
                {
                    g_restoreReplaySelectionOnNextTitleUpdate = false;
                    g_replaySelectionRestoreTarget = -1;
                    mod::Log("HookedTitleUpdateImpl: replay selection guard window ended");
                }
            }
        }
        return result;
    }

    g_titleConfirmDown = false;
    return UpdateNetplayMenu(screenContext);
}

// ---------------------------------------------------------------------------
// Replay screen bypass — hooked update implementation.
// ---------------------------------------------------------------------------
static char HookedReplayScreenUpdateImplBody(uint32_t screenContext)
{
    if (g_spectateReplayBypassActive)
    {
        // Clear init flag to skip BGM playback and replay file scanning.
        // The native init code plays track 6 BGM and calls
        // initializeReplaySystem — both are undesirable for spectating.
        __try
        {
            *reinterpret_cast<int8_t*>(screenContext + 44) = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (g_spectateReplayBypassCountdown > 0)
        {
            --g_spectateReplayBypassCountdown;
            mod::Log(
                "SpectateReplayBypass: waiting for DLL mode detection countdown=%d",
                g_spectateReplayBypassCountdown);
            return 8; // Stay on replay screen to let DLL detect 0→8 transition
        }

        // Done waiting — transition to character select.
        g_spectateReplayBypassActive = false;
        mod::Log("SpectateReplayBypass: advancing to charselect (return 1)");
        return 1;
    }

    // Normal (non-spectate) replay screen: call original function.
    if (g_originalReplayUpdate == nullptr)
        return 8;
    return g_originalReplayUpdate(screenContext);
}

// ---------------------------------------------------------------------------
// HookedReplayScreenUpdateImpl — dispatches to HookedReplayScreenUpdateImplBody.
//
// Spectate and join-spectate both pass through the replay screen during the
// lightweight watcher handoff. ExitProcess can fire here if the spectate
// session ends or disconnects before the handoff fully completes. Without
// the UI setjmp guard, NeutralizeExitProcess falls through to the fragile
// VEH TOCTOU recovery path and EFZ can close outright instead of returning
// to the netplay menu.
//
// Reuse the same g_netplayUiJmpBuf that title/charselect use: only one UI
// screen update runs at a time, so replay is safe to guard the same way.
// On longjmp recovery, force game mode 0 and let the title hook consume the
// exit interception and re-enter the netplay menu cleanly.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4611) // setjmp / C++ destruction interaction
#endif
extern "C" char __cdecl HookedReplayScreenUpdateImpl(uint32_t screenContext)
{
    netplay::bridge::takeover::g_netplayUiJmpActive = true;
    if (setjmp(netplay::bridge::takeover::g_netplayUiJmpBuf) != 0)
    {
        netplay::bridge::takeover::g_netplayUiJmpActive = false;

        mod::Log(
            "HookedReplayScreenUpdateImpl: recovered from ExitProcess via "
            "ui longjmp — forcing game mode to title");

        mod::ResetCrashRecoveryState();
        DisarmSpectateReplayBypass();
        g_restoreReplaySelectionOnNextTitleUpdate = false;
        g_replaySelectionGuardFramesRemaining = 0;
        netplay::bridge::ForceGameModeToTitle();

        // Exit interception is already armed; skip this frame and let the
        // title-screen update consume/cleanup in a clean state.
        return 0;
    }

    const char result = HookedReplayScreenUpdateImplBody(screenContext);
    netplay::bridge::takeover::g_netplayUiJmpActive = false;
    return result;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---------------------------------------------------------------------------
// Charselect intro animation input suppression.
// ---------------------------------------------------------------------------
// HookedCharSelectUpdateImplBody — the real charselect update logic.
// Called from HookedCharSelectUpdateImpl which wraps it in setjmp/longjmp
// protection so NeutralizeExitProcess can safely escape.
// ---------------------------------------------------------------------------
static uint32_t g_charSelectUpdateCallCount = 0;

static char HookedCharSelectUpdateImplBody(uint32_t screenContext)
{
    ++g_charSelectUpdateCallCount;

    // Drive the bridge and state export every charselect frame.
    // Previously this was only called during the entry-hold window, which
    // caused the exported state to freeze as soon as the hold ended.
    netplay::bridge::Tick();

    // Diagnostic: log charselect screen state on the first 5 frames
    // and then every 300 frames to track init/exit flags and game mode.
    if (g_charSelectUpdateCallCount <= 5
        || (g_charSelectUpdateCallCount % 300 == 0 && g_charSelectUpdateCallCount <= 3000))
    {
        uint8_t initFlag = 0xFF, exitFlag = 0xFF, gameMode = 0xFF, secondaryMode = 0xFF;
        __try
        {
            initFlag = *reinterpret_cast<const uint8_t*>(screenContext + 44);
            exitFlag = *reinterpret_cast<const uint8_t*>(screenContext + 45);
            const uint32_t gameSys = *reinterpret_cast<const uint32_t*>(
                screenContext + netplay::constants::kOffsetGameSystem);
            if (gameSys != 0)
            {
                gameMode = *reinterpret_cast<const uint8_t*>(gameSys + 4964);
                secondaryMode = *reinterpret_cast<const uint8_t*>(gameSys + 4965);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        mod::Log(
            "CHARSELECT_UPDATE: frame=%u ctx=0x%08lX init=%u exit=%u mode=%u/%u",
            g_charSelectUpdateCallCount,
            static_cast<unsigned long>(screenContext),
            static_cast<unsigned>(initFlag),
            static_cast<unsigned>(exitFlag),
            static_cast<unsigned>(gameMode),
            static_cast<unsigned>(secondaryMode));
    }

    if (g_originalCharSelectUpdate == nullptr)
    {
        return 1;
    }

    if (g_charSelectEntryHoldArmed && !g_charSelectEntryHoldActive)
    {
        g_charSelectEntryHoldArmed = false;
        g_charSelectEntryHoldActive = true;
        g_charSelectEntryHoldFramesRemaining = kCharSelectEntryHoldFrames;
        g_charSelectUpdateCallCount = 0;  // reset for fresh logging window
        mod::Log("CharSelectHold: started on first charselect frame budget=%d", g_charSelectEntryHoldFramesRemaining);
    }

    if (!g_charSelectEntryHoldActive)
    {
        const char csResult = g_originalCharSelectUpdate(screenContext);
        if (csResult != 1)
        {
            mod::Log(
                "CHARSELECT_UPDATE: originalUpdate returned %d (non-1) frame=%u",
                static_cast<int>(csResult), g_charSelectUpdateCallCount);
        }
        return csResult;
    }
    const auto bridgeStatus = netplay::bridge::GetStatus();
    const bool syncReady = bridgeStatus.vsHumanSyncReady != 0;
    if (syncReady)
    {
        g_charSelectEntryHoldActive = false;
        g_charSelectEntryHoldFramesRemaining = 0;
        mod::Log(
            "CharSelectHold: released early sync(mode=%d flag1084=%d session=%d)",
            bridgeStatus.syncGameMode,
            bridgeStatus.syncMode0Flag1084,
            bridgeStatus.syncSessionByte);
        {
            const char csResult = g_originalCharSelectUpdate(screenContext);
            if (csResult != 1)
                mod::Log("CHARSELECT_UPDATE: hold-release originalUpdate returned %d frame=%u",
                    static_cast<int>(csResult), g_charSelectUpdateCallCount);
            return csResult;
        }
    }

    --g_charSelectEntryHoldFramesRemaining;
    if (g_charSelectEntryHoldFramesRemaining <= 0)
    {
        g_charSelectEntryHoldActive = false;
        g_charSelectEntryHoldFramesRemaining = 0;
        mod::Log(
            "CharSelectHold: timeout release sync(mode=%d flag1084=%d session=%d)",
            bridgeStatus.syncGameMode,
            bridgeStatus.syncMode0Flag1084,
            bridgeStatus.syncSessionByte);
        {
            const char csResult = g_originalCharSelectUpdate(screenContext);
            if (csResult != 1)
                mod::Log("CHARSELECT_UPDATE: hold-timeout originalUpdate returned %d frame=%u",
                    static_cast<int>(csResult), g_charSelectUpdateCallCount);
            return csResult;
        }
    }

    return 1;
}

// ---------------------------------------------------------------------------
// HookedCharSelectUpdateImpl — dispatches to HookedCharSelectUpdateImplBody.
//
// ExitProcess can fire during charselect when the DLL detects a desync (e.g.
// State mismatch at early rollback frames).  The frame-hook setjmp
// (g_netplayFrameJmpBuf) only guards sub_1006E590; other DLL vtable methods
// called during charselect are NOT covered.  Without setjmp protection here,
// NeutralizeExitProcess falls through to the fragile VEH TOCTOU last-resort
// recovery which fails silently (game closes, no crash logs).
//
// Wrap the body in setjmp on g_netplayUiJmpBuf — the same buffer used by
// HookedTitleUpdateImpl.  Only one screen update runs at a time (title OR
// charselect), so reusing the UI jmpbuf is safe.  On longjmp recovery: force
// game mode to 0 so the title hook can consume the exit interception and
// re-enter the netplay menu cleanly.
// ---------------------------------------------------------------------------
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4611) // setjmp / C++ destruction interaction
#endif
extern "C" char __cdecl HookedCharSelectUpdateImpl(uint32_t screenContext)
{
    netplay::bridge::takeover::g_netplayUiJmpActive = true;
    if (setjmp(netplay::bridge::takeover::g_netplayUiJmpBuf) != 0)
    {
        netplay::bridge::takeover::g_netplayUiJmpActive = false;

        mod::Log(
            "HookedCharSelectUpdateImpl: recovered from ExitProcess via "
            "ui longjmp — forcing game mode to title");

        // Reset the one-shot VEH TOCTOU guard so future sessions can still
        // be recovered if needed.
        mod::ResetCrashRecoveryState();

        // Force game mode to 0 (title screen).  On the next main-loop
        // iteration, HookedTitleUpdateImplBody runs, detects
        // g_revivalExitIntercepted, calls ConsumeRevivalExitInterception
        // (which does full teardown: terminate helper, restore patches,
        // ForceLocalPlayInit, disable text), and re-enters the netplay menu.
        netplay::bridge::ForceGameModeToTitle();

        // Clear charselect hold state so it doesn't carry over.
        g_charSelectEntryHoldActive = false;
        g_charSelectEntryHoldArmed = false;
        g_charSelectEntryHoldFramesRemaining = 0;

        // ExitProcess interception flag is already set; skip this frame and
        // let the title-screen update consume/cleanup in a clean state.
        return 0;
    }

    const char result = HookedCharSelectUpdateImplBody(screenContext);
    netplay::bridge::takeover::g_netplayUiJmpActive = false;
    return result;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---------------------------------------------------------------------------
// HookedTitleUpdateImpl — dispatches to HookedTitleUpdateImplBody.
//
// ExitProcess interception no longer uses longjmp.  The DLL call-site
// patches (SaveAndApplyDllExitProcessPatches) make ExitProcess unreachable
// during tournament mode.  The IAT hook is a safety fallback only.
// ---------------------------------------------------------------------------
extern "C" char __cdecl HookedTitleUpdateImpl(uint32_t screenContext)
{
    netplay::bridge::takeover::g_netplayUiJmpActive = true;
    if (setjmp(netplay::bridge::takeover::g_netplayUiJmpBuf) != 0)
    {
        netplay::bridge::takeover::g_netplayUiJmpActive = false;
        mod::Log("HookedTitleUpdateImpl: recovered from ExitProcess via ui longjmp");
        // ExitProcess interception flag is already set; skip this frame and let
        // the next title update consume/cleanup in a clean state.
        return 0;
    }

    const char result = HookedTitleUpdateImplBody(screenContext);
    netplay::bridge::takeover::g_netplayUiJmpActive = false;
    return result;
}

extern "C" BOOL __cdecl HookedTitleRenderImpl(uint32_t screenContext)
{
    if (g_netplayMenuState.active && g_useRuntimeTextOverlay)
    {
        return RenderNetplayMenuRuntimeText(screenContext);
    }
    if (g_netplayMenuState.active && g_netplayMenuState.useConfigStyleRender)
    {
        return RenderNetplayMenuConfigStyle(screenContext);
    }
    return GetOriginalTitleRender()(screenContext);
}

#if defined(_M_IX86)
extern "C" void __cdecl NetplayCaseImpl(uint32_t screenContext)
{
    if (!g_netplayAssetsAvailable)
    {
        mod::Log("NetplayCaseImpl: netplay assets unavailable, ignoring");
        return;
    }
    mod::Log("NetplayCaseImpl: invoked");
    TriggerNetplayMenuEntry(screenContext);
}

extern "C" __declspec(naked) void NetplayCaseThunk()
{
    __asm
    {
        mov eax, dword ptr [ebp-8]
        push eax
        call NetplayCaseImpl
        add esp, 4
        mov al, 0
        mov edx, dword ptr [g_titleCaseReturnAddress]
        jmp edx
    }
}

extern "C" void __cdecl ReplayCaseCompatImpl(uint32_t screenContext)
{
    auto* const selectionPtr =
        reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection);
    const int oldSelection = static_cast<int>(*selectionPtr);
    if (oldSelection != 4)
    {
        // Revival checks mode0+1084 == 4 to recognize replay context.
        g_replaySelectionRestoreTarget = static_cast<int8_t>(oldSelection);
        *selectionPtr = 4;
        g_restoreReplaySelectionOnNextTitleUpdate = true;
        g_replaySelectionGuardFramesRemaining = 300;
        mod::Log(
            "ReplayCaseCompatImpl: remapped title selection %d -> 4 for replay compatibility (restoreTarget=%d guardFrames=%u)",
            oldSelection,
            static_cast<int>(g_replaySelectionRestoreTarget),
            static_cast<unsigned>(g_replaySelectionGuardFramesRemaining));
    }
}

extern "C" __declspec(naked) void ReplayCaseCompatThunk()
{
    __asm
    {
        mov eax, dword ptr [ebp-8]
        push eax
        call ReplayCaseCompatImpl
        add esp, 4
        mov edx, dword ptr [g_replayCaseDispatchAddress]
        jmp edx
    }
}

extern "C" __declspec(naked) void HookedTitleUpdateThunk()
{
    __asm
    {
        push ecx
        call HookedTitleUpdateImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void HookedTitleRenderThunk()
{
    __asm
    {
        push ecx
        call HookedTitleRenderImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void HookedCharSelectUpdateThunk()
{
    __asm
    {
        push ecx
        call HookedCharSelectUpdateImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void HookedReplayScreenUpdateThunk()
{
    __asm
    {
        push ecx
        call HookedReplayScreenUpdateImpl
        add esp, 4
        ret
    }
}
#endif
} // namespace netplay::hooks::internal

namespace netplay
{
bool AreHooksInstalled()
{
    return hooks::internal::g_hooksInstalled.load();
}

void PrepareForProcessExit(bool emergency)
{
    (void)hooks::internal::ShutdownLobbySessionForProcessExit(
        emergency,
        emergency ? "process_exit_emergency" : "process_exit");
}

void ShowInProgressMessage(HWND owner)
{
    mod::Log("ShowInProgressMessage: owner=0x%p", owner);
    (void)owner;
    if (hooks::internal::g_netplayMenuState.active)
    {
        hooks::internal::SetNetplayStatusMessage("In progress.");
        return;
    }

    mod::Log("ShowInProgressMessage: netplay menu inactive, suppressed popup");
}
} // namespace netplay
