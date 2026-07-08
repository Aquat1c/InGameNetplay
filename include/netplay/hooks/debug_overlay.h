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

// ---------------------------------------------------------------------------
// Game-RT text overlay: menu text drawn with the crisp TTF badge font on the
// 640x480 game render target, replacing 5x7 indexed-surface text where a
// producer (currently the battle log menu) submits items. Coordinates are in
// the 320x240 menu-logical space; Render() maps them x2 onto the game RT.
// ---------------------------------------------------------------------------

enum class RtTextAlign : uint8_t
{
    Left = 0,    // anchor at x0
    Center,      // centered between x0 and x1
    Right,       // anchor right edge at x1
};

enum class RtTextSize : uint8_t
{
    Row = 0,     // dense list rows / body text
    Header,      // panel titles (badge font size)
};

struct RtTextItem
{
    int16_t x0 = 0;                    // logical left bound (320-space)
    int16_t x1 = 0;                    // logical right bound
    int16_t y = 0;                     // logical top of the 5x7 cell it replaces
    RtTextAlign align = RtTextAlign::Left;
    RtTextSize size = RtTextSize::Row;
    uint32_t rgba = 0xFFFFFFFFu;       // IM_COL32-style ABGR-packed color
    char text[112] = {};
};

// True when the ImGui context and the TTF fonts are ready, i.e. submitted
// items will actually be drawn this frame. Producers use this to decide
// whether to suppress their 5x7 fallback text.
bool IsRtTextAvailable();

// Width of `text` in menu-logical pixels (RT pixels / 2) for the given size,
// so producers can run layout/fitting against the TTF metrics. Returns -1
// when the overlay is unavailable (caller falls back to 5x7 metrics).
int MeasureRtTextWidth(RtTextSize size, const char* text);

// Frame protocol for producers: Begin clears the staging list, Submit appends,
// Commit publishes staging as the active list consumed by Render(). Clear
// drops the active list immediately (menu closed).
void BeginRtTextFrame();
void SubmitRtText(const RtTextItem& item);
void CommitRtTextFrame();
void ClearRtText();
}
