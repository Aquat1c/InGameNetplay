#pragma once
// Live apply of a peer's palette row onto a char-select portrait. This is the
// one game-memory-writing piece of the palette feature: it takes a received
// wire row, remaps it to portrait indices (palette_remap), writes the colors
// into the char-select screen object's inline portrait palette buffer, and
// pushes that range to the hardware palette via EFZ's setPaletteRange.
//
// Char-select / VS / win are OUTSIDE the rollback parity island, so cosmetic
// writes here are determinism-neutral. All raw accesses are SEH-guarded.
//
// Memorial efz.exe RVAs (version-invariant; ported from efz_palette_override):
//   setPaletteRange @ 0x0040BD70  (RVA 0x0000BD70)
// Char-select screen object layout (csObj = ReadScreenObject(1)):
//   +46   : inline portrait palette buffer (4 bytes/color, R,G,B,0)
//   [8]   : graphics context (csObj+0x20) passed to setPaletteRange
//   portrait dest index base = 40*side + 175 (P1 175-214, P2 215-254)
//   setPaletteRange firstEntry = (uint8_t)(40*side - 81)  (== 175/215)

#include <cstdint>

#include "netplay/interop/overlay_protocol.h"

namespace netplay::interop::apply
{
// Apply `row` to the `side` (0=P1, 1=P2) portrait within char-select object
// `csObj`. No-op (returns false) for a stock/CLEAR row (slotByte==0), an
// unknown character, a remap that produced nothing, or any access fault.
// Returns true only when a custom palette was written AND pushed to hardware.
bool ApplyRowToCharSelectPortrait(std::uint32_t csObj, int side,
                                  const protocol::PaletteBlobBody& row);

// Re-push the `side` portrait range from the buffer as it currently stands
// (i.e. the stock palette the game's own reload just wrote). Used to REVERT a
// previously-overlaid custom palette back to stock when the selection returns
// to a color with no custom row. SEH-guarded.
bool PushCharSelectPortraitRange(std::uint32_t csObj, int side);

// Apply the winner's cached custom row onto the win (result) screen portrait.
// gameData = *(resultScreenCtx + 28); buffer = gameData + 3893; dest index base
// = 40*winnerSide + 95. Unlike char-select, the game renders the win portrait
// from this buffer, so no setPaletteRange is needed. No-op (returns false) for a
// stock/CLEAR row, unknown char, empty remap, or any fault - which is what
// avoids the offline mod's "applies even with no custom color" artifact.
bool ApplyRowToWinScreen(std::uint32_t gameData, int winnerSide,
                         const protocol::PaletteBlobBody& row);
} // namespace netplay::interop::apply
