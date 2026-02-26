#include "netplay/render/draw_surface.h"

#include "logger.h"
#include "netplay/core/constants.h"

#include <algorithm>
#include <cstring>

namespace
{
using namespace netplay::constants;

struct DrawLogState
{
    bool surfaceDcOk = false;
    bool surfaceDcUnavailable = false;
    bool windowDcFallback = false;
    bool surfaceLockOk = false;
    bool surfaceLockUnavailable = false;
    bool backbufferSurfaceDesc = false;
    bool primarySurfaceDesc = false;
    bool overlayPaletteChoice = false;
};

DrawLogState g_drawLogState = {};

bool IsExecutableAddress(uint32_t address)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0)
    {
        return false;
    }
    if (mbi.State != MEM_COMMIT)
    {
        return false;
    }

    const DWORD protect = mbi.Protect & 0xFF;
    return protect == PAGE_EXECUTE
        || protect == PAGE_EXECUTE_READ
        || protect == PAGE_EXECUTE_READWRITE
        || protect == PAGE_EXECUTE_WRITECOPY;
}

bool TryAcquireSurfaceDc(void* surface, const char* label, HDC* outDc)
{
    if (surface == nullptr)
    {
        return false;
    }
    const uint32_t vtable = *reinterpret_cast<uint32_t*>(surface);
    if (vtable == 0)
    {
        return false;
    }

    const uint32_t getDcAddress = *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceGetDc);
    if (getDcAddress == 0 || !IsExecutableAddress(getDcAddress))
    {
        return false;
    }

    auto const getDc = reinterpret_cast<HRESULT(__stdcall*)(void*, HDC*)>(getDcAddress);
    HDC dc = nullptr;
    const HRESULT hr = getDc(surface, &dc);
    if (SUCCEEDED(hr) && dc != nullptr)
    {
        if (!g_drawLogState.surfaceDcOk)
        {
            mod::Log("AcquireMenuDrawDc: using %s surface DC", label);
            g_drawLogState.surfaceDcOk = true;
        }
        *outDc = dc;
        return true;
    }
    if (!g_drawLogState.surfaceDcUnavailable)
    {
        mod::Log(
            "AcquireMenuDrawDc: %s surface GetDC failed (surface=0x%08X hr=0x%08X)",
            label,
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(surface)),
            static_cast<unsigned>(hr));
    }
    return false;
}

bool QuerySurfaceDimensions(void* surface, int* outWidth, int* outHeight)
{
    if (surface == nullptr || outWidth == nullptr || outHeight == nullptr)
    {
        return false;
    }

    const uint32_t vtable = *reinterpret_cast<uint32_t*>(surface);
    if (vtable == 0)
    {
        return false;
    }

    constexpr uint32_t kVtableOffsetSurfaceGetDesc = 88;
    const uint32_t getDescAddress = *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceGetDesc);
    if (getDescAddress == 0 || !IsExecutableAddress(getDescAddress))
    {
        return false;
    }

    auto const getDesc = reinterpret_cast<HRESULT(__stdcall*)(void*, uint32_t*)>(getDescAddress);
    uint32_t desc[36] = {};
    desc[0] = 124;
    const HRESULT hr = getDesc(surface, desc);
    if (!SUCCEEDED(hr))
    {
        return false;
    }

    const int width = static_cast<int>(desc[3]);
    const int height = static_cast<int>(desc[2]);
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    *outWidth = width;
    *outHeight = height;
    return true;
}

