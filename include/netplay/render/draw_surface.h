#pragma once

#include <cstdint>
#include <windows.h>

namespace netplay::draw
{
struct LockedMenuSurface
{
    void* surface = nullptr;
    uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int pitch = 0;
    void* lockToken = nullptr;
    const char* label = "";
};

bool AcquireMenuDrawDc(uint32_t screenContext, HDC* outDc, void** outSurface, HWND* outWindow, bool allowWindowDc);
bool AcquirePresentedMenuDrawDc(uint32_t screenContext, HDC* outDc, void** outSurface, HWND* outWindow, bool allowWindowDc);
void ReleaseMenuDrawDc(HDC dc, void* surface, HWND window);

bool AcquireMenuDrawSurfaceLock(uint32_t screenContext, LockedMenuSurface* outSurface);
void ReleaseMenuDrawSurfaceLock(const LockedMenuSurface& surface);

void ResolveOverlayTextPaletteColors(
    uint32_t screenContext,
    uint8_t paletteStart,
    uint8_t paletteCount,
    uint8_t* outSelected,
    uint8_t* outNormal);

uint8_t ResolveBestPaletteColor(
    uint32_t screenContext,
    int targetR,
    int targetG,
    int targetB);
}
