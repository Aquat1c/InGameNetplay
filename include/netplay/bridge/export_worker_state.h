#pragma once

namespace netplay::bridge::state_export
{
enum class WorkerState : long { Dormant, Running, Stopping };
constexpr bool AcceptsAsyncUpdates(WorkerState state) { return state == WorkerState::Running; }
enum class UpdateMode : long { Full, SuspendedHeartbeat };
}
