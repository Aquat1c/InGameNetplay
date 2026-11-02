#pragma once

namespace netplay::interop::charselect
{
bool Install();

// GAME THREAD ONLY: reloadCharacterPalette does not fire while EDIT COLOR is held.
void TickGameThread();
}
