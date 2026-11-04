#pragma once
#include <cstdint>

namespace netplay::bridge::takeover
{
enum class SpectatorPostInitState : uint8_t { NotAttempted, Running, Succeeded, Failed };
enum class PeerQuitBroadcastState : long { Idle, Sending, Succeeded };
enum class InjectedBootstrapState : long { Idle, Initializing, Ready };
enum class TournamentCleanupState : long
{
    Inactive, RestoreExe, InitializeLocal, RestoreDll, PublishTitle, Quarantined
};

constexpr TournamentCleanupState CompleteTournamentCleanupStep(
    TournamentCleanupState state, bool succeeded)
{
    if (!succeeded)
        return state == TournamentCleanupState::InitializeLocal
            ? TournamentCleanupState::Quarantined : state;
    switch (state)
    {
    case TournamentCleanupState::RestoreExe: return TournamentCleanupState::InitializeLocal;
    case TournamentCleanupState::InitializeLocal: return TournamentCleanupState::RestoreDll;
    case TournamentCleanupState::RestoreDll: return TournamentCleanupState::PublishTitle;
    case TournamentCleanupState::PublishTitle: return TournamentCleanupState::Inactive;
    default: return state;
    }
}
}
