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
// (mod_settings::IsModInteropChannelEnabled); the loopback wiring additionally
// requires IsModInteropLoopbackEnabled.

namespace netplay::interop::charselect
{
// Install the char-select hooks (idempotent). No-op and returns false unless
// the master interop flag is set. Call once at mod init after InstallHooks().
bool Install();

// Remove the hooks and end any active session (idempotent).
void Uninstall();
} // namespace netplay::interop::charselect
