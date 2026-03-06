#include "netplay/render/software_font.h"

#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace netplay::font
{
namespace
{

// ---------------------------------------------------------------------------
// GDI Unicode text renderer (CJK / Cyrillic / mixed)
// ---------------------------------------------------------------------------
#if defined(_WIN32)

constexpr int kUnicodeTextLaneHeightPx = 12;

struct Utf8FontRendererState
{
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBitmap = nullptr;
    void* bits = nullptr;
    int dibWidth = 0;
    int dibHeight = 0;

    HFONT font = nullptr;
    int fontPixelHeight = 0;
    int fontProfile = 0;
};

static Utf8FontRendererState g_utf8Font = {};
static bool g_loggedUtf8RendererUse = false;

static bool ContainsNonAscii(const std::string& text)
{
    for (unsigned char c : text)
    {
        if ((c & 0x80u) != 0)
        {
            return true;
        }
    }
    return false;
}

static bool ContainsCjkCodepoint(const std::wstring& text)
{
    for (wchar_t ch : text)
    {
        const unsigned int cp = static_cast<unsigned int>(ch);
        if ((cp >= 0x3040u && cp <= 0x30FFu)
            || (cp >= 0x31F0u && cp <= 0x31FFu)
            || (cp >= 0xFF66u && cp <= 0xFF9Fu)
            || (cp >= 0x4E00u && cp <= 0x9FFFu))
        {
            return true;
        }
    }
    return false;
}

static bool ContainsCyrillicCodepoint(const std::wstring& text)
{
    for (wchar_t ch : text)
    {
        const unsigned int cp = static_cast<unsigned int>(ch);
        if ((cp >= 0x0400u && cp <= 0x04FFu) || (cp >= 0x0500u && cp <= 0x052Fu))
        {
            return true;
        }
    }
    return false;
}

static bool ContainsAsciiAlnum(const std::wstring& text)
{
    for (wchar_t ch : text)
    {
        if ((ch >= L'0' && ch <= L'9')
            || (ch >= L'A' && ch <= L'Z')
            || (ch >= L'a' && ch <= L'z'))
        {
            return true;
        }
    }
    return false;
}

static std::wstring MultiByteToWideBestEffort(UINT codePage, DWORD flags, const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int required = MultiByteToWideChar(
        codePage,
        flags,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (required <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    const int written = MultiByteToWideChar(
        codePage,
        flags,
        text.c_str(),
        static_cast<int>(text.size()),
        wide.data(),
        required);
    if (written <= 0)
    {
        return {};
    }
    return wide;
}

static std::wstring Utf8ToWideBestEffort(const std::string& text)
{
    std::wstring wide = MultiByteToWideBestEffort(CP_UTF8, MB_ERR_INVALID_CHARS, text);
    if (!wide.empty())
    {
        return wide;
    }

    wide = MultiByteToWideBestEffort(CP_UTF8, 0, text);
    if (!wide.empty())
    {
        return wide;
    }

    return MultiByteToWideBestEffort(CP_ACP, 0, text);
}

static bool EnsureUtf8GdiDc()
{
    if (g_utf8Font.dc != nullptr)
    {
        return true;
    }
    g_utf8Font.dc = CreateCompatibleDC(nullptr);
    return g_utf8Font.dc != nullptr;
}

static bool EnsureUtf8ScratchDib(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return false;
    }
    if (!EnsureUtf8GdiDc())
    {
        return false;
    }

    if (g_utf8Font.dib != nullptr && g_utf8Font.dibWidth == width && g_utf8Font.dibHeight == height)
    {
        return true;
    }

    if (g_utf8Font.dib != nullptr)
    {
        if (g_utf8Font.oldBitmap != nullptr)
        {
            SelectObject(g_utf8Font.dc, g_utf8Font.oldBitmap);
            g_utf8Font.oldBitmap = nullptr;
        }
        DeleteObject(g_utf8Font.dib);
        g_utf8Font.dib = nullptr;
        g_utf8Font.bits = nullptr;
        g_utf8Font.dibWidth = 0;
        g_utf8Font.dibHeight = 0;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(g_utf8Font.dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr)
    {
        return false;
    }

    HGDIOBJ prev = SelectObject(g_utf8Font.dc, dib);
    if (prev == nullptr || prev == HGDI_ERROR)
    {
        DeleteObject(dib);
        return false;
    }

    g_utf8Font.dib = dib;
    g_utf8Font.oldBitmap = prev;
    g_utf8Font.bits = bits;
    g_utf8Font.dibWidth = width;
    g_utf8Font.dibHeight = height;
    return true;
}

static HFONT CreateUnicodeUiFont(int pixelHeight, int profile)
{
    if (pixelHeight <= 0)
    {
        return nullptr;
    }

    static const wchar_t* kCjkPureFaces[] = {
        L"MS PGothic",
        L"MS Gothic",
        L"MS UI Gothic",
        L"Meiryo UI",
        L"Yu Gothic UI",
        L"Arial Unicode MS",
    };
    static const wchar_t* kCjkMixedFaces[] = {
        L"MS PGothic",
        L"MS UI Gothic",
        L"Meiryo UI",
        L"Yu Gothic UI",
        L"Segoe UI",
        L"Arial Unicode MS",
    };
    static const wchar_t* kCyrillicFaces[] = {
        L"Segoe UI",
        L"Tahoma",
        L"Arial",
        L"Arial Unicode MS",
    };
    static const wchar_t* kGeneralFaces[] = {
        L"Segoe UI",
        L"Arial",
        L"Tahoma",
        L"MS UI Gothic",
        L"Arial Unicode MS",
    };

    const wchar_t* const* faces = kGeneralFaces;
    std::size_t faceCount = sizeof(kGeneralFaces) / sizeof(kGeneralFaces[0]);
    if (profile == 1)
    {
        faces = kCjkPureFaces;
        faceCount = sizeof(kCjkPureFaces) / sizeof(kCjkPureFaces[0]);
    }
    else if (profile == 2)
    {
        faces = kCjkMixedFaces;
        faceCount = sizeof(kCjkMixedFaces) / sizeof(kCjkMixedFaces[0]);
    }
    else if (profile == 3)
    {
        faces = kCyrillicFaces;
        faceCount = sizeof(kCyrillicFaces) / sizeof(kCyrillicFaces[0]);
    }

    for (std::size_t i = 0; i < faceCount; ++i)
    {
        const wchar_t* face = faces[i];
        HFONT font = CreateFontW(
            -pixelHeight,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_TT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            face);
        if (font == nullptr)
        {
            continue;
        }

        wchar_t actualFace[LF_FACESIZE] = {};
        HGDIOBJ old = SelectObject(g_utf8Font.dc, font);
        const int got = GetTextFaceW(g_utf8Font.dc, static_cast<int>(LF_FACESIZE), actualFace);
        if (old != nullptr && old != HGDI_ERROR)
        {
            SelectObject(g_utf8Font.dc, old);
        }
        char logOnceKey[96] = {};
        snprintf(
            logOnceKey,
            sizeof(logOnceKey),
            "NetplayFont::CreateUnicodeUiFont.profile%d.height%d",
            profile,
            pixelHeight);
        static bool s_loggedFontSelection = false;
        if (!s_loggedFontSelection)
        {
            s_loggedFontSelection = true;
            mod::Log(
                "NetplayFont::CreateUnicodeUiFont: request='%ls' actual='%ls' height=%d got=%d",
                face,
                (got > 0) ? actualFace : L"(unknown)",
                pixelHeight,
                got);
        }
        return font;
    }

    return nullptr;
}

static bool EnsureUtf8Font(int pixelHeight, int profile)
{
    if (!EnsureUtf8GdiDc())
    {
        return false;
    }
    if (g_utf8Font.font != nullptr
        && g_utf8Font.fontPixelHeight == pixelHeight
        && g_utf8Font.fontProfile == profile)
    {
        return true;
    }

    if (g_utf8Font.font != nullptr)
    {
        DeleteObject(g_utf8Font.font);
        g_utf8Font.font = nullptr;
        g_utf8Font.fontPixelHeight = 0;
    }

    g_utf8Font.font = CreateUnicodeUiFont(pixelHeight, profile);
    g_utf8Font.fontPixelHeight = (g_utf8Font.font != nullptr) ? pixelHeight : 0;
    g_utf8Font.fontProfile = (g_utf8Font.font != nullptr) ? profile : 0;
    return g_utf8Font.font != nullptr;
}

static int MeasureTextWideGdi(const std::wstring& text, int pixelHeight, int profile)
{
    if (text.empty() || !EnsureUtf8Font(pixelHeight, profile))
    {
        return 0;
    }

    HFONT old = static_cast<HFONT>(SelectObject(g_utf8Font.dc, g_utf8Font.font));
    SIZE size = {};
    const BOOL ok = GetTextExtentPoint32W(g_utf8Font.dc, text.c_str(), static_cast<int>(text.size()), &size);
    if (old != nullptr && old != HGDI_ERROR)
    {
        SelectObject(g_utf8Font.dc, old);
    }
    return ok ? size.cx : 0;
}

static std::wstring FitWideGdiWithEllipsis(const std::wstring& text, int maxWidth, int pixelHeight, int profile)
{
    if (maxWidth <= 0)
    {
        return {};
    }
    if (MeasureTextWideGdi(text, pixelHeight, profile) <= maxWidth)
    {
        return text;
    }

    const std::wstring ellipsis = L"...";
    const int ellipsisW = MeasureTextWideGdi(ellipsis, pixelHeight, profile);
    if (ellipsisW <= 0 || ellipsisW > maxWidth)
    {
        return {};
    }

    std::wstring clipped = text;
    while (!clipped.empty())
    {
        const std::wstring candidate = clipped + ellipsis;
        if (MeasureTextWideGdi(candidate, pixelHeight, profile) <= maxWidth)
        {
            return candidate;
        }
        clipped.pop_back();
    }
    return ellipsis;
}

// Detect content type and select font profile + pixel height.
static void ClassifyUtf8Text(const std::wstring& wideText, int scaleY, int* outProfile, int* outPixelHeight)
{
    const bool hasCjk = ContainsCjkCodepoint(wideText);
    const bool hasCyrillic = ContainsCyrillicCodepoint(wideText);
    const bool hasAscii = ContainsAsciiAlnum(wideText);
    const int profile = hasCjk ? ((hasAscii || hasCyrillic) ? 2 : 1) : (hasCyrillic ? 3 : 0);
    int pixelHeight;
    if (profile == 1)
        pixelHeight = std::max(10, 9 * scaleY + 1);
    else if (profile == 2)
        pixelHeight = std::max(10, 9 * scaleY);
    else if (profile == 3)
        pixelHeight = std::max(9, 8 * scaleY);
    else
        pixelHeight = std::max(8, 7 * scaleY);
    *outProfile = profile;
    *outPixelHeight = pixelHeight;
}

// Render text via GDI onto an indexed surface.  |alignMode|: 0=center, 1=left, 2=right.
static bool DrawTextUtf8Gdi(
    const IndexedSurfaceView& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleY,
    uint8_t color,
    int alignMode)
{
    if (text.empty() || surface.pixels == nullptr || rightX < leftX || scaleY <= 0)
    {
        return false;
    }

    const std::wstring wideText = Utf8ToWideBestEffort(text);
    if (wideText.empty())
    {
        return false;
    }

    int profile = 0;
    int pixelHeight = 0;
    ClassifyUtf8Text(wideText, scaleY, &profile, &pixelHeight);
    if (!EnsureUtf8Font(pixelHeight, profile))
    {
        return false;
    }

    const int maxWidth = (rightX - leftX) + 1;
    const std::wstring fitted = FitWideGdiWithEllipsis(wideText, maxWidth, pixelHeight, profile);
    if (fitted.empty())
    {
        return false;
    }

    const int textWidth = MeasureTextWideGdi(fitted, pixelHeight, profile);
    if (textWidth <= 0)
    {
        return false;
    }

    const int drawW = std::max(8, textWidth + 2);
    const int drawHUnclamped = std::max(pixelHeight + ((profile == 1) ? 3 : 2), 10);
    const int drawH = std::min(kUnicodeTextLaneHeightPx, drawHUnclamped);
    if (!EnsureUtf8ScratchDib(drawW, drawH))
    {
        return false;
    }

    std::memset(g_utf8Font.bits, 0, static_cast<std::size_t>(drawW) * static_cast<std::size_t>(drawH) * 4u);

    HFONT old = static_cast<HFONT>(SelectObject(g_utf8Font.dc, g_utf8Font.font));
    SetBkMode(g_utf8Font.dc, TRANSPARENT);
    SetTextColor(g_utf8Font.dc, RGB(255, 255, 255));
    SetTextAlign(g_utf8Font.dc, TA_LEFT | TA_TOP);
    (void)TextOutW(g_utf8Font.dc, 0, 0, fitted.c_str(), static_cast<int>(fitted.size()));
    if (old != nullptr && old != HGDI_ERROR)
    {
        SelectObject(g_utf8Font.dc, old);
    }

    int startX;
    if (alignMode == 1) // left
        startX = leftX;
    else if (alignMode == 2) // right
        startX = rightX - textWidth;
    else // center
        startX = leftX + ((maxWidth - textWidth) / 2);

    const int drawBaseY = y - 1;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(g_utf8Font.bits);
    constexpr uint8_t inkThreshold = 32;
    for (int py = 0; py < drawH; ++py)
    {
        const int dstY = drawBaseY + py;
        if (dstY < 0 || dstY >= surface.height)
        {
            continue;
        }

        for (int px = 0; px < drawW; ++px)
        {
            const int dstX = startX + px;
            if (dstX < 0 || dstX >= surface.width)
            {
                continue;
            }

            const std::size_t srcOff = (static_cast<std::size_t>(py) * static_cast<std::size_t>(drawW) + static_cast<std::size_t>(px)) * 4u;
            const uint8_t b = src[srcOff + 0];
            const uint8_t g = src[srcOff + 1];
            const uint8_t r = src[srcOff + 2];
            const uint8_t intensity = static_cast<uint8_t>(std::max({b, g, r}));
            if (intensity < inkThreshold)
            {
                continue;
            }

            surface.pixels[dstY * surface.pitch + dstX] = color;
        }
    }

    if (!g_loggedUtf8RendererUse)
    {
        g_loggedUtf8RendererUse = true;
        mod::Log(
            "NetplayFont::DrawTextUtf8Gdi: enabled for non-ASCII text height=%d profile=%d",
            pixelHeight,
            profile);
    }
    return true;
}
#endif // _WIN32
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
    case '!': { static const uint8_t g[7] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04}; return g; }
    case '^': { static const uint8_t g[7] = {0x04, 0x0A, 0x11, 0x00, 0x00, 0x00, 0x00}; return g; }
    case '[': { static const uint8_t g[7] = {0x0E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0E}; return g; }
    case ']': { static const uint8_t g[7] = {0x0E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0E}; return g; }
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
#if defined(_WIN32)
    if (ContainsNonAscii(text))
    {
        if (DrawTextUtf8Gdi(surface, text, leftX, rightX, y, scaleY, color, /*alignMode=*/2))
        {
            return;
        }
    }
#endif

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
#if defined(_WIN32)
    if (ContainsNonAscii(text))
    {
        if (DrawTextUtf8Gdi(surface, text, leftX, rightX, y, scaleY, color, /*alignMode=*/1))
        {
            return;
        }
    }
#endif

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
#if defined(_WIN32)
    if (ContainsNonAscii(text))
    {
        if (DrawTextUtf8Gdi(surface, text, leftX, rightX, y, scaleY, color, /*alignMode=*/0))
        {
            return;
        }
    }
#endif

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



