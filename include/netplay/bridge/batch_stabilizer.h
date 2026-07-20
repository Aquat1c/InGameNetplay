// Batch-dispatch timing stabilizer - empirical suppressor for the Nayuki
// air-throw RNG desync (docs/NAYUKI_AWAKE_AIR_THROW_RNG_DESYNC.md).
//
// Bisection with the standalone debug probes isolated a single sufficient
// suppressor: a detour wrap on Revival's rollback batch dispatcher whose
// pre/post-dispatch sampling work sits immediately before the dispatcher's
// input-availability decision. With ONLY that wrap active the desync stops
// reproducing in every network regime (localhost and real 40-50ms); every
// other probe combination still desynced. The mechanism class matches the
// known Wine-side fix for the same game (removing a message-queue wait from
// the dinput event path): variable waits injected into the input-poll path
// put the batch decision on a knife edge; the wrap's fixed work re-aligns it.
//
// This unit is unconditional (no user setting), observe-only (zero game-state
// writes - cannot desync against stock peers), fail-closed (verified RVA or a
// unique masked-signature match required), and self-repairing across sessions
// and rehosts.
#pragma once

namespace netplay::bridge::batch_stabilizer
{

// Call once per hooked game tick (cheap fast-path when healthy). Installs the
// wrap when Revival is present, repairs it if the patch byte was lost, and
// reinstalls if EfzRevival.dll moved to a new base.
void EnsurePerTick();

} // namespace netplay::bridge::batch_stabilizer
