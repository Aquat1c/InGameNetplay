#pragma once
#include <cstdint>

namespace netplay::bridge::async_host
{
enum class ReturnKeyState { Idle, WaitingForFrontend };
struct ReturnKeyLifecycle
{
    ReturnKeyState state = ReturnKeyState::Idle;
    int ticks = 0;
    bool Pending() const { return state == ReturnKeyState::WaitingForFrontend; }
    void Begin() { state = ReturnKeyState::WaitingForFrontend; }
    void Clear() { state = ReturnKeyState::Idle; }
    bool AdvanceTimeout()
    {
        if (!Pending()) { ticks = 0; return false; }
        if (++ticks <= 1200) return false;
        Clear();
        ticks = 0;
        return true;
    }
};

enum class ListenerState : uint8_t { Inactive, Waiting, Stabilizing, Ready, Failed };
enum class InitialFamilyMismatch : uint8_t { Disabled, Available, Deferred };

struct ListenerStartup
{
    ListenerState state = ListenerState::Inactive;
    InitialFamilyMismatch mismatch = InitialFamilyMismatch::Disabled;
    uint32_t acknowledgedSerial = 0;
    uint32_t candidateSerial = 0;
    uint32_t candidateFirstTick = 0;
    uint32_t waitStartTick = 0;

    void Reset() { *this = {}; }
    void Begin(uint32_t now, bool deferInitialMismatch)
    {
        Reset();
        state = ListenerState::Waiting;
        waitStartTick = now;
        mismatch = deferInitialMismatch
            ? InitialFamilyMismatch::Available : InitialFamilyMismatch::Disabled;
    }
    bool IsReady() const { return state == ListenerState::Ready; }
    bool IsFailed() const { return state == ListenerState::Failed; }
    bool ObserveCandidate(uint32_t serial, uint32_t now)
    {
        if (candidateSerial != serial)
        {
            candidateSerial = serial;
            candidateFirstTick = now;
            state = ListenerState::Stabilizing;
            return false;
        }
        return now - candidateFirstTick >= 100u;
    }
    bool DeferInitialFamilyMismatch()
    {
        if (mismatch != InitialFamilyMismatch::Available) return false;
        mismatch = InitialFamilyMismatch::Deferred;
        return true;
    }
    void Acknowledge(uint32_t serial)
    {
        state = ListenerState::Ready;
        acknowledgedSerial = serial;
        waitStartTick = 0;
        mismatch = InitialFamilyMismatch::Disabled;
    }
    void Fail() { state = ListenerState::Failed; }
};
}
