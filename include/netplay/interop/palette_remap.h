#pragma once
// Sprite->portrait palette remap, ported from efz_palette_override's
// LoadPaletteFromPalFileRemapped with the game-memory writes factored out: this
// stage is PURE - it turns one raw 40-color sprite .pal (the wire payload) into
// a list of portrait-index color writes. The live apply stage (later, gated)
// takes these and writes them into the actual portrait palette buffer at the
// per-screen base index (charselect P1 175- / P2 215-, win 95-/135-).
//
// Algorithm parity notes (must match the offline mod exactly):
//  - per-character area tables from palette_mapping.h (verified vs the PS1
//    randomizer script);
//  - an area is skipped when any sprite index exceeds the palette, when all its
//    colors are black, or when any index >20 is black (unset sentinel);
//  - equal sprite/portrait counts -> direct 1:1 copy (preserves intentional
//    distinct colors); otherwise multi-point HSL gradient with shortest-hue
//    interpolation across ALL sprite control points.

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

// Remap `raw120` (40 BGR triplets - the overlay wire payload; no header byte)
// for `charName` (resource folder name, case-insensitive; "nanase"/"rumi",
// "exnanase"/"doppel" etc. are aliases). Appends writes into out[0..cap-1] and
// returns the number produced. 0 = unknown character or nothing applicable
// (all areas skipped). A portrait index is emitted at most once.
std::size_t RemapSpriteToPortrait(const std::uint8_t* raw120,
                                  const char* charName,
                                  PortraitColor* out, std::size_t cap);

// Exposed for tests: the exact HSL conversions the gradient path uses
// (script-parity float math; H 0-360, S/L 0-100).
void RgbToHsl(std::uint8_t r, std::uint8_t g, std::uint8_t b,
              float* h, float* s, float* l);
void HslToRgb(float h, float s, float l,
              std::uint8_t* r, std::uint8_t* g, std::uint8_t* b);
} // namespace netplay::interop::remap
