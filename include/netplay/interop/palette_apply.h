#pragma once
// Portrait dest index = 175 + 40*side; buffer is 4 bytes/color at csObj+46.

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
}
