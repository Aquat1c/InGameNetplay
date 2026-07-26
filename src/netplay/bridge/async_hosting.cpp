#include "netplay/bridge/async_hosting.h"

#include "netplay/bridge/frontend_return.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/core/mod_settings.h"
#include "netplay/core/options_keybinds.h"
#include "netplay/hooks/menu_query.h"

#include "logger.h"

#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace netplay::bridge::async_host
{
namespace
{
State g_state = State::Idle;
bool g_minimized = false;
uint16_t g_port = 0;
int g_lastPromptSerial = 0;

// Return/rehost hotkey (resolved from settings at host start).
int g_returnKeyVk = 0;
std::string g_returnKeyDisplay = "F1";
bool g_returnKeyWasDown = false;
bool g_returnInProgress = false;
int g_returnInProgressTicks = 0;

// Cached host nickname + cooldown so we can auto-rehost (restart the listener)
// after a peer disconnects / errors out while we are holding the delay prompt.
// (Port is the existing g_port.)
std::string g_nickname;
HostSessionNetworkConfig g_networkConfig = {};
bool g_networkConfigValid = false;
bool g_listenerAcknowledged = false;
uint32_t g_listenerAckSerial = 0;
uint32_t g_listenerCandidateSerial = 0;
DWORD g_listenerCandidateFirstTick = 0;
DWORD g_listenerWaitStartTick = 0;
bool g_listenerStartupFailed = false;
// Direct Host's visible title flow owns the one allowed family retry for the
// initial immutable config. Keep the first helper alive long enough for that
// flow to consume a stable same-port/wrong-family observation.
bool g_deferInitialFamilyMismatchToTitleFlow = false;
bool g_initialFamilyMismatchDeferred = false;
int g_autoRehostCooldownTicks = 0;
constexpr DWORD kListenerAckTimeoutMs = 15000;

// On-arrival actions queued when the return hotkey is pressed while minimized.
bool g_acceptOnArrival = false;
bool g_rehostOnArrival = false;

// Helper-liveness latch for timeout detection: once the helper has been seen
// alive, a later "not alive" means the peer/session dropped.
bool g_peerWasAlive = false;

const char* StateName(State s)
{
    switch (s)
    {
    case State::Idle: return "Idle";
    case State::Hosting: return "Hosting";
    case State::PeerFoundHeld: return "PeerFoundHeld";
    case State::Accepted: return "Accepted";
    case State::TimedOut: return "TimedOut";
    }
    return "?";
}

void SetState(State next, const char* reason)
{
    if (g_state == next)
    {
        return;
    }
    mod::Log(
        "ASYNC_HOST_STATE old=%s new=%s reason=%s",
        StateName(g_state), StateName(next), reason != nullptr ? reason : "");
    g_state = next;
}

// Restart the host listener after the peer dropped / errored on the held delay
// prompt, so the host transparently returns to waiting for a new opponent instead
// of dropping to a dead state. Self-contained: tears down the dead session and
// starts a fresh one on the same port + nickname, then returns to Hosting.
void AutoRehost(const char* reason)
{
    g_deferInitialFamilyMismatchToTitleFlow = false;
    g_initialFamilyMismatchDeferred = false;
    mod::Log(
        "ASYNC_HOST_AUTO_REHOST reason=%s port=%u family=%s fallback=%d "
        "automaticRetry=%d publicAddress='%s'",
        reason != nullptr ? reason : "",
        static_cast<unsigned>(g_port),
        g_networkConfigValid
            ? network::FamilyName(g_networkConfig.effectiveFamily)
            : "legacy-INI",
        g_networkConfigValid
                && g_networkConfig.preferredFamily
                    != g_networkConfig.effectiveFamily
            ? 1
            : 0,
        g_networkConfigValid
                && g_networkConfig.automaticFamilyRetryAttempted
            ? 1
            : 0,
        g_networkConfigValid ? g_networkConfig.publicAddress.c_str() : "");
    CancelSession(reason != nullptr ? reason : "async_host_auto_rehost");

    g_listenerAcknowledged = false;
    g_listenerAckSerial = 0;
    g_listenerCandidateSerial = 0;
    g_listenerCandidateFirstTick = 0;
    g_listenerWaitStartTick = GetTickCount();
    g_listenerStartupFailed = false;
    const bool ok = g_networkConfigValid
        ? StartHostSession(
              g_port,
              g_nickname.c_str(),
              g_networkConfig,
              /*writeNicknameToIni=*/false)
        : StartSession(
              NetbridgeRole::Host,
              g_port,
              "",
              g_nickname.c_str(),
              /*writeNicknameToIni=*/false);

    g_peerWasAlive = false;
    g_returnInProgress = false;
    g_autoRehostCooldownTicks = 96; // ~1.5s for the fresh helper to come up

    if (ok)
    {
        g_lastPromptSerial = GetDelayPromptMetrics().serial;
        SetState(State::Hosting, "auto_rehost");
        // Keep the per-frame battle/result/loading hooks live (lost across the
        // session restart) so the F1 driver + peer detection keep running.
        frontend_return::EnsureFrontendReturnUpdateHooks();
        mod::Log("ASYNC_HOST_AUTO_REHOST: listener restarted on port %u",
            static_cast<unsigned>(g_port));
    }
    else
    {
        // Keep state Hosting so the next eligible Tick retries after the cooldown.
        SetState(State::Hosting, "auto_rehost_retry");
        mod::Log("ASYNC_HOST_AUTO_REHOST: StartSession failed - retry after cooldown");
    }
}

} // namespace

static void BeginHostTracking(
    uint16_t port,
    const char* nickname,
    const HostSessionNetworkConfig* networkConfig)
{
    g_port = port;
    g_nickname = (nickname != nullptr) ? nickname : "";
    if (networkConfig != nullptr)
    {
        g_networkConfig = *networkConfig;
        g_networkConfigValid = true;
    }
    else
    {
        g_networkConfig = {};
        g_networkConfigValid = false;
    }
    g_listenerAcknowledged = false;
    g_listenerAckSerial = 0;
    g_listenerCandidateSerial = 0;
    g_listenerCandidateFirstTick = 0;
    g_listenerWaitStartTick = GetTickCount();
    g_listenerStartupFailed = false;
    g_deferInitialFamilyMismatchToTitleFlow =
        networkConfig != nullptr
        && !networkConfig->automaticFamilyRetryAttempted;
    g_initialFamilyMismatchDeferred = false;
    g_autoRehostCooldownTicks = 0;
    g_minimized = false;
    g_acceptOnArrival = false;
    g_rehostOnArrival = false;
    g_returnInProgress = false;
    g_returnKeyWasDown = false;
    g_peerWasAlive = false;

    // Resolve the configured return/rehost hotkey (DIK_* -> VK).
    const std::string& binding = mod_settings::AsyncHostReturnKeyBinding();
    g_returnKeyVk = options::keybinds::VirtualKeyForBindingValue(binding);
    g_returnKeyDisplay = options::keybinds::FormatBindingValue(binding);
    if (g_returnKeyVk == 0)
    {
        // Fall back to F1 if the binding is unusable (e.g. a pad binding).
        g_returnKeyVk = VK_F1;
        g_returnKeyDisplay = "F1";
    }

    // Snapshot the current console-published prompt serial so only a NEW prompt
    // (an actual peer connecting after we started hosting) counts as peer-found.
    // The console-capture thread updates this independently of the full Tick(),
    // so detection works even while the user is in gameplay/practice.
    g_lastPromptSerial = GetDelayPromptMetrics().serial;

    // Ensure the per-screen (battle/result/loading) update hooks are installed
    // now, so async_host::Tick() runs every frame while the user is minimized in
    // practice/gameplay - otherwise the return hotkey and peer detection would
    // only work at the title/menu. (Normally these hooks install lazily on the
    // first frontend-return request.)
    frontend_return::EnsureFrontendReturnUpdateHooks();

    SetState(State::Hosting, "host_started");
    mod::Log(
        "ASYNC_HOST_BEGIN role=host port=%u promptSerialBase=%d returnKey='%s' "
        "vk=0x%02X family=%s fallback=%d automaticRetry=%d "
        "publicAddress='%s'",
        static_cast<unsigned>(port), g_lastPromptSerial,
        g_returnKeyDisplay.c_str(), g_returnKeyVk,
        g_networkConfigValid
            ? network::FamilyName(g_networkConfig.effectiveFamily)
            : "legacy-INI",
        g_networkConfigValid
                && g_networkConfig.preferredFamily
                    != g_networkConfig.effectiveFamily
            ? 1
            : 0,
        g_networkConfigValid
                && g_networkConfig.automaticFamilyRetryAttempted
            ? 1
            : 0,
        g_networkConfigValid ? g_networkConfig.publicAddress.c_str() : "");
}

void OnHostStarted(uint16_t port, const char* nickname)
{
    HostSessionNetworkConfig activeConfig = {};
    if (GetActiveHostSessionNetworkConfig(&activeConfig))
    {
        BeginHostTracking(port, nickname, &activeConfig);
        return;
    }
    BeginHostTracking(port, nickname, nullptr);
}

void OnHostStarted(
    uint16_t port,
    const char* nickname,
    const HostSessionNetworkConfig& networkConfig)
{
    BeginHostTracking(port, nickname, &networkConfig);
}

void Tick()
{
    if (g_state == State::Idle)
    {
        return;
    }

    // Cooldown after an auto-rehost: a freshly-spawned helper is briefly "not
    // alive" and the phase briefly Idle while the new session spins up - don't
    // mistake that for another disconnect.
    if (g_autoRehostCooldownTicks > 0)
    {
        --g_autoRehostCooldownTicks;
    }

    const NetbridgeStatus status = GetStatus();
    const NetbridgePhase phase = static_cast<NetbridgePhase>(status.phase);
    const bool peerAliveNow = IsPeerProcessAlive();
    if (peerAliveNow)
    {
        // Latch the same liveness sample used for listener validation. The
        // helper can exit between two polls, so polling again after accepting
        // the acknowledgement could otherwise miss the alive -> dead edge.
        g_peerWasAlive = true;
    }

    const bool listenerSessionLive =
        phase != NetbridgePhase::Failed
        && phase != NetbridgePhase::SessionEnded
        && phase != NetbridgePhase::Idle
        && peerAliveNow;
    if (!g_listenerAcknowledged && listenerSessionLive)
    {
        HostListenerObservation observation = {};
        if (GetHostListenerObservation(&observation))
        {
            const DWORD nowTick = GetTickCount();
            if (g_listenerCandidateSerial
                    != observation.serial)
            {
                g_listenerCandidateSerial =
                    observation.serial;
                g_listenerCandidateFirstTick = nowTick;
            }
            else if (nowTick - g_listenerCandidateFirstTick
                         >= 100u
                     && NotifyHostListenerReady(observation))
            {
                // Continue through this tick so normal peer/liveness tracking
                // starts immediately after the native listener is confirmed.
            }
            else if (nowTick - g_listenerCandidateFirstTick
                             >= 100u
                     && observation.expectedFamilyKnown
                     && (!observation.familyMatches
                         || !observation.portMatches))
            {
                if (g_deferInitialFamilyMismatchToTitleFlow
                    && observation.portMatches
                    && !observation.familyMatches
                    && !g_initialFamilyMismatchDeferred)
                {
                    // The title pump runs immediately after this Tick. Defer
                    // exactly once; if ownership is unexpectedly absent, the
                    // next tick treats the mismatch as terminal instead of
                    // suppressing the listener timeout forever.
                    g_initialFamilyMismatchDeferred = true;
                    mod::Log(
                        "ASYNC_HOST_LISTENER_MISMATCH observedFamily=%s "
                        "observedPort=%u familyMatches=0 portMatches=1 "
                        "serial=%u action=defer_to_title_flow",
                        network::FamilyName(observation.family),
                        static_cast<unsigned>(observation.port),
                        observation.serial);
                    return;
                }
                mod::Log(
                    "ASYNC_HOST_LISTENER_MISMATCH observedFamily=%s "
                    "observedPort=%u familyMatches=%d portMatches=%d serial=%u",
                    network::FamilyName(observation.family),
                    static_cast<unsigned>(observation.port),
                    observation.familyMatches ? 1 : 0,
                    observation.portMatches ? 1 : 0,
                    observation.serial);
                g_listenerStartupFailed = true;
                CancelSession("host_listener_ack_mismatch");
                SetState(State::TimedOut, "listener_ack_mismatch");
                return;
            }
        }
    }

    const bool fatalProfileFailure =
        phase == NetbridgePhase::Failed
        && std::strcmp(status.errorMsg, "Unsupported EfzRevival.dll profile") == 0;
    if (fatalProfileFailure)
    {
        mod::Log(
            "ASYNC_HOST_FATAL_STOP reason=unsupported_revival_profile error='%s'",
            status.errorMsg);
        Reset();
        return;
    }

    if (!g_listenerAcknowledged
        && (phase == NetbridgePhase::Failed
            || phase == NetbridgePhase::SessionEnded))
    {
        mod::Log(
            "ASYNC_HOST_START_FAILED_BEFORE_LISTENER phase=%s error='%s' "
            "action=stop_auto_rehost",
            PhaseToString(phase),
            status.errorMsg);
        g_listenerStartupFailed = true;
        SetState(State::TimedOut, "listener_start_failed");
        return;
    }

    if (!g_listenerAcknowledged
        && g_listenerWaitStartTick != 0
        && phase != NetbridgePhase::Idle
        && GetTickCount() - g_listenerWaitStartTick
            >= kListenerAckTimeoutMs)
    {
        mod::Log(
            "ASYNC_HOST_LISTENER_TIMEOUT elapsedMs=%lu phase=%s "
            "helperPid=%lu family=%s port=%u action=cancel",
            static_cast<unsigned long>(
                GetTickCount() - g_listenerWaitStartTick),
            PhaseToString(phase),
            static_cast<unsigned long>(status.processId),
            g_networkConfigValid
                ? network::FamilyName(
                      g_networkConfig.effectiveFamily)
                : "legacy-INI",
            static_cast<unsigned>(g_port));
        g_listenerStartupFailed = true;
        CancelSession("host_listener_ack_timeout");
        SetState(State::TimedOut, "listener_ack_timeout");
        return;
    }

    if (g_listenerAcknowledged
        && g_autoRehostCooldownTicks == 0
        && (g_state == State::Hosting || g_state == State::PeerFoundHeld))
    {
        // While hosting (BEFORE the user accepts), a peer disconnect or session
        // error means the connection died on the held delay prompt. Per design:
        // accept the error and AUTO-REHOST (restart the listener) instead of
        // dropping to a dead state. Detectors:
        //  - helper-liveness latch (IsPeerProcessAlive): reliable in ALL contexts
        //    (menu + gameplay) since it polls the process handle directly;
        //  - session phase Failed/SessionEnded: reliable at the menu (the snapshot
        //    is stale in gameplay, but never spuriously Failed there).
        bool disconnected = false;
        const char* why = "";
        if (!peerAliveNow && g_peerWasAlive)
        {
            disconnected = true;
            why = "helper_gone";
        }
        if (!disconnected
            && (phase == NetbridgePhase::Failed || phase == NetbridgePhase::SessionEnded))
        {
            disconnected = true;
            why = "session_failed";
        }
        if (disconnected)
        {
            AutoRehost(why);
            return;
        }

        // Clean idle (session cancelled/ended cleanly elsewhere) - release. A
        // user-initiated cancel calls Reset() directly, so this only catches an
        // out-of-band clean teardown. Phase is never spuriously Idle in gameplay.
        if (phase == NetbridgePhase::Idle)
        {
            mod::Log("ASYNC_HOST_CANCEL reason=session_phase=Idle");
            Reset();
            return;
        }
    }

    // Return hotkey (F1 by default): while minimized AND actually in a GAMEPLAY
    // screen (practice / VS-CPU / charselect / result / replay), pressing it
    // drives EFZ back to the netplay menu and un-minimizes to the HOST overlay,
    // where a held peer is auto-accepted. Gating on gameplay screens is what
    // makes this safe - firing a return-to-menu transition while ALREADY at the
    // title / netplay menu was the latent bug that got the whole block removed
    // before. There is nothing to return from at the title, so we skip it there.
    // Safety net: a return that fails to reach the menu (e.g. the drive aborts)
    // would otherwise leave g_returnInProgress stuck true and block EVERY later
    // F1 press. A successful return clears it via ConsumeReturnKeyArrival within a
    // second or two, so anything lingering much longer is stuck - clear it.
    if (g_returnInProgress)
    {
        if (++g_returnInProgressTicks > 1200) // ~20s at 64fps
        {
            mod::Log("ASYNC_HOST_RETURN_KEY return stuck >20s - clearing to re-enable F1");
            g_returnInProgress = false;
            g_returnInProgressTicks = 0;
        }
    }
    else
    {
        g_returnInProgressTicks = 0;
    }

    // F1 returns to the netplay HOST menu from ANYWHERE the host is active -
    // gameplay, title, character select, settings, etc. The ONLY place it must
    // not fire is when the netplay menu overlay is already open (the user is
    // already there; firing a return-to-menu transition there was the original
    // latent bug). Not gated on g_minimized either: leaving the menu via Back
    // keeps the host alive without minimizing.
    if (g_returnKeyVk != 0)
    {
        const bool down = (GetAsyncKeyState(g_returnKeyVk) & 0x8000) != 0;
        const bool pressed = down && !g_returnKeyWasDown;
        g_returnKeyWasDown = down;
        if (pressed && !g_returnInProgress && !frontend_return::IsReturningToFrontend())
        {
            const frontend_return::FrontendContext ctx =
                frontend_return::CaptureFrontendContext();
            const bool inNetplayMenu = netplay::hooks::IsNetplayMenuActive();
            mod::Log(
                "ASYNC_HOST_RETURN_KEY edge state=%s screen=%d minimized=%d inNetplayMenu=%d",
                StateName(g_state), static_cast<int>(ctx.screen),
                g_minimized ? 1 : 0, inNetplayMenu ? 1 : 0);
            if (!inNetplayMenu)
            {
                frontend_return::ReturnRequest req;
                req.target = frontend_return::ReturnTarget::NetplayMenu;
                req.owner = frontend_return::ReturnOwner::AsyncHostAccept;
                req.reason = "async_host_return_key";
                req.preserveHelper = true;
                req.enterNetplayMenuAfterTitle = true;
                // Mirror the disconnect-recovery path's flags: without these the
                // drive fails immediately ("force_fallback_disabled") and never
                // exits the battle/charselect screen back to the title/menu.
                req.allowNativeBattleExit = true;
                req.allowNativeResultExit = true;
                req.allowForcedFallback = true;
                const frontend_return::ReturnResult r =
                    frontend_return::BeginReturnToFrontend(req);
                if (r.accepted || r.pending)
                {
                    g_returnInProgress = true;
                    mod::Log(
                        "ASYNC_HOST_RETURN_KEY accepted - returning to netplay HOST menu (had_peer=%d)",
                        g_state == State::PeerFoundHeld ? 1 : 0);
                }
                else
                {
                    mod::Log(
                        "ASYNC_HOST_RETURN_KEY return rejected state=%s",
                        frontend_return::CurrentStateName());
                }
            }
        }
    }

    switch (g_state)
    {
    case State::Hosting:
    {
        const DelayPromptMetrics metrics = GetDelayPromptMetrics();
        const bool newPrompt =
            metrics.serial != 0 && metrics.serial != g_lastPromptSerial;
        if (newPrompt)
        {
            g_lastPromptSerial = metrics.serial;
            SetState(State::PeerFoundHeld, "peer_connected_prompt_ready");
            mod::Log(
                "ASYNC_HOST_DELAY_PROMPT_HELD promptSerial=%d ping=%d phase=%s minimized=%d",
                metrics.serial, metrics.averagePingMs,
                PhaseToString(phase), g_minimized ? 1 : 0);
        }
        break;
    }
    case State::PeerFoundHeld:
    case State::TimedOut:
        // Waiting for accept / rehost (handled via the return key above, or via
        // the menu input handler when not minimized).
        break;
    case State::Accepted:
        mod::Log("ASYNC_HOST_HANDOFF_BEGIN - releasing to normal delay flow");
        Reset();
        break;
    case State::Idle:
        break;
    }
}

bool IsActive()
{
    return g_state != State::Idle;
}

bool IsMinimized()
{
    return g_minimized;
}

void SetMinimized(bool minimized)
{
    if (g_minimized != minimized)
    {
        g_minimized = minimized;
        mod::Log("ASYNC_HOST_OVERLAY %s", minimized ? "minimized" : "restored");
    }
}

bool IsPeerFoundHeld()
{
    return g_state == State::PeerFoundHeld;
}

bool IsTimedOut()
{
    return g_state == State::TimedOut;
}

bool IsHostListenerReady()
{
    return g_listenerAcknowledged;
}

bool HasHostListenerStartupFailed()
{
    return g_listenerStartupFailed;
}

bool NotifyHostListenerReady(
    const HostListenerObservation& observation)
{
    if (!observation.available
        || !observation.portMatches
        || (observation.expectedProcessKnown
            && !observation.processMatches)
        || (observation.expectedFamilyKnown
            && !observation.familyMatches))
    {
        return false;
    }

    if (!g_listenerAcknowledged
        || g_listenerAckSerial != observation.serial)
    {
        g_listenerAcknowledged = true;
        g_listenerAckSerial = observation.serial;
        g_listenerWaitStartTick = 0;
        g_listenerStartupFailed = false;
        g_deferInitialFamilyMismatchToTitleFlow = false;
        g_initialFamilyMismatchDeferred = false;
        mod::Log(
            "ASYNC_HOST_LISTENER_READY family=%s port=%u serial=%u",
            network::FamilyName(observation.family),
            static_cast<unsigned>(observation.port),
            observation.serial);
    }
    return true;
}

bool ShouldSuppressDelayOverlay()
{
    return g_state == State::PeerFoundHeld;
}

const char* ReturnKeyDisplay()
{
    return g_returnKeyDisplay.c_str();
}

void RequestAccept()
{
    if (g_state == State::PeerFoundHeld)
    {
        SetState(State::Accepted, "user_accept");
        mod::Log("ASYNC_HOST_ACCEPT_PRESSED");
    }
}

bool ConsumeReturnKeyArrival()
{
    if (g_returnInProgress)
    {
        g_returnInProgress = false;
        g_returnKeyWasDown = false;
        mod::Log("ASYNC_HOST_RETURN_KEY arrival consumed at netplay menu");
        return true;
    }
    return false;
}

bool OnNetplayMenuRestored()
{
    g_returnInProgress = false;
    if (g_rehostOnArrival)
    {
        g_rehostOnArrival = false;
        g_acceptOnArrival = false;
        mod::Log("ASYNC_HOST: rehost-on-arrival requested");
        return true;
    }
    if (g_acceptOnArrival)
    {
        g_acceptOnArrival = false;
        if (g_state == State::PeerFoundHeld)
        {
            RequestAccept();
        }
    }
    return false;
}

void RequestCancel(const char* reason)
{
    if (g_state == State::Idle)
    {
        return;
    }
    mod::Log("ASYNC_HOST_CANCEL reason=%s", reason != nullptr ? reason : "");
    CancelSession(reason != nullptr ? reason : "async_host_cancel");
    Reset();
}

void Reset()
{
    g_minimized = false;
    g_lastPromptSerial = 0;
    g_acceptOnArrival = false;
    g_rehostOnArrival = false;
    g_returnInProgress = false;
    g_returnKeyWasDown = false;
    g_peerWasAlive = false;
    g_listenerAcknowledged = false;
    g_listenerAckSerial = 0;
    g_listenerCandidateSerial = 0;
    g_listenerCandidateFirstTick = 0;
    g_listenerWaitStartTick = 0;
    g_listenerStartupFailed = false;
    g_deferInitialFamilyMismatchToTitleFlow = false;
    g_initialFamilyMismatchDeferred = false;
    g_networkConfig = {};
    g_networkConfigValid = false;
    SetState(State::Idle, "reset");
}

State GetState()
{
    return g_state;
}

uint16_t HostPort()
{
    return g_port;
}

const char* HostNickname()
{
    return g_nickname.c_str();
}

bool GetHostNetworkConfig(HostSessionNetworkConfig* outConfig)
{
    if (outConfig == nullptr || !g_networkConfigValid)
    {
        return false;
    }
    *outConfig = g_networkConfig;
    return true;
}
} // namespace netplay::bridge::async_host
