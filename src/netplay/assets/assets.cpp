#include "netplay/assets/assets.h"

#include "logger.h"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>

namespace
{
using namespace netplay::constants;

bool ContainsNoCase(std::string_view text, std::string_view needle)
{
    if (needle.empty() || needle.size() > text.size())
    {
        return false;
    }

    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i)
    {
        bool matched = true;
        for (std::size_t j = 0; j < needle.size(); ++j)
        {
            char a = text[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
            {
                a = static_cast<char>(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z')
            {
                b = static_cast<char>(b - 'A' + 'a');
            }
            if (a != b)
            {
                matched = false;
                break;
            }
        }
        if (matched)
        {
            return true;
        }
    }

    return false;
}

struct RowRun
{
    int start = 0;
    int length = 0;
};
}

namespace netplay::assets
{
bool FileExists(const std::string& path)
{
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string JoinPath(const std::string& left, const char* right)
{
    if (left.empty())
    {
        return right;
    }
    if (left.back() == '\\' || left.back() == '/')
    {
        return left + right;
    }
    return left + "\\" + right;
}

std::string BuildModuleDirectory(HMODULE moduleHandle)
{
    if (moduleHandle == nullptr)
    {
        return ".";
    }

    char modulePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return ".";
    }

    std::string path(modulePath);
    const std::size_t slashPos = path.find_last_of("\\/");
    if (slashPos == std::string::npos)
    {
        return ".";
    }

    path.resize(slashPos);
    return path;
}

std::string ResolveNetplayBackgroundPath(const std::string& moduleDirectory)
{
    mod::Log("ResolveNetplayBackgroundPath: searching DLL-relative assets");
    const std::array<const char*, 2> candidates = {
        "assets\\netplay_bg.dat",
        "netplay_bg.dat",
    };

    for (const char* candidate : candidates)
    {
        const std::string path = JoinPath(moduleDirectory, candidate);
        mod::Log("ResolveNetplayBackgroundPath: probing '%s'", path.c_str());
        if (FileExists(path))
        {
            mod::Log("ResolveNetplayBackgroundPath: using '%s'", path.c_str());
            return path;
        }
    }

    if (FileExists("netplay_bg.dat"))
    {
        mod::Log("ResolveNetplayBackgroundPath: fallback working-directory file 'netplay_bg.dat'");
        return "netplay_bg.dat";
    }

    mod::Log("ResolveNetplayBackgroundPath: no candidate found");
    return {};
}

std::string ResolveNetplayObjectsPath(const std::string& moduleDirectory)
{
    mod::Log("ResolveNetplayObjectsPath: searching DLL-relative assets");
    const std::array<const char*, 6> candidates = {
        "assets\\netplay_ob.dat",
        "assets\\netplay_ui_ob.dat",
        "assets\\config_ob.dat",
        "netplay_ob.dat",
        "netplay_ui_ob.dat",
        "config_ob.dat",
    };

    for (const char* candidate : candidates)
    {
        const std::string path = JoinPath(moduleDirectory, candidate);
        mod::Log("ResolveNetplayObjectsPath: probing '%s'", path.c_str());
        if (FileExists(path))
        {
            mod::Log("ResolveNetplayObjectsPath: using '%s'", path.c_str());
            return path;
        }
    }

    mod::Log("ResolveNetplayObjectsPath: fallback to vanilla title objects (no DLL-local netplay object found)");
    return "system\\title_ob.dat";
}

bool ParseEfzDatImage(const std::string& path, ParsedDatImage* outImage)
{
    if (outImage == nullptr)
    {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        mod::Log("ParseEfzDatImage: failed to open '%s'", path.c_str());
        return false;
    }

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 1 + 3 + 4)
    {
        mod::Log("ParseEfzDatImage: file too small '%s' (%zu bytes)", path.c_str(), bytes.size());
        return false;
    }

    auto tryHeader = [&bytes](size_t headerOffset, int* outW, int* outH, size_t* outPixelOffset, size_t* outPixelCount)
    {
        if (headerOffset + 4u > bytes.size())
        {
            return false;
        }

        const int width = static_cast<int>(bytes[headerOffset] | (bytes[headerOffset + 1] << 8));
        const int height = static_cast<int>(bytes[headerOffset + 2] | (bytes[headerOffset + 3] << 8));
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        {
            return false;
        }

        const size_t pixelOffset = headerOffset + 4u;
        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (pixelCount == 0 || pixelOffset + pixelCount > bytes.size())
        {
            return false;
        }

        *outW = width;
        *outH = height;
        *outPixelOffset = pixelOffset;
        *outPixelCount = pixelCount;
        return true;
    };

    std::vector<size_t> candidateOffsets;
    candidateOffsets.reserve(20);
    candidateOffsets.push_back(1u + (static_cast<size_t>(bytes[0]) + 1u) * 3u);
    candidateOffsets.push_back(1u + static_cast<size_t>(bytes[0]) * 3u);
    candidateOffsets.push_back(769u);
    candidateOffsets.push_back(766u);
    for (size_t o = 760; o <= 772; ++o)
    {
        candidateOffsets.push_back(o);
    }

    std::vector<size_t> offsets;
    offsets.reserve(candidateOffsets.size());
    for (size_t off : candidateOffsets)
    {
        bool seen = false;
        for (size_t existing : offsets)
        {
            if (existing == off)
            {
                seen = true;
                break;
            }
        }
        if (!seen)
        {
            offsets.push_back(off);
        }
    }

    bool found = false;
    size_t headerOffset = 0;
    size_t pixelOffset = 0;
    size_t pixelCount = 0;
    int width = 0;
    int height = 0;
    int bestScore = -1;
    for (size_t off : offsets)
    {
        int w = 0;
        int h = 0;
        size_t po = 0;
        size_t pc = 0;
        if (!tryHeader(off, &w, &h, &po, &pc))
        {
            continue;
        }

        int score = 0;
        if (po + pc == bytes.size())
        {
            score += 10000;
        }
        if (w == 320)
        {
            score += 100;
        }
        if (h == 240 || h == 480)
        {
            score += 100;
        }
        if (off == 769 || off == 766)
        {
            score += 50;
        }

        if (!found || score > bestScore)
        {
            found = true;
            bestScore = score;
            headerOffset = off;
            pixelOffset = po;
            pixelCount = pc;
            width = w;
            height = h;
        }
    }

    if (!found)
    {
        mod::Log(
            "ParseEfzDatImage: could not resolve header layout for '%s' (size=%zu firstByte=%u)",
            path.c_str(),
            bytes.size(),
            static_cast<unsigned>(bytes[0]));
        return false;
    }

    outImage->width = width;
    outImage->height = height;
    outImage->pixelsTopDown.resize(pixelCount);

    const uint8_t* srcPixels = bytes.data() + pixelOffset;
    for (int y = 0; y < height; ++y)
    {
        const int srcY = (height - 1) - y;
        std::memcpy(
            outImage->pixelsTopDown.data() + static_cast<size_t>(y) * static_cast<size_t>(width),
            srcPixels + static_cast<size_t>(srcY) * static_cast<size_t>(width),
            static_cast<size_t>(width));
    }

    const size_t paletteEntries = (headerOffset > 1u) ? ((headerOffset - 1u) / 3u) : 0u;
    std::array<uint32_t, 256> usage = {};
    for (uint8_t px : outImage->pixelsTopDown)
    {
        ++usage[px];
    }

    auto chooseMostUsed = [&usage](const std::vector<uint8_t>& indices, uint8_t* outIndex) -> bool
    {
        if (indices.empty() || outIndex == nullptr)
        {
            return false;
        }
        uint8_t best = indices[0];
        uint32_t bestUsage = usage[best];
        for (uint8_t candidate : indices)
        {
            if (usage[candidate] > bestUsage)
            {
                best = candidate;
                bestUsage = usage[candidate];
            }
        }
        *outIndex = best;
        return true;
    };

    std::vector<uint8_t> exactMagenta;
    std::vector<uint8_t> nearMagenta;
    exactMagenta.reserve(paletteEntries < 256u ? paletteEntries : 256u);
    nearMagenta.reserve(paletteEntries < 256u ? paletteEntries : 256u);

    for (size_t i = 0; i < paletteEntries && i < 256u; ++i)
    {
        const size_t base = 1u + i * 3u;
        if (base + 2u >= headerOffset)
        {
            break;
        }

        const uint8_t b = bytes[base];
        const uint8_t g = bytes[base + 1u];
        const uint8_t r = bytes[base + 2u];
        if (r == 255u && g == 0u && b == 255u)
        {
            exactMagenta.push_back(static_cast<uint8_t>(i));
        }
        else if (r >= 240u && b >= 240u && g <= 24u)
        {
            nearMagenta.push_back(static_cast<uint8_t>(i));
        }
    }

    uint8_t resolvedTransparent = 0;
    const char* transparentMethod = "none";
    if (chooseMostUsed(exactMagenta, &resolvedTransparent))
    {
        transparentMethod = "exact_magenta";
    }
    else if (chooseMostUsed(nearMagenta, &resolvedTransparent))
    {
        transparentMethod = "near_magenta";
    }
    else
    {
        uint8_t dominant = 0;
        uint32_t dominantUsage = usage[0];
        for (int i = 1; i < 256; ++i)
        {
            if (usage[static_cast<size_t>(i)] > dominantUsage)
            {
                dominant = static_cast<uint8_t>(i);
                dominantUsage = usage[static_cast<size_t>(i)];
            }
        }
        resolvedTransparent = dominant;
        transparentMethod = "dominant_index_fallback";
    }

    outImage->transparentIndex = resolvedTransparent;
    outImage->hasTransparentIndex = true;

    mod::Log(
        "ParseEfzDatImage: parsed '%s' size=%zu firstByte=%u headerOffset=%zu width=%d height=%d transparentSrc=%u hasTransparent=%d method=%s exact=%zu near=%zu",
        path.c_str(),
        bytes.size(),
        static_cast<unsigned>(bytes[0]),
        headerOffset,
        width,
        height,
        static_cast<unsigned>(outImage->transparentIndex),
        outImage->hasTransparentIndex ? 1 : 0,
        transparentMethod,
        exactMagenta.size(),
        nearMagenta.size());

    return true;
}

bool DeriveConfigStyleRowsFromDat(
    const ParsedDatImage& image,
    int optionCount,
    std::array<int, kNetplayConfigOptionCount>* topRows,
    std::array<int, kNetplayConfigOptionCount>* bottomRows,
    int* rowHeight)
{
    if (optionCount != kNetplayConfigOptionCount || topRows == nullptr || bottomRows == nullptr || rowHeight == nullptr)
    {
        return false;
    }

    const int width = image.width;
    const int height = image.height;
    const uint8_t transparent = image.transparentIndex;
    if (width <= 0 || height <= 0 || image.pixelsTopDown.size() != static_cast<size_t>(width) * static_cast<size_t>(height))
    {
        return false;
    }

    std::vector<uint8_t> activeRows(static_cast<size_t>(height), 0);
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row = image.pixelsTopDown.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
        int opaqueCount = 0;
        for (int x = 0; x < width; ++x)
        {
            if (row[x] != transparent)
            {
                ++opaqueCount;
            }
        }

        activeRows[static_cast<size_t>(y)] = (opaqueCount * 100 >= width * 90) ? 1u : 0u;
    }

    std::vector<RowRun> runs;
    for (int y = 0; y < height;)
    {
        if (activeRows[static_cast<size_t>(y)] == 0)
        {
            ++y;
            continue;
        }
        const int start = y;
        while (y < height && activeRows[static_cast<size_t>(y)] != 0)
        {
            ++y;
        }
        runs.push_back(RowRun{start, y - start});
    }

    std::vector<RowRun> topCandidates;
    std::vector<RowRun> bottomCandidates;
    const int half = height / 2;
    for (const RowRun& run : runs)
    {
        if (run.length < 8 || run.start < 40)
        {
            continue;
        }
        if (run.start < half)
        {
            topCandidates.push_back(run);
        }
        else if (run.start >= half + 20)
        {
            bottomCandidates.push_back(run);
        }
    }

    if (topCandidates.size() < static_cast<size_t>(optionCount)
        || bottomCandidates.size() < static_cast<size_t>(optionCount))
    {
        mod::Log(
            "DeriveConfigStyleRowsFromDat: insufficient runs (top=%zu bottom=%zu) for %d options",
            topCandidates.size(),
            bottomCandidates.size(),
            optionCount);
        return false;
    }

    int minHeight = kNetplayDefaultHighlightHeight;
    for (int i = 0; i < optionCount; ++i)
    {
        (*topRows)[static_cast<size_t>(i)] = topCandidates[static_cast<size_t>(i)].start;
        (*bottomRows)[static_cast<size_t>(i)] = bottomCandidates[static_cast<size_t>(i)].start;
        const int topLen = topCandidates[static_cast<size_t>(i)].length;
        const int bottomLen = bottomCandidates[static_cast<size_t>(i)].length;
        const int shorterLen = (topLen < bottomLen) ? topLen : bottomLen;
        minHeight = (minHeight < shorterLen) ? minHeight : shorterLen;
    }

    *rowHeight = (minHeight > 1) ? minHeight : 1;
    return true;
}

NetplayObjectProfile DetermineObjectProfile(const std::string& objectPath)
{
    NetplayObjectProfile profile;
    if (ContainsNoCase(objectPath, "config_ob.dat"))
    {
        profile.colorOffset = 161;
        profile.paletteDestStart = 162;
        profile.paletteCount = 32;
        profile.useConfigStyleRender = true;
        profile.renderLayout = {};
    }
    else if (ContainsNoCase(objectPath, "netplay_ob.dat"))
    {
        profile.colorOffset = 193;
        profile.paletteDestStart = 193;
        profile.paletteCount = 48;
        profile.useConfigStyleRender = true;
        profile.deriveLayoutFromDat = true;
        profile.renderLayout = {};
    }
    return profile;
}
}


