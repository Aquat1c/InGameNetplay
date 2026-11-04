#pragma once

namespace netplay::bridge::takeover::console_capture
{
enum class WorkerState { Stopped, Running, Draining, Stopping };
struct WorkerLifecycle
{
    WorkerState state = WorkerState::Stopped;
    bool HasWorker() const { return state != WorkerState::Stopped; }
    bool Accepting() const { return state == WorkerState::Running; }
    bool StartedOrDraining() const { return state == WorkerState::Running || state == WorkerState::Draining; }
    bool Stopping() const { return state == WorkerState::Stopping; }
    void Started() { state = WorkerState::Running; }
    void BeginDrain() { if (state == WorkerState::Running) state = WorkerState::Draining; }
    void DrainFinished() { state = WorkerState::Stopping; }
    void Joined() { state = WorkerState::Stopped; }
};
}