bool TryLockMenuSurface(void* surface, const char* label, netplay::draw::LockedMenuSurface* outSurface)
{
    if (surface == nullptr || outSurface == nullptr)
    {
        return false;
    }

    const uint32_t vtable = *reinterpret_cast<uint32_t*>(surface);
    if (vtable == 0)
    {
        return false;
    }
    const uint32_t lockAddress = *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceLock);
    if (lockAddress == 0 || !IsExecutableAddress(lockAddress))
    {
        return false;
    }

    auto const lockSurface = reinterpret_cast<HRESULT(__stdcall*)(void*, void*, uint32_t*, int, uint32_t)>(lockAddress);
    auto const unlockSurface = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(
        *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceUnlock));
    uint32_t lockDesc[36] = {};
    lockDesc[0] = 124;
    const HRESULT hr = lockSurface(surface, nullptr, lockDesc, 1, 0);
    if (SUCCEEDED(hr))
    {
        const uintptr_t ptr9 = static_cast<uintptr_t>(lockDesc[9]);
        const uintptr_t ptr8 = static_cast<uintptr_t>(lockDesc[8]);
        const uintptr_t pixelPtr = (ptr9 != 0u) ? ptr9 : ptr8;
        int width = static_cast<int>(lockDesc[3]);
        int height = static_cast<int>(lockDesc[2]);
        const int pitch = static_cast<int>(lockDesc[4]);

        int descWidth = 0;
        int descHeight = 0;
        if ((width <= 0 || height <= 0) && QuerySurfaceDimensions(surface, &descWidth, &descHeight))
        {
            width = (width > 0) ? width : descWidth;
            height = (height > 0) ? height : descHeight;
        }
        if (width <= 0 && pitch > 0)
        {
            width = pitch;
        }
        if (height <= 0)
        {
            height = 240;
        }

        if (pixelPtr == 0u || pitch <= 0 || width <= 0 || height <= 0)
        {
            if (unlockSurface != nullptr && IsExecutableAddress(reinterpret_cast<uint32_t>(unlockSurface)))
            {
                (void)unlockSurface(surface, reinterpret_cast<void*>(pixelPtr));
            }
            if (!g_drawLogState.surfaceLockUnavailable)
            {
                mod::Log(
                    "AcquireMenuDrawSurfaceLock: %s lock payload invalid (surface=0x%08X ptr9=0x%08X ptr8=0x%08X pitch=%u w=%d h=%d descW=%d descH=%d)",
                    label,
                    static_cast<unsigned>(reinterpret_cast<uintptr_t>(surface)),
                    static_cast<unsigned>(ptr9),
                    static_cast<unsigned>(ptr8),
                    static_cast<unsigned>(lockDesc[4]),
                    width,
                    height,
                    descWidth,
                    descHeight);
            }
            return false;
        }

        outSurface->surface = surface;
        outSurface->width = width;
        outSurface->height = height;
        outSurface->pitch = pitch;
        outSurface->pixels = reinterpret_cast<uint8_t*>(pixelPtr);
        outSurface->lockToken = reinterpret_cast<void*>(pixelPtr);
        outSurface->label = label;
        if (!g_drawLogState.surfaceLockOk)
        {
            mod::Log(
                "AcquireMenuDrawSurfaceLock: locked %s surface (surface=0x%08X size=%dx%d pitch=%d)",
                label,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(surface)),
                outSurface->width,
                outSurface->height,
                outSurface->pitch);
            g_drawLogState.surfaceLockOk = true;
        }
        if ((strcmp(label, "backbuffer") == 0 && !g_drawLogState.backbufferSurfaceDesc)
            || (strcmp(label, "primary") == 0 && !g_drawLogState.primarySurfaceDesc))
        {
            mod::Log(
                "AcquireMenuDrawSurfaceLock: %s surface details size=%dx%d pitch=%d (lockW=%u lockH=%u)",
                label,
                outSurface->width,
                outSurface->height,
                outSurface->pitch,
                static_cast<unsigned>(lockDesc[3]),
                static_cast<unsigned>(lockDesc[2]));
            if (strcmp(label, "backbuffer") == 0)
            {
                g_drawLogState.backbufferSurfaceDesc = true;
            }
            else if (strcmp(label, "primary") == 0)
            {
                g_drawLogState.primarySurfaceDesc = true;
            }
        }
        return true;
    }

    if (!g_drawLogState.surfaceLockUnavailable)
    {
        mod::Log(
            "AcquireMenuDrawSurfaceLock: %s surface Lock failed (surface=0x%08X hr=0x%08X)",
            label,
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(surface)),
            static_cast<unsigned>(hr));
    }
    return false;
}
}

