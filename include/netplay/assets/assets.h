#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

#include "netplay/core/constants.h"

namespace netplay::assets
{
struct NetplayObjectProfile
{
    unsigned char colorOffset = 193;
    int paletteDestStart = 193;
    int paletteCount = 48;
    bool useConfigStyleRender = false;
    bool deriveLayoutFromDat = false;
    netplay::constants::NetplayRenderLayout renderLayout = {};
};

struct ParsedDatImage
{
    int width = 0;
    int height = 0;
    uint8_t transparentIndex = 0;
    bool hasTransparentIndex = false;
    std::vector<uint8_t> pixelsTopDown;
};

bool FileExists(const std::string& path);
std::string JoinPath(const std::string& left, const char* right);
std::string BuildModuleDirectory(HMODULE moduleHandle);
std::string DeriveModsRelativeDirectory(const std::string& moduleDirectory);
std::string ResolveNetplayBackgroundPath(const std::string& moduleDirectory);
std::string ResolveNetplayObjectsPath(const std::string& moduleDirectory);
std::string ResolveTitleObjectsPath(const std::string& moduleDirectory);

bool ParseEfzDatImage(const std::string& path, ParsedDatImage* outImage);
bool DeriveConfigStyleRowsFromDat(
    const ParsedDatImage& image,
    int optionCount,
    std::array<int, netplay::constants::kNetplayConfigOptionCount>* topRows,
    std::array<int, netplay::constants::kNetplayConfigOptionCount>* bottomRows,
    int* rowHeight);

NetplayObjectProfile DetermineObjectProfile(const std::string& objectPath);
}


