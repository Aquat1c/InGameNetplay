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

int g_returnKeyVk = 0;
std::string g_returnKeyDisplay = "F1";
bool g_returnKeyWasDown = false;
bool g_returnInProgress = false;
int g_returnInProgressTicks = 0;

std::string g_nickname;
HostSessionNetworkConfig g_networkConfig = {};
bool g_networkConfigValid = false;
bool g_listenerAcknowledged = false;
uint32_t g_listenerAckSerial = 0;
uint32_t g_listenerCandidateSerial = 0;
DWORD g_listenerCandidateFirstTick = 0;
DWORD g_listenerWaitStartTick = 0;
bool g_listenerStartupFailed = false;
bool g_deferInitialFamilyMismatchToTitleFlow = false;
bool g_initialFamilyMismatchDeferred = false;
int g_autoRehostCooldownTicks = 0;
constexpr DWORD kListenerAckTimeoutMs = 15000;

bool g_acceptOnArrival = false;
bool g_rehostOnArrival = false;

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
        frontend_return::EnsureFrontendReturnUpdateHooks();
        mod::Log("ASYNC_HOST_AUTO_REHOST: listener restarted on port %u",
            static_cast<unsigned>(g_port));
    }
    else
    {
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

    const std::string& binding = mod_settings::AsyncHostReturnKeyBinding();
    g_returnKeyVk = options::keybinds::VirtualKeyForBindingValue(binding);
    g_returnKeyDisplay = options::keybinds::FormatBindingValue(binding);
    if (g_returnKeyVk == 0)
    {
        g_returnKeyVk = VK_F1;
        g_returnKeyDisplay = "F1";
    }

    g_lastPromptSerial = GetDelayPromptMetrics().serial;

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

    if (g_autoRehostCooldownTicks > 0)
    {
        --g_autoRehostCooldownTicks;
    }

    const NetbridgeStatus status = GetStatus();
    const NetbridgePhase phase = static_cast<NetbridgePhase>(status.phase);
    const bool peerAliveNow = IsPeerProcessAlive();
    if (peerAliveNow)
    {
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
        && !g_listenerStartupFailed
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

        if (phase == NetbridgePhase::Idle)
        {
            mod::Log("ASYNC_HOST_CANCEL reason=session_phase=Idle");
            Reset();
            return;
        }
    }

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
