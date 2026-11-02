#pragma once
// Must match the offline mod's algorithm exactly.

#include <cstddef>
#include <cstdint>

namespace netplay::interop::remap
{
struct PortraitColor
{
    std::uint8_t portIdx;
    std::uint8_t r, g, b;
};

inline constexpr std::size_t kMaxPortraitColors = 40u;

// 0 = unknown character or all areas skipped.
std::size_t RemapSpriteToPortrait(const std::uint8_t* raw120,
                                  const char* charName,
                                  PortraitColor* out, std::size_t cap);

void RgbToHsl(std::uint8_t r, std::uint8_t g, std::uint8_t b,
              float* h, float* s, float* l);
void HslToRgb(float h, float s, float l,
              std::uint8_t* r, std::uint8_t* g, std::uint8_t* b);
}
