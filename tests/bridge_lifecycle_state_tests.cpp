#include "netplay/bridge/async_host_state.h"
#include "netplay/bridge/takeover_state.h"
#include "netplay/bridge/session_start_state.h"
#include "netplay/bridge/gameplay_exit_state.h"
#include "netplay/bridge/frontend_return_state.h"
#include <cstdio>
int main() {
    using namespace netplay::bridge;
    int failures = 0;
    const auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "%s\n", message); ++failures; }
    };
    async_host::ListenerStartup listener;
    listener.Begin(10, true);
    expect(!listener.ObserveCandidate(2, 20), "first listener observation must debounce");
    expect(!listener.ObserveCandidate(2, 119), "listener debounce excludes 99 ms");
    expect(listener.ObserveCandidate(2, 120), "listener debounce accepts 100 ms");
    expect(!listener.ObserveCandidate(4, 121), "new listener observation restarts debounce");
    expect(listener.DeferInitialFamilyMismatch(), "initial mismatch may defer once");
    expect(!listener.DeferInitialFamilyMismatch(), "second mismatch cannot defer again");
    listener.Acknowledge(4);
    expect(listener.IsReady() && !listener.IsFailed(), "acknowledgement ends listener startup");
    listener.Begin(30, false);
    expect(!listener.IsReady() && !listener.IsFailed(), "rehost clears prior acknowledgement");
    expect(!listener.DeferInitialFamilyMismatch(), "automatic family retry cannot defer mismatch");
    listener.Fail();
    expect(listener.IsFailed() && !listener.IsReady(), "failed listener cannot remain acknowledged");
    listener.Reset();
    expect(listener.state == async_host::ListenerState::Inactive, "reset retires listener startup");
    using takeover::TournamentCleanupState;
    expect(takeover::CompleteTournamentCleanupStep(TournamentCleanupState::RestoreExe, false) == TournamentCleanupState::RestoreExe, "failed EXE restoration stays retryable");
    expect(takeover::CompleteTournamentCleanupStep(TournamentCleanupState::InitializeLocal, false) == TournamentCleanupState::Quarantined, "failed local init must not be retried destructively");
    expect(takeover::CompleteTournamentCleanupStep(TournamentCleanupState::RestoreDll, false) == TournamentCleanupState::RestoreDll, "failed DLL restoration stays retryable");
    expect(takeover::CompleteTournamentCleanupStep(TournamentCleanupState::RestoreDll, true) == TournamentCleanupState::PublishTitle, "successful DLL restoration advances to title publication");
    expect(takeover::CompleteTournamentCleanupStep(TournamentCleanupState::Quarantined, true) == TournamentCleanupState::Quarantined, "quarantine cannot restart cleanup");
    SessionStartLifecycle start;
    start.Reject();
    expect(start.IsRejected() && !start.IsRunning(), "rejected admission owns no worker");
    start.ClearRejection();
    const auto generation = start.Begin();
    expect(start.IsRunning() && start.IsCurrent(generation), "queued worker owns its generation");
    start.Invalidate();
    expect(start.IsRunning() && !start.IsCurrent(generation), "cancelled worker stays running until its result is retired");
    start.Finish();
    expect(!start.IsRunning() && !start.IsRejected(), "retired worker permits a new request");
    expect(start.Begin() != generation, "new request cannot publish the cancelled generation");
    using recovery::RecoveryState;
    expect(!recovery::CanBeginRecovery(RecoveryState::MenuPending), "pending title continuation still owns recovery");
    expect(!recovery::CanBeginRecovery(RecoveryState::MenuClaimed), "claimed menu entry keeps cleanup ownership until completion");
    expect(recovery::CanBeginRecovery(RecoveryState::Completed), "completed recovery permits a later session recovery");
    auto entry = recovery::AdvanceMenuEntry(recovery::MenuEntryProgress::None, recovery::MenuEntryProgress::Consumed);
    expect(recovery::EntryConsumed(entry) && !recovery::EntryStarted(entry), "direct continuation consumption does not invent a menu start");
    entry = recovery::AdvanceMenuEntry(entry, recovery::MenuEntryProgress::Started);
    expect(recovery::EntryConsumed(entry) && recovery::EntryStarted(entry), "late menu entry preserves the consumed latch");
    using frontend_return::ReturnOwnership;
    expect(frontend_return::ReturnOwned(ReturnOwnership::ContinuationClaimed), "claimed title continuation still owns frontend return");
    expect(!frontend_return::ReturnOwned(ReturnOwnership::ContinuationConsumed), "consumed continuation permits a later return");
    expect(frontend_return::FinishReturn(ReturnOwnership::ContinuationConsumed) == ReturnOwnership::ContinuationConsumed, "completion preserves the consumed teardown-suppression latch");
    expect(frontend_return::FinishReturn(ReturnOwnership::Active) == ReturnOwnership::Inactive, "plain title return releases ownership");
    expect(frontend_return::ReturnOwned(ReturnOwnership::ContinuationClaimedConsumed), "consumed continuation retains ownership until completion");
    expect(frontend_return::ReturnConsumed(ReturnOwnership::ContinuationClaimedConsumed), "claimed continuation publishes teardown suppression before completion");
    expect(frontend_return::FinishReturn(ReturnOwnership::ContinuationClaimedConsumed) == ReturnOwnership::ContinuationConsumed, "completion releases ownership and preserves suppression");
    expect(frontend_return::ResetReturnConsumed(ReturnOwnership::ContinuationClaimedConsumed) == ReturnOwnership::ContinuationClaimed, "clearing suppression must not release an unfinished continuation");
    return failures ? 1 : 0;
}
