#include "netplay/bridge/async_host_state.h"
#include "netplay/bridge/takeover_state.h"
#include "netplay/bridge/session_start_state.h"
#include "netplay/bridge/gameplay_exit_state.h"
#include "netplay/bridge/frontend_return_state.h"
#include "netplay/bridge/console_capture_state.h"
#include "netplay/bridge/export_worker_state.h"
#include "netplay/bridge/native_resource_state.h"
#include "netplay/bridge/console_handoff_policy.h"
#include "netplay/bridge/host_protocol_state.h"
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
    takeover::console_capture::WorkerLifecycle consoleWorker;
    consoleWorker.Started();
    expect(consoleWorker.Accepting() && consoleWorker.StartedOrDraining(), "running console accepts and owns queued work");
    consoleWorker.BeginDrain();
    expect(!consoleWorker.Accepting() && consoleWorker.StartedOrDraining(), "draining console rejects new work but permits close boundaries");
    consoleWorker.DrainFinished();
    expect(consoleWorker.Stopping() && consoleWorker.HasWorker() && !consoleWorker.StartedOrDraining(), "stopping console remains owned until join");
    consoleWorker.BeginDrain();
    expect(consoleWorker.Stopping() && !consoleWorker.StartedOrDraining(), "duplicate shutdown must not reopen a worker after drain completion");
    consoleWorker.Joined();
    expect(!consoleWorker.HasWorker(), "joined console permits restart");
    expect(state_export::AcceptsAsyncUpdates(state_export::WorkerState::Running), "running exporter accepts asynchronous updates");
    expect(!state_export::AcceptsAsyncUpdates(state_export::WorkerState::Stopping), "stopping exporter rejects new asynchronous work");
    takeover::RenderContextLifecycle render;
    render.Consume();
    expect(!render.HasContext(), "consuming an empty render context does not create a retained pointer");
    render.Save(123);
    render.Consume();
    expect(render.HasContext() && !render.IsSaved() && render.pointer == 123, "consumed render context remains available for later cleanup");
    render.Clear();
    expect(!render.HasContext(), "procedural rendering clears retained context");
    takeover::StallLifecycle stall;
    stall.ObserveSync();
    stall.Begin();
    stall.Reset(false);
    expect(!stall.Active() && stall.TracksSync(), "stall reset may preserve synchronization observation");
    stall.Reset(true);
    stall.Begin();
    expect(stall.Active() && !stall.TracksSync(), "stall may start before a valid sync sample");
    stall.ObserveSync();
    expect(stall.Active() && stall.TracksSync(), "first sync sample preserves an already active stall");
    using console_handoff::PromptState;
    expect(console_handoff::ReadPromptGeneration(3, 4) == PromptState::Pending, "native prompt acknowledgement requires the exact generation");
    expect(console_handoff::ReadOrderedPromptProgress(3, 4) == PromptState::Served, "ordered prompt progress accepts later served generations");
    expect(console_handoff::ReadDelayHandoff(3, 2, 4, 3) == console_handoff::DelayHandoffState::InputQueued, "queued input still belongs to the held native prompt");
    expect(console_handoff::ReadDelayHandoff(3, 2, 4, 4) == console_handoff::DelayHandoffState::InputServed, "served explicit input releases the held delay reader");
    expect(console_handoff::ReadTimeoutCleanup(true, 4, 5) == console_handoff::TimeoutCleanupState::WaitingForWake, "timeout cleanup cannot consume an earlier wake");
    expect(console_handoff::ReadTimeoutCleanup(false, 0, 0) == console_handoff::TimeoutCleanupState::Ready, "dead helper does not require a future wake");
    async_host::ReturnKeyLifecycle returnKey;
    returnKey.Begin();
    for (int tick = 0; tick < 1200; ++tick) expect(!returnKey.AdvanceTimeout(), "return key remains blocked through 1200 ticks");
    expect(returnKey.AdvanceTimeout() && !returnKey.Pending(), "return key timeout releases on tick 1201");
    returnKey.Begin();
    returnKey.Clear();
    expect(!returnKey.AdvanceTimeout() && returnKey.ticks == 0, "cleared return resets its timeout at the next poll");
    frontend_return::NativeBattleCleanupLifecycle nativeCleanup;
    nativeCleanup.Prepare(false);
    nativeCleanup.ObserveUpdate();
    expect(!nativeCleanup.CanSuppressFallback(), "native update with failed patch restoration keeps fallback available");
    nativeCleanup.Prepare(true);
    expect(!nativeCleanup.CanSuppressFallback(), "restored patches alone do not prove native cleanup ran");
    nativeCleanup.ObserveUpdate();
    expect(nativeCleanup.CanSuppressFallback(), "native cleanup suppresses fallback only after restored update entry");
    takeover::HostProtocolLifecycle protocol;
    protocol.Begin();
    expect(protocol.Active() && !protocol.HasCandidate(), "temporary protocol override remains owned before listener observation");
    protocol.BeginCandidate(100);
    expect(!protocol.CandidateMature(199) && protocol.CandidateMature(200), "protocol listener requires the original 100 ms debounce");
    protocol.RejectCandidate();
    expect(protocol.Active() && !protocol.HasCandidate(), "rejected candidate cannot release protocol restoration ownership");
    expect(takeover::ScheduledQuitReady(true, 499) == false && takeover::ScheduledQuitReady(true, 500), "graceful quit retains the 500 ms helper grace period");
    expect(takeover::ScheduledQuitReady(false, 0), "dead helper ends the graceful quit wait immediately");
    expect(console_handoff::ReadAuxiliaryScript(3, 2, 1, 30) == console_handoff::AuxiliaryScriptState::NewGeneration, "new auxiliary generation ignores an old line offset");
    expect(console_handoff::ReadAuxiliaryScript(3, 2, 3, 30) == console_handoff::AuxiliaryScriptState::Reading, "same auxiliary generation retains its line offset");
    expect(console_handoff::ReadAuxiliaryScript(3, 3, 3, 30) == console_handoff::AuxiliaryScriptState::Served, "consumed auxiliary generation cannot replay lines");
    expect(console_handoff::ReadSpectatorWaitDeadline(100, 199, 100) == console_handoff::SpectatorWaitState::Waiting, "spectator prompt waits until its exact deadline");
    expect(console_handoff::ReadSpectatorWaitDeadline(100, 200, 100) == console_handoff::SpectatorWaitState::TimedOut, "spectator prompt timeout keeps the exact deadline");
    return failures ? 1 : 0;
}
