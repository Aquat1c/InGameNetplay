#pragma once

#include "efz_netplay_state.h"
#include "netplay/bridge/session_bridge.h"
#include <cstring>
#include <mutex>

namespace netplay::bridge::state_export
{
class StateSnapshot
{
public:
    void Publish(const EFZNetplayState& state)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
    }

    void PublishHandoff(const NetbridgeStatus& status, uint32_t tick, uint32_t sequence)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.magic = EFZ_NETPLAY_STATE_MAGIC;
        state_.version = EFZ_NETPLAY_STATE_VERSION;
        state_.structSize = sizeof(state_);
        state_.stateSeq = sequence;
        state_.lastUpdateTick = tick;
        state_.sessionPhase = status.phase;
        state_.sessionMode = EFZ_SESSION_NONE;
        const bool active = status.phase == EFZ_PHASE_CONNECTING
            || status.phase == EFZ_PHASE_DELAY_SETUP || status.phase == EFZ_PHASE_CONNECTED;
        if (active)
        {
            switch (static_cast<NetbridgeRole>(status.role))
            {
            case NetbridgeRole::Host: state_.sessionMode = EFZ_SESSION_HOSTING; break;
            case NetbridgeRole::Join:
            case NetbridgeRole::JoinSpectate: state_.sessionMode = EFZ_SESSION_JOINING; break;
            case NetbridgeRole::Spectate: state_.sessionMode = EFZ_SESSION_SPECTATING; break;
            }
        }
        state_.localSide = status.activePlayer;
        state_.inNetplayMenu = state_.inNetplayCharacterSelect = state_.inNetplayMatch = 0;
        state_.netplayMenuScreen = state_.netplayMenuDetail = 0;
        state_.activityPhase = active
            ? (status.phase == EFZ_PHASE_DELAY_SETUP ? EFZ_ACTIVITY_DELAY_SETUP : EFZ_ACTIVITY_CONNECTING)
            : EFZ_ACTIVITY_IDLE;
        state_.endReason = EFZ_END_NONE;
        state_.capabilityFlags = (state_.capabilityFlags & EFZ_CAP_REVIVAL)
            | EFZ_CAP_SESSION | EFZ_CAP_ACTIVITY | EFZ_CAP_GAME_FLOW;
    }

    bool Copy(EFZNetplayState* output, uint32_t outputSize, uint32_t minimumVersion)
    {
        if (output == nullptr || outputSize < 12) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.magic != EFZ_NETPLAY_STATE_MAGIC || state_.version < minimumVersion
            || state_.structSize != sizeof(state_)) return false;
        const size_t copySize = outputSize < sizeof(state_) ? outputSize : sizeof(state_);
        std::memcpy(output, &state_, copySize);
        return true;
    }

    // Legacy ABI only; callers requiring coherence must use Copy.
    const EFZNetplayState* LegacyPointer() const { return &state_; }

private:
    std::mutex mutex_;
    EFZNetplayState state_{};
};
}
