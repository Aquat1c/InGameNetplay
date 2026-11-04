#pragma once
#include <cstdint>

namespace netplay::bridge::takeover
{
enum class HostProtocolState { Inactive, AwaitingListener, Stabilizing };
struct HostProtocolLifecycle
{
    HostProtocolState state = HostProtocolState::Inactive;
    uint32_t firstCandidateTick = 0;
    bool Active() const { return state != HostProtocolState::Inactive; }
    bool HasCandidate() const { return state == HostProtocolState::Stabilizing; }
    void Begin() { state = HostProtocolState::AwaitingListener; firstCandidateTick = 0; }
    void RejectCandidate() { Begin(); }
    void BeginCandidate(uint32_t now) { state = HostProtocolState::Stabilizing; firstCandidateTick = now; }
    bool CandidateMature(uint32_t now) const { return HasCandidate() && now - firstCandidateTick >= 100u; }
};
}
