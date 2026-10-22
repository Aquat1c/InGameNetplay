#pragma once
// Pure sprite->portrait palette remap: one raw 40-color sprite .pal (the wire
// payload) into portrait-index color writes (charselect P1 175- / P2 215-,
// win 95-/135-).  Must match the offline mod's algorithm exactly: area tables
// from palette_mapping.h, the area skip rules, and the 1:1-copy-vs-HSL-gradient
// choice below.

#include <cstddef>
#include <cstdint>

namespace netplay::interop::remap
{
struct PortraitColor
{
    std::uint8_t portIdx;  // 1-based portrait palette index (1..40)
    std::uint8_t r, g, b;
};

// Max writes one remap can produce (every portrait slot once).
inline constexpr std::size_t kMaxPortraitColors = 40u;

// Remap `raw120` (40 BGR triplets, no header byte) for `charName` (resource
// folder name; aliases apply).  Appends writes into out[0..cap-1], returns the
// count; 0 = unknown character or all areas skipped.  A portrait index is
// emitted at most once.
std::size_t RemapSpriteToPortrait(const std::uint8_t* raw120,
                                  const char* charName,
                                  PortraitColor* out, std::size_t cap);

// Exposed for tests: the exact HSL conversions the gradient path uses
// (H 0-360, S/L 0-100).
void RgbToHsl(std::uint8_t r, std::uint8_t g, std::uint8_t b,
              float* h, float* s, float* l);
void HslToRgb(float h, float s, float l,
              std::uint8_t* r, std::uint8_t* g, std::uint8_t* b);
} // namespace netplay::interop::remap