namespace netplay::draw
{
bool AcquireMenuDrawDc(uint32_t screenContext, HDC* outDc, void** outSurface, HWND* outWindow, bool allowWindowDc)
{
    if (outDc == nullptr || outSurface == nullptr || outWindow == nullptr)
    {
        return false;
    }

    *outDc = nullptr;
    *outSurface = nullptr;
    *outWindow = nullptr;

    const uint32_t graphicsContext = *reinterpret_cast<uint32_t*>(screenContext + kOffsetGraphicsContext);
    if (graphicsContext != 0)
    {
        void* backBufferSurface = *reinterpret_cast<void**>(graphicsContext + kOffsetGraphicsBackBufferSurface);
        if (TryAcquireSurfaceDc(backBufferSurface, "backbuffer", outDc))
        {
            *outSurface = backBufferSurface;
            return true;
        }

        void* primarySurface = *reinterpret_cast<void**>(graphicsContext + kOffsetGraphicsPrimarySurface);
        if (TryAcquireSurfaceDc(primarySurface, "primary", outDc))
        {
            *outSurface = primarySurface;
            return true;
        }
    }

    if (allowWindowDc)
    {
        const HWND hwnd = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
        if (hwnd != nullptr && IsWindow(hwnd))
        {
            HDC windowDc = GetDC(hwnd);
            if (windowDc != nullptr)
            {
                if (!g_drawLogState.windowDcFallback)
                {
                    mod::Log("AcquireMenuDrawDc: using window DC fallback");
                    g_drawLogState.windowDcFallback = true;
                }
                *outDc = windowDc;
                *outWindow = hwnd;
                return true;
            }
        }
    }
    if (!g_drawLogState.surfaceDcUnavailable)
    {
        mod::Log("AcquireMenuDrawDc: no usable surface DC");
        g_drawLogState.surfaceDcUnavailable = true;
    }
    return false;
}

void ReleaseMenuDrawDc(HDC dc, void* surface, HWND window)
{
    if (dc == nullptr)
    {
        return;
    }

    if (surface != nullptr)
    {
        const uint32_t vtable = *reinterpret_cast<uint32_t*>(surface);
        if (vtable != 0)
        {
            const uint32_t releaseAddress = *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceReleaseDc);
            if (releaseAddress != 0 && IsExecutableAddress(releaseAddress))
            {
                auto const releaseDc = reinterpret_cast<HRESULT(__stdcall*)(void*, HDC)>(releaseAddress);
                (void)releaseDc(surface, dc);
                return;
            }
        }
    }
    if (window != nullptr && IsWindow(window))
    {
        ReleaseDC(window, dc);
    }
}

bool AcquireMenuDrawSurfaceLock(uint32_t screenContext, LockedMenuSurface* outSurface)
{
    if (outSurface == nullptr)
    {
        return false;
    }

    *outSurface = {};
    const uint32_t graphicsContext = *reinterpret_cast<uint32_t*>(screenContext + kOffsetGraphicsContext);
    if (graphicsContext == 0)
    {
        return false;
    }

    void* backBufferSurface = *reinterpret_cast<void**>(graphicsContext + kOffsetGraphicsBackBufferSurface);
    if (TryLockMenuSurface(backBufferSurface, "backbuffer", outSurface))
    {
        return true;
    }

    void* primarySurface = *reinterpret_cast<void**>(graphicsContext + kOffsetGraphicsPrimarySurface);
    if (TryLockMenuSurface(primarySurface, "primary", outSurface))
    {
        return true;
    }

    if (!g_drawLogState.surfaceLockUnavailable)
    {
        mod::Log("AcquireMenuDrawSurfaceLock: no lockable menu surface");
        g_drawLogState.surfaceLockUnavailable = true;
    }
    return false;
}

void ReleaseMenuDrawSurfaceLock(const LockedMenuSurface& surface)
{
    if (surface.surface == nullptr || surface.lockToken == nullptr)
    {
        return;
    }
    const uint32_t vtable = *reinterpret_cast<uint32_t*>(surface.surface);
    if (vtable == 0)
    {
        return;
    }
    const uint32_t unlockAddress = *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceUnlock);
    if (unlockAddress == 0 || !IsExecutableAddress(unlockAddress))
    {
        return;
    }
    auto const unlockSurface = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(unlockAddress);
    (void)unlockSurface(surface.surface, surface.lockToken);
}

