#include "netplay/render/software_font.h"

#include <cctype>

namespace netplay::font
{
namespace
{
const uint8_t* GetGlyph5x7(char c)
{
    switch (c)
    {
    case 'A': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}; return g; }
    case 'B': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}; return g; }
    case 'C': { static const uint8_t g[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}; return g; }
    case 'D': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}; return g; }
    case 'E': { static const uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}; return g; }
    case 'F': { static const uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}; return g; }
    case 'G': { static const uint8_t g[7] = {0x0E, 0x11, 0x10, 0x10, 0x13, 0x11, 0x0E}; return g; }
    case 'H': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}; return g; }
    case 'I': { static const uint8_t g[7] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case 'J': { static const uint8_t g[7] = {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}; return g; }
    case 'K': { static const uint8_t g[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}; return g; }
    case 'L': { static const uint8_t g[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}; return g; }
    case 'M': { static const uint8_t g[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}; return g; }
    case 'N': { static const uint8_t g[7] = {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}; return g; }
    case 'O': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}; return g; }
    case 'P': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}; return g; }
    case 'Q': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}; return g; }
    case 'R': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}; return g; }
    case 'S': { static const uint8_t g[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}; return g; }
    case 'T': { static const uint8_t g[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}; return g; }
    case 'U': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}; return g; }
    case 'V': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}; return g; }
    case 'W': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}; return g; }
    case 'X': { static const uint8_t g[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}; return g; }
    case 'Y': { static const uint8_t g[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}; return g; }
    case 'Z': { static const uint8_t g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}; return g; }
    case 'a': { static const uint8_t g[7] = {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}; return g; }
    case 'b': { static const uint8_t g[7] = {0x10, 0x10, 0x1C, 0x12, 0x11, 0x11, 0x1E}; return g; }
    case 'c': { static const uint8_t g[7] = {0x00, 0x00, 0x0E, 0x10, 0x10, 0x10, 0x0E}; return g; }
    case 'd': { static const uint8_t g[7] = {0x01, 0x01, 0x07, 0x09, 0x11, 0x11, 0x0F}; return g; }
    case 'e': { static const uint8_t g[7] = {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}; return g; }
    case 'f': { static const uint8_t g[7] = {0x06, 0x08, 0x08, 0x1E, 0x08, 0x08, 0x08}; return g; }
    case 'g': { static const uint8_t g[7] = {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}; return g; }
    case 'h': { static const uint8_t g[7] = {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11}; return g; }
    case 'i': { static const uint8_t g[7] = {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case 'j': { static const uint8_t g[7] = {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}; return g; }
    case 'k': { static const uint8_t g[7] = {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}; return g; }
    case 'l': { static const uint8_t g[7] = {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case 'm': { static const uint8_t g[7] = {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}; return g; }
    case 'n': { static const uint8_t g[7] = {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11}; return g; }
    case 'o': { static const uint8_t g[7] = {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}; return g; }
    case 'p': { static const uint8_t g[7] = {0x00, 0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10}; return g; }
    case 'q': { static const uint8_t g[7] = {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x01}; return g; }
    case 'r': { static const uint8_t g[7] = {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}; return g; }
    case 's': { static const uint8_t g[7] = {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}; return g; }
    case 't': { static const uint8_t g[7] = {0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06}; return g; }
    case 'u': { static const uint8_t g[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}; return g; }
    case 'v': { static const uint8_t g[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}; return g; }
    case 'w': { static const uint8_t g[7] = {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}; return g; }
    case 'x': { static const uint8_t g[7] = {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}; return g; }
    case 'y': { static const uint8_t g[7] = {0x00, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x0E}; return g; }
    case 'z': { static const uint8_t g[7] = {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}; return g; }
    case '0': { static const uint8_t g[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}; return g; }
    case '1': { static const uint8_t g[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case '2': { static const uint8_t g[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}; return g; }
    case '3': { static const uint8_t g[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}; return g; }
    case '4': { static const uint8_t g[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}; return g; }
    case '5': { static const uint8_t g[7] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}; return g; }
    case '6': { static const uint8_t g[7] = {0x07, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}; return g; }
    case '7': { static const uint8_t g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}; return g; }
    case '8': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}; return g; }
    case '9': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C}; return g; }
    case ':': { static const uint8_t g[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}; return g; }
    case '.': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}; return g; }
    case '-': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}; return g; }
    case '/': { static const uint8_t g[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}; return g; }
    case '_': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}; return g; }
    case '(': { static const uint8_t g[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}; return g; }
    case ')': { static const uint8_t g[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}; return g; }
    case '+': { static const uint8_t g[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}; return g; }
    case ' ': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; return g; }
    default:
        return nullptr;
    }
}

int MeasureText5x7(const std::string& text, int scaleX)
{
    if (text.empty())
    {
        return 0;
    }
    const int glyphW = 5 * scaleX;
    const int gap = scaleX;
    return static_cast<int>(text.size()) * (glyphW + gap) - gap;
}

} // anonymous namespace

