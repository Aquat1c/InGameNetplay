#pragma once

#include <cstdint>

struct IDirect3DDevice9;

// ImGui-based debug overlay, rendered through the game's Direct3D9 EndScene hook.
// Two jobs:
//   1. The in-gameplay async-host indicator (drawn via ImGui's foreground draw
//      list, which works in battle/practice where the DirectDraw menu surface
//      cannot).
//   2. A DELETE-toggled debug panel for tuning on-screen coordinates/scaling so
//      values can be dialed in and then hardcoded.
//
// Availability is gated by the "debug menu" mod option (IsDebugMenuEnabled).
namespace netplay::debug_overlay
{
// Called once per D3D9 EndScene (from the battle-log EndScene hook). Lazily
// initialises ImGui (DX9 + Win32 + a chained wndproc hook) and renders whatever
// is active. Cheap no-op when nothing needs drawing.
void Render(IDirect3DDevice9* device);

// Toggle the debug panel (also bound to the DELETE key via the wndproc hook).
void ToggleDebugPanel();

// True while the debug panel is open (callers may suppress game input).
bool IsDebugPanelActive();
}
