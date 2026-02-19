#pragma once

#include <cstdint>
#include <string>

namespace netplay::font
{
struct IndexedSurfaceView
{
    uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int pitch = 0;
};

void DrawTextRight5x7(
    const IndexedSurfaceView& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleX,
    int scaleY,
    uint8_t color);

void DrawTextLeft5x7(
    const IndexedSurfaceView& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleX,
    int scaleY,
    uint8_t color);
}

