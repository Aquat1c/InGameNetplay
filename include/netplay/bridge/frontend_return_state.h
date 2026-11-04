#pragma once

namespace netplay::bridge::frontend_return
{
enum class NativeBattleCleanupState { AwaitingUpdate, AwaitingUpdateWithPatchFailure, Reached, ReachedWithPatchFailure };
struct NativeBattleCleanupLifecycle
{
    NativeBattleCleanupState state = NativeBattleCleanupState::AwaitingUpdate;
    void Prepare(bool patchesRestored)
    {
        state = patchesRestored ? NativeBattleCleanupState::AwaitingUpdate : NativeBattleCleanupState::AwaitingUpdateWithPatchFailure;
    }
    void ObserveUpdate()
    {
        state = state == NativeBattleCleanupState::AwaitingUpdateWithPatchFailure
            || state == NativeBattleCleanupState::ReachedWithPatchFailure
            ? NativeBattleCleanupState::ReachedWithPatchFailure : NativeBattleCleanupState::Reached;
    }
    bool CanSuppressFallback() const { return state == NativeBattleCleanupState::Reached; }
};

enum class ReturnOwnership : long
{
    Inactive,
    Active,
    ContinuationPending,
    ContinuationClaimed,
    ContinuationClaimedConsumed,
    ContinuationConsumed
};
constexpr bool ReturnOwned(ReturnOwnership state)
{
    return state == ReturnOwnership::Active || state == ReturnOwnership::ContinuationPending
        || state == ReturnOwnership::ContinuationClaimed
        || state == ReturnOwnership::ContinuationClaimedConsumed;
}
constexpr bool ReturnConsumed(ReturnOwnership state)
{
    return state == ReturnOwnership::ContinuationClaimedConsumed
        || state == ReturnOwnership::ContinuationConsumed;
}
constexpr ReturnOwnership FinishReturn(ReturnOwnership state)
{
    return ReturnConsumed(state) ? ReturnOwnership::ContinuationConsumed : ReturnOwnership::Inactive;
}
constexpr ReturnOwnership ResetReturnConsumed(ReturnOwnership state)
{
    return state == ReturnOwnership::ContinuationClaimedConsumed ? ReturnOwnership::ContinuationClaimed
        : state == ReturnOwnership::ContinuationConsumed ? ReturnOwnership::Inactive : state;
}
}
