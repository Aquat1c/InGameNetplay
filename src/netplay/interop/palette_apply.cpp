#include "netplay/interop/palette_apply.h"

#include <windows.h>

#include "logger.h"
#include "netplay/interop/palette_remap.h"
#include "netplay/interop/palette_source.h"

namespace netplay::interop::apply
{
namespace
{
// setPaletteRange @ efz.exe 0x0040BD70. __thiscall(graphicsCtx, paletteData,
// firstEntry, count); count/firstEntry are single bytes.
constexpr std::uintptr_t kRvaSetPaletteRange = 0x0000BD70u;
using SetPaletteRange_t =
    int(__thiscall*)(void* graphicsCtx, int paletteData,
                     unsigned char firstEntry, unsigned char count);

// Char-select object offsets (see header).
constexpr std::uint32_t kPortraitBufferOffset = 46u;
constexpr std::uint32_t kGraphicsCtxOffset = 0x20u;   // gameContext[8]
constexpr int kPortraitDestBase = 175;                // + 40*side
constexpr unsigned char kColorCount = 0x28u;          // 40 colors

// Result (win) screen offsets: buffer = gameData + 3893, dest base = 40*w + 95.
constexpr std::uint32_t kWinBufferOffset = 3893u;
constexpr int kWinDestBase = 95;                      // + 40*winnerSide
} // namespace

bool ApplyRowToCharSelectPortrait(std::uint32_t csObj, int side,
                                  const protocol::PaletteBlobBody& row)
{
    if (csObj == 0 || (side != 0 && side != 1))
    {
        return false;
    }
    if (row.slotByte == 0)
    {
        return false;   // stock / CLEAR row: nothing custom to apply
    }
    const char* folder = source::CharFolderName(row.charId);
    if (folder == nullptr)
    {
        return false;
    }

    remap::PortraitColor colors[remap::kMaxPortraitColors];
    const std::size_t n = remap::RemapSpriteToPortrait(
        row.rawBgr, folder, colors, remap::kMaxPortraitColors);
    mod::Log("PaletteApply: side=%d char=%u folder=%s remapColors=%u",
             side, row.charId, folder, static_cast<unsigned>(n));
    if (n == 0)
    {
        return false;
    }

    const std::uintptr_t exeBase =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    if (exeBase == 0)
    {
        return false;
    }
    auto setPaletteRange =
        reinterpret_cast<SetPaletteRange_t>(exeBase + kRvaSetPaletteRange);

    bool applied = false;
    __try
    {
        const std::uintptr_t paletteBuffer = csObj + kPortraitBufferOffset;
        const int destBase = kPortraitDestBase + 40 * side;
        for (std::size_t i = 0; i < n; ++i)
        {
            // portIdx is 1-based (1..40); dest slot = destBase + portIdx - 1.
            const int destIdx = destBase + colors[i].portIdx - 1;
            auto* dest = reinterpret_cast<std::uint8_t*>(
                paletteBuffer + 4u * static_cast<std::uintptr_t>(destIdx));
            dest[0] = colors[i].r;
            dest[1] = colors[i].g;
            dest[2] = colors[i].b;
            dest[3] = 0;
        }

        void* graphicsCtx = *reinterpret_cast<void**>(csObj + kGraphicsCtxOffset);
        const unsigned char firstEntry =
            static_cast<unsigned char>(40 * side - 81);   // 175 / 215
        (void)setPaletteRange(graphicsCtx, static_cast<int>(paletteBuffer),
                              firstEntry, kColorCount);
        mod::Log("PaletteApply: wrote %u colors, setPaletteRange gfx=%p first=%u",
                 static_cast<unsigned>(n), graphicsCtx,
                 static_cast<unsigned>(firstEntry));
        applied = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log("PaletteApply: SEH fault applying side=%d char=%u (csObj=0x%08lX)",
                 side, row.charId, static_cast<unsigned long>(csObj));
        applied = false;
    }
    return applied;
}

bool ApplyRowToWinScreen(std::uint32_t gameData, int winnerSide,
                         const protocol::PaletteBlobBody& row)
{
    if (gameData == 0 || (winnerSide != 0 && winnerSide != 1))
    {
        return false;
    }
    if (row.slotByte == 0)
    {
        return false;   // stock/CLEAR row: this is the gate the offline mod lacks
    }
    const char* folder = source::CharFolderName(row.charId);
    if (folder == nullptr)
    {
        return false;
    }

    remap::PortraitColor colors[remap::kMaxPortraitColors];
    const std::size_t n = remap::RemapSpriteToPortrait(
        row.rawBgr, folder, colors, remap::kMaxPortraitColors);
    if (n == 0)
    {
        return false;
    }

    // The result screen renders the win portrait straight from gameData+3893, so
    // (unlike char-select) writing the buffer is enough - no setPaletteRange.
    bool applied = false;
    __try
    {
        const std::uintptr_t buffer = gameData + kWinBufferOffset;
        const int destBase = kWinDestBase + 40 * winnerSide;
        for (std::size_t i = 0; i < n; ++i)
        {
            const int destIdx = destBase + colors[i].portIdx - 1;
            auto* dest = reinterpret_cast<std::uint8_t*>(
                buffer + 4u * static_cast<std::uintptr_t>(destIdx));
            dest[0] = colors[i].r;
            dest[1] = colors[i].g;
            dest[2] = colors[i].b;
            dest[3] = 0;
        }
        applied = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        applied = false;
    }
    mod::Log("PaletteApply: winscreen winner=%d char=%u folder=%s colors=%u applied=%d",
             winnerSide, row.charId, folder, static_cast<unsigned>(n),
             applied ? 1 : 0);
    return applied;
}

bool PushCharSelectPortraitRange(std::uint32_t csObj, int side)
{
    if (csObj == 0 || (side != 0 && side != 1))
    {
        return false;
    }
    const std::uintptr_t exeBase =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    if (exeBase == 0)
    {
        return false;
    }
    auto setPaletteRange =
        reinterpret_cast<SetPaletteRange_t>(exeBase + kRvaSetPaletteRange);
    bool ok = false;
    __try
    {
        const std::uintptr_t paletteBuffer = csObj + kPortraitBufferOffset;
        void* graphicsCtx = *reinterpret_cast<void**>(csObj + kGraphicsCtxOffset);
        const unsigned char firstEntry =
            static_cast<unsigned char>(40 * side - 81);
        (void)setPaletteRange(graphicsCtx, static_cast<int>(paletteBuffer),
                              firstEntry, kColorCount);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    return ok;
}
} // namespace netplay::interop::apply
