#include "netplay/hooks/internal/frontend_lifecycle.h"
#include "netplay/core/lobby_lifecycle.h"
#include <cassert>

using namespace netplay::hooks::internal;

int main()
{
    netplay::lobby::MatchLifecycle match;
    using EndResult = netplay::lobby::MatchLifecycle::EndResult;
    assert(!match.IsBusy() && match.BeginEnd() == EndResult::NoMatch);
    match.BeginChallenge();
    assert(match.IsChallengePending() && !match.IsConnected());
    assert(match.BeginEnd() == EndResult::Challenge);
    assert(match.IsEndPending() && !match.IsReturning());
    assert(match.BeginEnd() == EndResult::NoMatch);
    match.Reset();
    match.BeginChallenge();
    match.Connect();
    assert(match.IsPlaying() && match.IsConnected() && !match.IsChallengePending());
    assert(match.BeginEnd() == EndResult::Connected);
    assert(match.IsReturning() && match.IsEndPending() && !match.IsPlaying());
    assert(match.BeginEnd() == EndResult::AlreadyReturning);
    assert(match.ConsumeReturn());
    assert(!match.ConsumeReturn() && match.IsConnected() && match.IsEndPending());
    assert(match.BeginEnd() == EndResult::NoMatch);
    assert(match.IsEndPending() && !match.IsReturning());
    assert(!match.BeginChallenge());
    assert(!match.Connect());
    match.Reject(false);
    assert(match.IsEndPending());
    match.Reset();
    assert(!match.Connect());
    assert(match.BeginChallenge());
    match.Reject(false);
    assert(!match.IsBusy());
    match.BeginChallenge();
    match.Reject(true);
    assert(match.IsEndPending());
    match.Reset();
    assert(!match.IsBusy());

    netplay::lobby::SpectateLifecycle spectate;
    assert(!spectate.IsBusy());
    assert(spectate.Begin() && !spectate.Begin());
    spectate.End(true);
    assert(spectate.IsReturning() && spectate.IsBusy() && !spectate.IsActive());
    assert(spectate.ConsumeReturn() && !spectate.ConsumeReturn());
    assert(!spectate.IsBusy());
    spectate.Begin();
    spectate.End(false);
    assert(!spectate.IsBusy());

    RoomRequestState roomRequest;
    assert(!roomRequest.IsInFlight());
    roomRequest.Begin();
    assert(roomRequest.IsInFlight() && !roomRequest.HasResult());
    roomRequest.Complete(false);
    assert(roomRequest.HasResult() && !roomRequest.Succeeded() && roomRequest.IsInFlight());
    roomRequest.DiscardResult();
    assert(!roomRequest.HasResult() && roomRequest.IsInFlight());
    roomRequest.Complete(true);
    assert(roomRequest.HasResult() && roomRequest.Succeeded());
    roomRequest.Reset();
    assert(!roomRequest.IsInFlight());
    roomRequest.DiscardResult();
    assert(!roomRequest.IsInFlight());

    DelaySetupState delay;
    assert(!delay.IsActive());
    delay.BeginSelection();
    assert(delay.IsActive() && !delay.IsWaitingForRuntime());
    delay.AwaitRuntime();
    assert(delay.IsWaitingForRuntime() && !delay.IsSyncArmed());
    delay.ArmSync();
    assert(delay.IsWaitingForRuntime() && delay.IsSyncArmed());
    delay.BeginSelection();
    assert(!delay.IsWaitingForRuntime() && !delay.IsSyncArmed());
    delay.Reset();
    assert(!delay.IsActive());

    JoiningState join;
    join.SetWaitingForGame(false);
    assert(!join.IsActive());
    join.Begin();
    assert(join.IsActive() && !join.HasFailed());
    join.SetWaitingForGame(true);
    assert(join.IsWaitingForGame());
    join.Fail();
    assert(join.HasFailed() && join.IsWaitingForGame());
    join.SetWaitingForGame(false);
    assert(join.HasFailed() && !join.IsWaitingForGame());
    join.Reset();
    assert(!join.IsActive());

    HostStartupState host;
    assert(!host.IsActive());
    host.BeginDiscovery();
    assert(host.IsDiscovering() && !host.IsSessionQueued());
    host.BeginSessionStart();
    assert(host.IsActive() && !host.IsDiscovering() && !host.IsSessionQueued());
    host.QueueSession();
    assert(host.IsSessionQueued() && !host.IsListening());
    host.ListenerReady();
    assert(host.IsSessionQueued() && host.IsListening());
    host.AwaitListener();
    assert(!host.IsListening() && host.IsSessionQueued());
    host.Fail(true);
    assert(host.IsActive() && host.HasFailed() && host.NeedsBridgeCancel());
    assert(!host.IsSessionQueued());
    host.Fail(false);
    assert(host.HasFailed() && !host.NeedsBridgeCancel());
    host.Reset();
    assert(!host.IsActive() && !host.HasFailed());
}
