#pragma once

#include "netplay/bridge/session_bridge.h"

#include <cstdint>

// Async hosting - lets a host start a listener, optionally minimize the hosting
// overlay and keep using EFZ, and HOLD the Revival delay prompt when a peer
// connects until the user explicitly accepts. See
// shared_documentation/ASYNC_HOSTING_DESIGN.md.
//
// Design rule: this module is pure state + bridge interaction. It NEVER drives
// EFZ with fake input and NEVER reimplements the delay/handoff. On accept it
// simply releases the hold so the existing, tested DelaySetup-overlay flow in
// title_flow runs unchanged. Input handling and overlay rendering live in the
// title/menu hook layer; this module only exposes state queries.
//
// Phase 1 scope (this file's initial behaviour): prompt-hold + accept from the
// title/netplay-menu context. Later phases extend Tick() to run per-frame in all
// contexts, add the context classifier, and drive EFZ to a safe handoff context
// for accept from gameplay/charselect/result/etc.
namespace netplay::bridge::async_host
{
enum class State : uint8_t
{
    Idle = 0,           // not hosting asynchronously
    Hosting = 1,        // host listener active, waiting for a peer
    PeerFoundHeld = 2,  // peer connected, delay prompt held awaiting accept
    Accepted = 3,       // user accepted; hold released, normal flow takes over
    TimedOut = 4,       // the held peer/session went away; offer rehost
};

// Called once when a host listener has been started via the netplay menu's
// Host Start action. Transitions Idle -> Hosting and snapshots the current
// delay-prompt serial so only a NEW prompt counts as "peer found".
void OnHostStarted(uint16_t port, const char* nickname);
void OnHostStarted(
    uint16_t port,
    const char* nickname,
    const HostSessionNetworkConfig& networkConfig);

// Per-frame driver. Cheap no-op while Idle. Phase 1: called from bridge::Tick()
// (title/netplay-menu context). Advances the state machine and self-cancels if
// the underlying session/peer goes away.
void Tick();

// True while async hosting owns the session (state != Idle).
bool IsActive();

// True while the user has minimized the hosting overlay (HOST_IDLE). The
// listener stays alive; the netplay menu may be closed. Phase 1 tracks the flag
// only - gameplay-context driving lands in a later phase.
bool IsMinimized();
void SetMinimized(bool minimized);

// True while a peer has connected and the delay prompt is being held awaiting
// the user's accept. While true, title_flow must NOT auto-activate the
// DelaySetup overlay.
bool IsPeerFoundHeld();

// True while a held peer/session timed out (e.g. Revival dropped the prompt or
// the helper exited). The overlay offers a rehost via the return hotkey.
bool IsTimedOut();

// Listener readiness is separate from process startup. A pre-ack helper
// failure is a startup error, not a peer disconnect and must not enter the
// automatic rehost loop.
bool IsHostListenerReady();
bool HasHostListenerStartupFailed();
bool NotifyHostListenerReady(
    const HostListenerObservation& observation);

// Display name of the configured return/rehost hotkey (e.g. "F1"), for overlays.
const char* ReturnKeyDisplay();

// Called by the menu layer when the netplay menu has been re-entered while async
// hosting was minimized. Performs any queued on-arrival action:
//   - if the user pressed the return key while a peer was held, releases the
//     hold so the normal delay overlay appears (accept-on-arrival);
//   - returns true if a REHOST was requested (timed-out path) so the caller can
//     restart the host listener; false otherwise.
bool OnNetplayMenuRestored();

// Called by the menu layer when the netplay menu is entered. Returns true if the
// entry was triggered by the return hotkey (F1) pressed in gameplay - in which
// case the caller should un-minimize to the HOST overlay (so a held peer
// auto-accepts) instead of restoring the minimized badge. Clears the flag.
bool ConsumeReturnKeyArrival();

// Convenience gate for the existing DelaySetup overlay activation site.
// Equivalent to IsPeerFoundHeld().
bool ShouldSuppressDelayOverlay();

// User accepted the held prompt (e.g. pressed D). Releases the hold: the next
// Tick lets the normal DelaySetup overlay flow activate. Idempotent / safe to
// call when not held.
void RequestAccept();

// Cancel async hosting and tear down the underlying session.
void RequestCancel(const char* reason);

// Reset to Idle without touching the session (used when the normal flow has
// already taken ownership, or on shutdown).
void Reset();

State GetState();

// Port the host listener was started on (for restoring the hosting overlay when
// the netplay menu is re-entered after a minimize).
uint16_t HostPort();
const char* HostNickname();
bool GetHostNetworkConfig(HostSessionNetworkConfig* outConfig);
}