int MeasureText5x7Width(const std::string& text, int scaleX)
{
    return MeasureText5x7(text, scaleX);
}

namespace
{

void PutSurfacePixel(const IndexedSurfaceView& surface, int x, int y, uint8_t color)
{
    if (surface.pixels == nullptr || x < 0 || y < 0 || x >= surface.width || y >= surface.height)
    {
        return;
    }
    surface.pixels[y * surface.pitch + x] = color;
}

void DrawGlyph5x7(
    const IndexedSurfaceView& surface,
    int x,
    int y,
    char c,
    int scaleX,
    int scaleY,
    uint8_t color)
{
    const uint8_t* glyph = GetGlyph5x7(c);
    if (glyph == nullptr)
    {
        return;
    }
    for (int row = 0; row < 7; ++row)
    {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < 5; ++col)
        {
            if ((bits & (1u << (4 - col))) == 0)
            {
                continue;
            }
            const int px = x + col * scaleX;
            const int py = y + row * scaleY;
            for (int sy = 0; sy < scaleY; ++sy)
            {
                for (int sx = 0; sx < scaleX; ++sx)
                {
                    PutSurfacePixel(surface, px + sx, py + sy, color);
                }
            }
        }
    }
}
}

void DrawTextRight5x7(
    const IndexedSurfaceView& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleX,
    int scaleY,
    uint8_t color)
{
    const int maxWidth = rightX - leftX;
    if (maxWidth <= 0)
    {
        return;
    }

    std::string clipped = text;
    while (!clipped.empty() && MeasureText5x7(clipped, scaleX) > maxWidth)
    {
        clipped.erase(clipped.begin());
    }
    if (clipped.empty())
    {
        return;
    }

    const int glyphW = 5 * scaleX;
    const int gap = scaleX;
    int x = rightX - MeasureText5x7(clipped, scaleX);
    for (char c : clipped)
    {
        DrawGlyph5x7(surface, x, y, c, scaleX, scaleY, color);
        x += glyphW + gap;
    }
}

void DrawTextLeft5x7(
    const IndexedSurfaceView& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleX,
    int scaleY,
    uint8_t color)
{
    const int maxWidth = rightX - leftX;
    if (maxWidth <= 0)
    {
        return;
    }

    std::string clipped;
    clipped.reserve(text.size());
    for (char c : text)
    {
        std::string trial = clipped;
        trial.push_back(c);
        if (MeasureText5x7(trial, scaleX) > maxWidth)
        {
            break;
        }
        clipped.push_back(c);
    }
    if (clipped.empty())
    {
        return;
    }

    const int glyphW = 5 * scaleX;
    const int gap = scaleX;
    int x = leftX;
    for (char c : clipped)
    {
        DrawGlyph5x7(surface, x, y, c, scaleX, scaleY, color);
        x += glyphW + gap;
    }
}

void DrawTextCentered5x7(
    const IndexedSurfaceView& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleX,
    int scaleY,
    uint8_t color)
{
    const int rangeWidth = rightX - leftX;
    if (rangeWidth <= 0)
    {
        return;
    }

    std::string clipped;
    clipped.reserve(text.size());
    for (char c : text)
    {
        std::string trial = clipped;
        trial.push_back(c);
        if (MeasureText5x7(trial, scaleX) > rangeWidth)
        {
            break;
        }
        clipped.push_back(c);
    }
    if (clipped.empty())
    {
        return;
    }

    const int textW = MeasureText5x7(clipped, scaleX);
    const int startX = leftX + (rangeWidth - textW) / 2;
    const int glyphW = 5 * scaleX;
    const int gap = scaleX;
    int x = startX;
    for (char c : clipped)
    {
        DrawGlyph5x7(surface, x, y, c, scaleX, scaleY, color);
        x += glyphW + gap;
    }
}

void FillIndexedSurfaceRect(
    const IndexedSurfaceView& surface,
    int x,
    int y,
    int w,
    int h,
    uint8_t color)
{
    if (surface.pixels == nullptr || w <= 0 || h <= 0)
    {
        return;
    }

    // Clip to surface bounds
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > surface.width) x1 = surface.width;
    if (y1 > surface.height) y1 = surface.height;
    if (x0 >= x1 || y0 >= y1)
    {
        return;
    }

    const int fillW = x1 - x0;
    for (int row = y0; row < y1; ++row)
    {
        std::memset(&surface.pixels[row * surface.pitch + x0], color, fillW);
    }
}

void DrawIndexedSurfaceFrame(
    const IndexedSurfaceView& surface,
    int x,
    int y,
    int w,
    int h,
    uint8_t color)
{
    if (surface.pixels == nullptr || w <= 0 || h <= 0)
    {
        return;
    }

    // Top edge
    FillIndexedSurfaceRect(surface, x, y, w, 1, color);
    // Bottom edge
    FillIndexedSurfaceRect(surface, x, y + h - 1, w, 1, color);
    // Left edge
    FillIndexedSurfaceRect(surface, x, y, 1, h, color);
    // Right edge
    FillIndexedSurfaceRect(surface, x + w - 1, y, 1, h, color);
}
}



