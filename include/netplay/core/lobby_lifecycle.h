#pragma once

#include <atomic>

namespace netplay::lobby
{
enum class MatchPhase { Idle, Challenging, Connected, Returning, EndingConnected, Ending };

class MatchLifecycle
{
public:
    enum class EndResult { NoMatch, AlreadyReturning, Challenge, Connected };
    bool IsBusy() const { return phase_.load() != MatchPhase::Idle; }
    bool IsChallengePending() const { return phase_.load() == MatchPhase::Challenging; }
    bool IsPlaying() const { return phase_.load() == MatchPhase::Connected; }
    bool IsReturning() const { return phase_.load() == MatchPhase::Returning; }
    bool IsConnected() const
    {
        const auto phase = phase_.load();
        return phase == MatchPhase::Connected || phase == MatchPhase::Returning
            || phase == MatchPhase::EndingConnected;
    }
    bool IsEndPending() const
    {
        const auto phase = phase_.load();
        return phase == MatchPhase::Returning || phase == MatchPhase::EndingConnected
            || phase == MatchPhase::Ending;
    }
    void Reset() { phase_.store(MatchPhase::Idle); }
    bool BeginChallenge()
    {
        auto expected = MatchPhase::Idle;
        return phase_.compare_exchange_strong(expected, MatchPhase::Challenging);
    }
    bool Connect()
    {
        auto expected = MatchPhase::Challenging;
        return phase_.compare_exchange_strong(expected, MatchPhase::Connected)
            || expected == MatchPhase::Connected;
    }
    void Reject(bool queueEnd)
    {
        auto phase = phase_.load();
        while (phase == MatchPhase::Challenging || phase == MatchPhase::Connected)
        {
            if (phase_.compare_exchange_weak(phase,
                    queueEnd ? MatchPhase::Ending : MatchPhase::Idle)) return;
        }
    }
    bool ConsumeReturn()
    {
        auto expected = MatchPhase::Returning;
        return phase_.compare_exchange_strong(expected, MatchPhase::EndingConnected);
    }
    EndResult BeginEnd()
    {
        auto phase = phase_.load();
        for (;;)
        {
            if (phase == MatchPhase::Returning) return EndResult::AlreadyReturning;
            if (phase == MatchPhase::Idle || phase == MatchPhase::Ending
                || phase == MatchPhase::EndingConnected) return EndResult::NoMatch;
            const bool challenge = phase == MatchPhase::Challenging;
            if (phase_.compare_exchange_weak(phase,
                    challenge ? MatchPhase::Ending : MatchPhase::Returning))
                return challenge ? EndResult::Challenge : EndResult::Connected;
        }
    }
private:
    std::atomic<MatchPhase> phase_{MatchPhase::Idle};
};

enum class SpectatePhase { Idle, Active, Returning };

class SpectateLifecycle
{
public:
    bool IsBusy() const { return phase_.load() != SpectatePhase::Idle; }
    bool IsActive() const { return phase_.load() == SpectatePhase::Active; }
    bool IsReturning() const { return phase_.load() == SpectatePhase::Returning; }
    void Reset() { phase_.store(SpectatePhase::Idle); }
    bool Begin() { return phase_.exchange(SpectatePhase::Active) != SpectatePhase::Active; }
    void End(bool preserveUntilRefresh)
    {
        phase_.store(preserveUntilRefresh ? SpectatePhase::Returning : SpectatePhase::Idle);
    }
    bool ConsumeReturn()
    {
        auto expected = SpectatePhase::Returning;
        return phase_.compare_exchange_strong(expected, SpectatePhase::Idle);
    }
private:
    std::atomic<SpectatePhase> phase_{SpectatePhase::Idle};
};
}
