#pragma once
#include <cstdint>

namespace netplay::bridge::takeover
{
enum class RenderContextState { Empty, Saved, Retained };
struct RenderContextLifecycle
{
    RenderContextState state = RenderContextState::Empty;
    uintptr_t pointer = 0;
    bool HasContext() const { return state != RenderContextState::Empty; }
    bool IsSaved() const { return state == RenderContextState::Saved; }
    void Save(uintptr_t value) { pointer = value; state = RenderContextState::Saved; }
    void Clear() { pointer = 0; state = RenderContextState::Empty; }
    void Consume() { if (HasContext()) state = RenderContextState::Retained; }
};
enum class PreimageCaptureState : long { Unattempted, Capturing, Captured, Failed };
enum class SpectatorWatcherState : long { Dormant, Started, StopRequested };
enum class StallState { Dormant, TrackingSync, StalledWithoutSync, StalledWithSync };
struct StallLifecycle
{
    StallState state = StallState::Dormant;
    bool Active() const { return state == StallState::StalledWithoutSync || state == StallState::StalledWithSync; }
    bool TracksSync() const { return state == StallState::TrackingSync || state == StallState::StalledWithSync; }
    void ObserveSync() { state = Active() ? StallState::StalledWithSync : StallState::TrackingSync; }
    void Begin() { state = TracksSync() ? StallState::StalledWithSync : StallState::StalledWithoutSync; }
    void Reset(bool clearSync) { state = !clearSync && TracksSync() ? StallState::TrackingSync : StallState::Dormant; }
};
}
