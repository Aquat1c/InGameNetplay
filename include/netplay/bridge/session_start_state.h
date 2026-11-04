#pragma once
#include <cstdint>

namespace netplay::bridge
{
enum class SessionStartState : uint8_t { Idle, Rejected, Running, CancelRequested };

struct SessionStartLifecycle
{
    SessionStartState state = SessionStartState::Idle;
    uint32_t generation = 0;
    uint32_t workerGeneration = 0;
    bool IsRunning() const
    {
        return state == SessionStartState::Running || state == SessionStartState::CancelRequested;
    }
    bool IsRejected() const { return state == SessionStartState::Rejected; }
    bool IsCurrent(uint32_t request) const { return request == generation; }
    bool OwnsWorker(uint32_t request) const { return IsRunning() && request == workerGeneration; }
    void Reject() { state = SessionStartState::Rejected; }
    void ClearRejection() { if (IsRejected()) state = SessionStartState::Idle; }
    uint32_t Begin() { state = SessionStartState::Running; return workerGeneration = ++generation; }
    void Invalidate()
    {
        ++generation;
        if (IsRunning()) state = SessionStartState::CancelRequested;
    }
    void Finish() { state = SessionStartState::Idle; }
    void Finish(uint32_t request) { if (OwnsWorker(request)) Finish(); }
};
}
