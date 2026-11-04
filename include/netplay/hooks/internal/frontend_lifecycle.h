#pragma once

#include <atomic>
#include <cstdint>

namespace netplay::hooks::internal
{
enum class RecoveryInputPhase { Idle, FixedWindow, AwaitingRelease };

class RecoveryInputQuarantine
{
public:
    bool IsActive() const { return phase_ != RecoveryInputPhase::Idle; }
    bool IsAwaitingRelease() const { return phase_ == RecoveryInputPhase::AwaitingRelease; }
    int FramesRemaining() const { return framesRemaining_; }
    void Begin(int frames)
    {
        framesRemaining_ = frames > 0 ? frames : 0;
        phase_ = framesRemaining_ > 0 ? RecoveryInputPhase::FixedWindow : RecoveryInputPhase::AwaitingRelease;
    }
    bool Suppress(bool actionHeld)
    {
        if (!IsActive()) return false;
        if (phase_ == RecoveryInputPhase::FixedWindow)
        {
            if (--framesRemaining_ == 0) phase_ = RecoveryInputPhase::AwaitingRelease;
            return true;
        }
        if (actionHeld) return true;
        phase_ = RecoveryInputPhase::Idle;
        return false;
    }
private:
    RecoveryInputPhase phase_ = RecoveryInputPhase::Idle;
    int framesRemaining_ = 0;
};

enum class HandoffWarmupPhase { Idle, Warming, Retrying, CoolingDown };

class HandoffWarmupState
{
public:
    bool IsActive() const { return phase_ == HandoffWarmupPhase::Warming || IsRetrying(); }
    bool IsRetrying() const { return phase_ == HandoffWarmupPhase::Retrying; }
    bool IsCoolingDown() const { return phase_ == HandoffWarmupPhase::CoolingDown; }
    int Frames() const { return frames_; }
    int BlockedFrames() const { return blockedFrames_; }
    uint32_t StartTick() const { return startTick_; }
    uint32_t CooldownStartTick() const { return cooldownStartTick_; }
    void Reset() { *this = {}; }
    void Begin(uint32_t tick)
    {
        Reset();
        phase_ = HandoffWarmupPhase::Warming;
        startTick_ = tick;
    }
    bool AdvanceFrame(int target)
    {
        if (!IsActive()) return false;
        if (++frames_ >= target) phase_ = HandoffWarmupPhase::Retrying;
        return IsRetrying();
    }
    int RecordBlockedFrame() { return ++blockedFrames_; }
    void BeginCooldown(uint32_t tick)
    {
        Reset();
        phase_ = HandoffWarmupPhase::CoolingDown;
        cooldownStartTick_ = tick;
    }
private:
    HandoffWarmupPhase phase_ = HandoffWarmupPhase::Idle;
    int frames_ = 0;
    int blockedFrames_ = 0;
    uint32_t startTick_ = 0;
    uint32_t cooldownStartTick_ = 0;
};

enum class RoomReturnRefreshPhase { Idle, WaitingForShutdown, RefreshPending };

class RoomReturnRefreshState
{
public:
    bool IsWaitingForShutdown() const { return phase_ == RoomReturnRefreshPhase::WaitingForShutdown; }
    bool IsRefreshPending() const { return phase_ == RoomReturnRefreshPhase::RefreshPending; }
    void BeginShutdown() { phase_ = RoomReturnRefreshPhase::WaitingForShutdown; }
    void CompleteShutdown()
    {
        if (IsWaitingForShutdown()) phase_ = RoomReturnRefreshPhase::RefreshPending;
    }
    void Reset() { phase_ = RoomReturnRefreshPhase::Idle; }
private:
    RoomReturnRefreshPhase phase_ = RoomReturnRefreshPhase::Idle;
};

enum class RoomRequestPhase { Idle, Running, Succeeded, Failed };

class RoomRequestState
{
public:
    bool IsInFlight() const { return phase_.load(std::memory_order_acquire) != RoomRequestPhase::Idle; }
    bool HasResult() const
    {
        const auto phase = phase_.load(std::memory_order_acquire);
        return phase == RoomRequestPhase::Succeeded || phase == RoomRequestPhase::Failed;
    }
    bool Succeeded() const { return phase_.load(std::memory_order_acquire) == RoomRequestPhase::Succeeded; }
    void Begin() { phase_.store(RoomRequestPhase::Running, std::memory_order_release); }
    void Complete(bool success)
    {
        phase_.store(success ? RoomRequestPhase::Succeeded : RoomRequestPhase::Failed,
            std::memory_order_release);
    }
    void DiscardResult() { if (HasResult()) Begin(); }
    void Reset() { phase_.store(RoomRequestPhase::Idle, std::memory_order_release); }
private:
    std::atomic<RoomRequestPhase> phase_{RoomRequestPhase::Idle};
};

enum class LobbyShutdownPhase { Idle, Running, Completed };

enum class DelaySetupPhase { Hidden, Selecting, WaitingForRuntime, SyncArmed };

class DelaySetupState
{
public:
    bool IsActive() const { return phase_ != DelaySetupPhase::Hidden; }
    bool IsWaitingForRuntime() const
    {
        return phase_ == DelaySetupPhase::WaitingForRuntime || IsSyncArmed();
    }
    bool IsSyncArmed() const { return phase_ == DelaySetupPhase::SyncArmed; }
    void Reset() { phase_ = DelaySetupPhase::Hidden; }
    void BeginSelection() { phase_ = DelaySetupPhase::Selecting; }
    void AwaitRuntime() { phase_ = DelaySetupPhase::WaitingForRuntime; }
    void ArmSync() { phase_ = DelaySetupPhase::SyncArmed; }
private:
    DelaySetupPhase phase_ = DelaySetupPhase::Hidden;
};

enum class JoiningPhase { Hidden, Connecting, WaitingForGame, Failed, FailedWaitingForGame };

class JoiningState
{
public:
    bool IsActive() const { return phase_ != JoiningPhase::Hidden; }
    bool IsWaitingForGame() const
    {
        return phase_ == JoiningPhase::WaitingForGame || phase_ == JoiningPhase::FailedWaitingForGame;
    }
    bool HasFailed() const
    {
        return phase_ == JoiningPhase::Failed || phase_ == JoiningPhase::FailedWaitingForGame;
    }
    void Reset() { phase_ = JoiningPhase::Hidden; }
    void Begin() { phase_ = JoiningPhase::Connecting; }
    void Fail()
    {
        phase_ = IsWaitingForGame() ? JoiningPhase::FailedWaitingForGame : JoiningPhase::Failed;
    }
    void SetWaitingForGame(bool waiting)
    {
        if (!IsActive()) return;
        phase_ = HasFailed()
            ? (waiting ? JoiningPhase::FailedWaitingForGame : JoiningPhase::Failed)
            : (waiting ? JoiningPhase::WaitingForGame : JoiningPhase::Connecting);
    }
private:
    JoiningPhase phase_ = JoiningPhase::Hidden;
};

enum class HostStartupPhase
{
    Hidden,
    DiscoveringAddress,
    StartingSession,
    AwaitingListener,
    Listening,
    Failed,
    FailedAwaitingCancel,
};

class HostStartupState
{
public:
    bool IsActive() const { return phase_ != HostStartupPhase::Hidden; }
    bool IsDiscovering() const { return phase_ == HostStartupPhase::DiscoveringAddress; }
    bool IsSessionQueued() const
    {
        return phase_ == HostStartupPhase::AwaitingListener || IsListening();
    }
    bool IsListening() const { return phase_ == HostStartupPhase::Listening; }
    bool HasFailed() const
    {
        return phase_ == HostStartupPhase::Failed || NeedsBridgeCancel();
    }
    bool NeedsBridgeCancel() const { return phase_ == HostStartupPhase::FailedAwaitingCancel; }
    void Reset() { phase_ = HostStartupPhase::Hidden; }
    void BeginDiscovery() { phase_ = HostStartupPhase::DiscoveringAddress; }
    void BeginSessionStart() { phase_ = HostStartupPhase::StartingSession; }
    void QueueSession() { phase_ = HostStartupPhase::AwaitingListener; }
    void AwaitListener() { phase_ = HostStartupPhase::AwaitingListener; }
    void ListenerReady() { phase_ = HostStartupPhase::Listening; }
    void Fail(bool needsBridgeCancel)
    {
        phase_ = needsBridgeCancel ? HostStartupPhase::FailedAwaitingCancel : HostStartupPhase::Failed;
    }

private:
    HostStartupPhase phase_ = HostStartupPhase::Hidden;
};
}
