#pragma once
// Char-select integration for the overlay palette feature. Hooks EFZ's
// reloadCharacterPalette / initializeCharacterSelectScreen (game thread) to:
//   - arm the overlay channel session on char-select entry,
//   - read our local char+color each palette rebuild and broadcast the row,
//   - apply the peer's received row onto their portrait.
//
// Stage 1 (this build) runs in LOCAL PLAY with a LOOPBACK sink: no network, no
// rollback involvement. Char-select is outside the parity island, so the live
// palette writes are determinism-neutral. Entirely gated by the master flag
// (mod_settings::AreOnlineCustomColorsEnabled); the loopback wiring additionally
// requires IsModInteropLoopbackEnabled.

namespace netplay::interop::charselect
{
// Install the char-select hooks (idempotent). No-op and returns false unless
// the master interop flag is set. Call once at mod init after InstallHooks().
bool Install();

// Per-frame driver, GAME THREAD ONLY: called from the per-frame tick hook's
// post-native path, after the game's own update. Runs the exchange + portrait
// apply during char-select (reloadCharacterPalette does not fire while EDIT
// COLOR is held, so the hooks alone miss it). Returns at once during a battle.
void TickGameThread();
} // namespace netplay::interop::charselect
