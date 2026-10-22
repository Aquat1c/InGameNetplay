// Internal to session_bridge; consumer mods include "efz_netplay_state.h" instead.
#pragma once

#include "efz_netplay_state.h"
#include "netplay/bridge/session_bridge.h"

namespace netplay::bridge::state_export
{
void Initialize();

void Shutdown();
/// Process-termination detach only: no waiting under the loader lock.
void EmergencyShutdown();

/// Control-plane only; never called from the active rollback-frame hook.
void Update(const NetbridgeStatus& status);

/// Drains export workers before online simulation handoff; returns false on timeout.
bool SuspendForOnlineSimulation();

void ResumeControlPlaneUpdates();

/// Backs the EFZNetplay_GetState DLL export.
const EFZNetplayState* GetExportedState();
} // namespace netplay::bridge::state_export
