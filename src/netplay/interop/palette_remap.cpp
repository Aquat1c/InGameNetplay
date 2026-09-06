#include "netplay/interop/palette_remap.h"

// Static-table header: this cpp is its single translation unit.
#include "netplay/interop/palette_mapping.h"

#include <cmath>
#include <cstdlib>

namespace netplay::interop::remap
{
namespace
{
constexpr int kMaxColors = 40;

inline float ClampF(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}
} // namespace

// Script-accurate RGB -> HSL (H:0-360, S/L:0-100). Ported verbatim from the
// offline mod (which matches the PS1 randomizer's math exactly).
void RgbToHsl(std::uint8_t r, std::uint8_t g, std::uint8_t b,
              float* h, float* s, float* l)
{
    const float rf = r / 255.0f;
    const float gf = g / 255.0f;
    const float bf = b / 255.0f;

    const float cmax = (rf > gf) ? ((rf > bf) ? rf : bf) : ((gf > bf) ? gf : bf);
    const float cmin = (rf < gf) ? ((rf < bf) ? rf : bf) : ((gf < bf) ? gf : bf);
    const float delta = cmax - cmin;

    float hv = 0.0f;
    if (delta == 0.0f)      hv = 0.0f;
    else if (cmax == rf)    hv = std::fmod(((gf - bf) / delta), 6.0f);
    else if (cmax == gf)    hv = ((bf - rf) / delta) + 2.0f;
    else                    hv = ((rf - gf) / delta) + 4.0f;

    hv *= 60.0f;
    if (hv < 0.0f) hv += 360.0f;

    float lv = (cmax + cmin) / 2.0f;
    float sv = 0.0f;
    if (delta != 0.0f)
    {
        sv = delta / (1.0f - std::fabs(2.0f * lv - 1.0f));
    }

    *h = hv;
    *s = sv * 100.0f;
    *l = lv * 100.0f;
}

// Script-accurate HSL -> RGB (H:0-360, S/L:0-100). Verbatim port.
void HslToRgb(float h, float s, float l,
              std::uint8_t* r, std::uint8_t* g, std::uint8_t* b)
{
    const float sf = s / 100.0f;
    const float lf = l / 100.0f;

    const float c = (1.0f - std::fabs(2.0f * lf - 1.0f)) * sf;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = lf - c / 2.0f;

    float rf = 0.0f, gf = 0.0f, bf = 0.0f;
    if (h >= 0.0f && h < 60.0f)        { rf = c; gf = x; bf = 0.0f; }
    else if (h >= 60.0f && h < 120.0f)  { rf = x; gf = c; bf = 0.0f; }
    else if (h >= 120.0f && h < 180.0f) { rf = 0.0f; gf = c; bf = x; }
    else if (h >= 180.0f && h < 240.0f) { rf = 0.0f; gf = x; bf = c; }
    else if (h >= 240.0f && h < 300.0f) { rf = x; gf = 0.0f; bf = c; }
    else if (h >= 300.0f && h < 360.0f) { rf = c; gf = 0.0f; bf = x; }

    *r = static_cast<std::uint8_t>(
        ClampF(std::fabs((rf + m) * 255.0f), 0.0f, 255.0f));
    *g = static_cast<std::uint8_t>(
        ClampF(std::fabs((gf + m) * 255.0f), 0.0f, 255.0f));
    *b = static_cast<std::uint8_t>(
        ClampF(std::fabs((bf + m) * 255.0f), 0.0f, 255.0f));
}

std::size_t RemapSpriteToPortrait(const std::uint8_t* raw120,
                                  const char* charName,
                                  PortraitColor* out, std::size_t cap)
{
    if (raw120 == nullptr || charName == nullptr || out == nullptr || cap == 0)
    {
        return 0;
    }
    const CharacterMapping* mapping = FindCharacterMapping(charName);
    if (mapping == nullptr)
    {
        return 0;
    }

    // The wire payload is the full 40-color body, no header byte.
    const int actualColors = kMaxColors;
    std::size_t produced = 0;

    for (int portAreaIdx = 0; portAreaIdx < mapping->portraitAreaCount;
         ++portAreaIdx)
    {
        const PortraitArea& portArea = mapping->portraitAreas[portAreaIdx];
        const SpriteArea* spriteArea = FindSpriteArea(mapping, portArea.name);
        if (spriteArea == nullptr)
        {
            continue;
        }

        // All sprite indices must be inside the palette.
        bool hasAllIndices = true;
        for (int i = 0; i < spriteArea->count; ++i)
        {
            if (spriteArea->indices[i] > actualColors)
            {
                hasAllIndices = false;
                break;
            }
        }
        if (!hasAllIndices)
        {
            continue;
        }

        // Black rules: indices 1-20 may be legitimately black (outlines);
        // any black at index >20 means "unset". All-black or any-unset skips
        // the area (parity with the offline mod).
        bool allBlack = true;
        bool anyBlack = false;
        for (int i = 0; i < spriteArea->count; ++i)
        {
            const int sprIdx = spriteArea->indices[i] - 1;   // 0-based
            if (sprIdx >= 0 && sprIdx < kMaxColors)
            {
                const std::uint8_t bB = raw120[sprIdx * 3 + 0];
                const std::uint8_t gB = raw120[sprIdx * 3 + 1];
                const std::uint8_t rB = raw120[sprIdx * 3 + 2];
                if (rB != 0 || gB != 0 || bB != 0)
                {
                    allBlack = false;
                }
                else if (spriteArea->indices[i] > 20)
                {
                    anyBlack = true;
                }
            }
        }
        if (allBlack || anyBlack)
        {
            continue;
        }

        const int portStart = portArea.start;
        const int portEnd = portArea.end;
        const int portCount = std::abs(portEnd - portStart) + 1;
        const int portDir = (portEnd >= portStart) ? 1 : -1;
        const int spriteCount = spriteArea->count;
        if (spriteCount < 1)
        {
            continue;
        }

        if (spriteCount == portCount)
        {
            // Direct 1:1 (preserves intentionally distinct colors).
            for (int i = 0; i < portCount && produced < cap; ++i)
            {
                const int sprIdx = spriteArea->indices[i] - 1;
                if (sprIdx < 0 || sprIdx >= kMaxColors)
                {
                    continue;
                }
                const int portIdx = portStart + i * portDir;
                out[produced].portIdx = static_cast<std::uint8_t>(portIdx);
                out[produced].b = raw120[sprIdx * 3 + 0];
                out[produced].g = raw120[sprIdx * 3 + 1];
                out[produced].r = raw120[sprIdx * 3 + 2];
                ++produced;
            }
            continue;
        }

        // Multi-point HSL gradient across all sprite control points.
        constexpr int kMaxSpriteColors = 16;
        float h[kMaxSpriteColors], s[kMaxSpriteColors], l[kMaxSpriteColors];
        std::uint8_t r[kMaxSpriteColors], g[kMaxSpriteColors], b[kMaxSpriteColors];
        int validCount = 0;
        for (int sc = 0; sc < spriteCount && sc < kMaxSpriteColors; ++sc)
        {
            const int sprIdx = spriteArea->indices[sc] - 1;
            if (sprIdx >= 0 && sprIdx < kMaxColors)
            {
                b[validCount] = raw120[sprIdx * 3 + 0];
                g[validCount] = raw120[sprIdx * 3 + 1];
                r[validCount] = raw120[sprIdx * 3 + 2];
                RgbToHsl(r[validCount], g[validCount], b[validCount],
                         &h[validCount], &s[validCount], &l[validCount]);
                ++validCount;
            }
        }
        if (validCount < 2)
        {
            continue;
        }

        for (int i = 0; i < portCount && produced < cap; ++i)
        {
            const double t =
                (portCount > 1) ? static_cast<double>(i) / (portCount - 1) : 0.0;
            const double spritePos = t * (validCount - 1);
            const int lowerIdx = static_cast<int>(spritePos);
            const int upperIdx = lowerIdx + 1;

            std::uint8_t finalR, finalG, finalB;
            if (upperIdx >= validCount || lowerIdx == upperIdx)
            {
                finalR = r[lowerIdx];
                finalG = g[lowerIdx];
                finalB = b[lowerIdx];
            }
            else
            {
                const double localT = spritePos - lowerIdx;

                float hDiff = h[upperIdx] - h[lowerIdx];
                if (hDiff > 180.0f)  hDiff -= 360.0f;
                if (hDiff < -180.0f) hDiff += 360.0f;
                float hInterp = h[lowerIdx] + hDiff * static_cast<float>(localT);
                if (hInterp < 0.0f)    hInterp += 360.0f;
                if (hInterp >= 360.0f) hInterp -= 360.0f;

                const float sInterp = ClampF(
                    s[lowerIdx]
                        + (s[upperIdx] - s[lowerIdx]) * static_cast<float>(localT),
                    0.0f, 100.0f);
                const float lInterp = ClampF(
                    l[lowerIdx]
                        + (l[upperIdx] - l[lowerIdx]) * static_cast<float>(localT),
                    0.0f, 100.0f);

                HslToRgb(hInterp, sInterp, lInterp, &finalR, &finalG, &finalB);
            }

            const int portIdx = portStart + i * portDir;
            out[produced].portIdx = static_cast<std::uint8_t>(portIdx);
            out[produced].r = finalR;
            out[produced].g = finalG;
            out[produced].b = finalB;
            ++produced;
        }
    }

    // Manual color-sharing fixes (parity with the .ps1 "portrait specific
    // fixes"): portrait[target] = portrait[source] * factor per channel, applied
    // in order so a later fix can read an earlier one's result (matches the
    // script's sequential ff calls). The source must be a slot the area remap
    // produced; a target not already present is appended.
    for (int ffIdx = 0; ffIdx < mapping->ffFixCount; ++ffIdx)
    {
        const FfFix& fix = mapping->ffFixes[ffIdx];
        std::uint8_t sr = 0, sg = 0, sb = 0;
        bool haveSource = false;
        for (std::size_t i = 0; i < produced; ++i)
        {
            if (out[i].portIdx == static_cast<std::uint8_t>(fix.source))
            {
                sr = out[i].r; sg = out[i].g; sb = out[i].b;
                haveSource = true;
                break;
            }
        }
        if (!haveSource)
        {
            continue;   // source not remapped: cannot replicate without stock
        }
        const float f = fix.factor;
        const std::uint8_t nr =
            static_cast<std::uint8_t>(ClampF(std::floor(sr * f + 0.5f), 0.0f, 255.0f));
        const std::uint8_t ng =
            static_cast<std::uint8_t>(ClampF(std::floor(sg * f + 0.5f), 0.0f, 255.0f));
        const std::uint8_t nb =
            static_cast<std::uint8_t>(ClampF(std::floor(sb * f + 0.5f), 0.0f, 255.0f));

        bool wrote = false;
        for (std::size_t i = 0; i < produced; ++i)
        {
            if (out[i].portIdx == static_cast<std::uint8_t>(fix.target))
            {
                out[i].r = nr; out[i].g = ng; out[i].b = nb;
                wrote = true;
                break;
            }
        }
        if (!wrote && produced < cap)
        {
            out[produced].portIdx = static_cast<std::uint8_t>(fix.target);
            out[produced].r = nr;
            out[produced].g = ng;
            out[produced].b = nb;
            ++produced;
        }
    }

    return produced;
}
} // namespace netplay::interop::remap
