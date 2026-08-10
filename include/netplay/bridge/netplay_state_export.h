// ===========================================================================
// EFZ Netplay State Export - Internal header
// ===========================================================================
//
// Internal API used by session_bridge to drive the state-export lifecycle.
// Consumer mods should include "efz_netplay_state.h" (the public header)
// instead.
// ===========================================================================
#pragma once

#include "efz_netplay_state.h"
#include "netplay/bridge/session_bridge.h"

namespace netplay::bridge::state_export
{
/// Create the named shared memory block and initialise to idle state.
/// Called once from session_bridge::Initialize().
void Initialize();

/// Tear down the shared memory mapping.
/// Called from session_bridge::Shutdown().
void Shutdown();

/// Populate the exported state from the current bridge status and live game
/// memory. Called by control-plane ticks and explicit transition publishers,
/// never by the active rollback-frame hook.
void Update(const NetbridgeStatus& status);

/// Stop accepting export requests, drain the last accepted control-plane
/// snapshot, and wait until no export worker is reading live EFZ/Revival
/// memory. Called before online simulation handoff. Returns false on timeout.
bool SuspendForOnlineSimulation();

/// Re-enable control-plane export requests after returning to menu/UI flow.
void ResumeControlPlaneUpdates();

/// Returns a pointer to the module-local copy of the exported state.
/// Used by the EFZNetplay_GetState DLL-export function.
const EFZNetplayState* GetExportedState();
} // namespace netplay::bridge::state_export
