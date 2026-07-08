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

// Text style profiles - one per menu/usage context, so each menu can tune
// its own font size, cell height, and vertical bias independently.  The
// style table lives in debug_overlay.cpp; the backslash debug panel exposes
// live sliders (font-size changes rebuild the atlas on Apply) so values can
// be dialed in and then hardcoded as the profile defaults.
enum class RtTextProfile : uint8_t
{
    MenuHeader = 0,   // large page headers
    MenuRow,          // generic menu rows
    Footer,           // footer tooltip lines
    BattleLogHeader,  // battle log panel titles
    BattleLogRow,     // battle log dense rows
    MenuSection,      // small inline section headers between menu rows
    Count,
};

struct RtTextItem
{
    int16_t x0 = 0;                    // logical left bound (320-space)
    int16_t x1 = 0;                    // logical right bound
    int16_t y = 0;                     // logical top of the 5x7 cell it replaces
    RtTextAlign align = RtTextAlign::Left;
    RtTextProfile profile = RtTextProfile::MenuRow;
    uint32_t rgba = 0xFFFFFFFFu;       // IM_COL32-style ABGR-packed color
    // Optional horizontal clip (logical 320-space). When clipX1 > clipX0 the
    // glyphs are scissored to [clipX0, clipX1] - used by menu slide animations
    // so text sliding past a panel edge is cut instead of spilling.
    int16_t clipX0 = 0;
    int16_t clipX1 = 0;
    char text[112] = {};
};

// True when the ImGui context and the TTF fonts are ready, i.e. submitted
// items will actually be drawn this frame. Producers use this to decide
// whether to suppress their 5x7 fallback text.
bool IsRtTextAvailable();

// True when the baked fonts cover Cyrillic and a Japanese font was merged
// into the name-carrying profiles (MenuRow/Footer/BattleLogRow) - i.e.
// non-ASCII nicknames render properly instead of as '?' glyphs.
bool RtTextHasExtendedGlyphs();

// Re-resolve the configured font face ([Others] MenuTtfFont) and rebuild the
// atlas on the next frame. Call after mod settings are saved/reloaded.
void NotifyFontSettingsChanged();

// Width of `text` in menu-logical pixels (RT pixels / 2) for the given
// profile, so producers can run layout/fitting against the TTF metrics.
// Returns -1 when the overlay is unavailable (caller falls back to 5x7
// metrics).
int MeasureRtTextWidth(RtTextProfile profile, const char* text);

// Frame protocol for producers: Begin clears the staging list, Submit appends,
// Commit publishes staging as the active list consumed by Render(). Clear
// drops the active list immediately (menu closed).
void BeginRtTextFrame();
void SubmitRtText(const RtTextItem& item);
void CommitRtTextFrame();
void ClearRtText();
}