void ResolveOverlayTextPaletteColors(
    uint32_t screenContext,
    uint8_t paletteStart,
    uint8_t paletteCount,
    uint8_t* outSelected,
    uint8_t* outNormal)
{
    if (outSelected == nullptr || outNormal == nullptr)
    {
        return;
    }

    const uint8_t transparentColor = *reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor);
    const uint8_t* palette = reinterpret_cast<uint8_t*>(screenContext + kOffsetPalette);

    int bestIndex = -1;
    int bestScore = -0x7FFFFFFF;
    for (int i = 0; i < paletteCount; ++i)
    {
        const int idx = static_cast<int>(paletteStart) + i;
        if (idx < 0 || idx >= 256 || idx == static_cast<int>(transparentColor))
        {
            continue;
        }
        const int r = palette[idx * 4 + 0];
        const int g = palette[idx * 4 + 1];
        const int b = palette[idx * 4 + 2];
        const int luma = 30 * r + 59 * g + 11 * b;
        const int cmax = (std::max)((std::max)(r, g), b);
        const int cmin = (std::min)((std::min)(r, g), b);
        const int saturation = cmax - cmin;
        const int score = luma - saturation * 220;
        if (score > bestScore)
        {
            bestScore = score;
            bestIndex = idx;
        }
    }

    if (bestIndex < 0)
    {
        *outSelected = paletteStart;
        *outNormal = static_cast<uint8_t>(paletteStart + ((paletteCount > 1) ? (paletteCount - 1) : 0));
        return;
    }

    const int selR = palette[bestIndex * 4 + 0];
    const int selG = palette[bestIndex * 4 + 1];
    const int selB = palette[bestIndex * 4 + 2];
    const int selectedLuma = 30 * selR + 59 * selG + 11 * selB;
    const int targetLuma = (selectedLuma * 65) / 100;
    int normalIndex = -1;
    int bestDelta = 0x7FFFFFFF;
    for (int i = 0; i < paletteCount; ++i)
    {
        const int idx = static_cast<int>(paletteStart) + i;
        if (idx < 0 || idx >= 256 || idx == static_cast<int>(transparentColor) || idx == bestIndex)
        {
            continue;
        }
        const int r = palette[idx * 4 + 0];
        const int g = palette[idx * 4 + 1];
        const int b = palette[idx * 4 + 2];
        const int luma = 30 * r + 59 * g + 11 * b;
        const int cmax = (std::max)((std::max)(r, g), b);
        const int cmin = (std::min)((std::min)(r, g), b);
        const int saturation = cmax - cmin;
        const int delta = std::abs(luma - targetLuma) + saturation * 8;
        if (delta < bestDelta)
        {
            bestDelta = delta;
            normalIndex = idx;
        }
    }

    if (normalIndex < 0)
    {
        normalIndex = bestIndex;
    }

    *outSelected = static_cast<uint8_t>(bestIndex);
    *outNormal = static_cast<uint8_t>(normalIndex);

    if (!g_drawLogState.overlayPaletteChoice)
    {
        const int nr = palette[normalIndex * 4 + 0];
        const int ng = palette[normalIndex * 4 + 1];
        const int nb = palette[normalIndex * 4 + 2];
        mod::Log(
            "OverlayTextPalette: selected=%u rgb=(%d,%d,%d) normal=%u rgb=(%d,%d,%d) transparent=%u",
            static_cast<unsigned>(*outSelected),
            selR,
            selG,
            selB,
            static_cast<unsigned>(*outNormal),
            nr,
            ng,
            nb,
            static_cast<unsigned>(transparentColor));
        g_drawLogState.overlayPaletteChoice = true;
    }
}

uint8_t ResolveBestPaletteColor(
    uint32_t screenContext,
    int targetR,
    int targetG,
    int targetB)
{
    constexpr uint32_t kOffsetPalette = 46;
    constexpr uint32_t kOffsetTransparentColor = 1070;

    const uint8_t transparentColor = *reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor);
    const uint8_t* palette = reinterpret_cast<uint8_t*>(screenContext + kOffsetPalette);

    int bestIndex = 0;
    int bestDist = 0x7FFFFFFF;
    for (int i = 0; i < 256; ++i)
    {
        if (i == static_cast<int>(transparentColor))
        {
            continue;
        }
        const int r = palette[i * 4 + 0];
        const int g = palette[i * 4 + 1];
        const int b = palette[i * 4 + 2];
        const int dr = r - targetR;
        const int dg = g - targetG;
        const int db = b - targetB;
        const int dist = dr * dr + dg * dg + db * db;
        if (dist < bestDist)
        {
            bestDist = dist;
            bestIndex = i;
        }
    }
    return static_cast<uint8_t>(bestIndex);
}
}


