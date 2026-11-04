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

bool SuspendForOnlineSimulation();

void ResumeControlPlaneUpdates();

bool IsSuspendedForOnlineSimulation();

// Game thread only, outside battle.
bool OnlineCharSelectChanged();

// Game thread only, on a non-battle screen edge.
void PublishOnlineBoundary(const NetbridgeStatus& status);

// Requires a drained export worker; reads only the supplied status.
void PublishTrainingHandoffBoundary(const NetbridgeStatus& status);

const EFZNetplayState* GetExportedState();
bool CopyExportedState(EFZNetplayState* output, uint32_t outputSize, uint32_t minimumVersion);
}
