#pragma once
// efz.exe RVAs (version-invariant):
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
bool ApplyRowToCharSelectPortrait(std::uint32_t csObj, int side,
                                  const protocol::PaletteBlobBody& row);

bool PushCharSelectPortraitRange(std::uint32_t csObj, int side);

constexpr int kPortraitEntries = 40;

bool CapturePortraitEntries(std::uint32_t csObj, int side,
                            std::uint32_t out[kPortraitEntries]);

bool RestorePortraitEntries(std::uint32_t csObj, int side,
                            const std::uint32_t entries[kPortraitEntries]);

bool ApplyRowToWinScreen(std::uint32_t gameData, int winnerSide,
                         const protocol::PaletteBlobBody& row);
} // namespace netplay::interop::apply
