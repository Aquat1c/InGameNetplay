#pragma once

namespace netplay::interop::charselect
{
bool Install();

// Per-frame driver, GAME THREAD ONLY: reloadCharacterPalette does not fire while EDIT COLOR is held.
void TickGameThread();
} // namespace netplay::interop::charselect
