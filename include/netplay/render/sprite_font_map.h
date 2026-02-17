#pragma once

#include <string>
#include <unordered_map>

namespace netplay::fontmap
{
struct SpriteGlyph
{
    int srcX = 0;
    int srcY = 0;
    int width = 0;
    int height = 0;
    int advance = 0;
};

struct SpriteFont
{
    bool loaded = false;
    bool uppercaseInput = true;
    int lineHeight = 14;
    int letterSpacing = 1;
    std::unordered_map<char, SpriteGlyph> glyphs;
};

bool LoadSpriteFontMapFromFile(const std::string& path, SpriteFont* outFont);
bool LoadNetplaySpriteFont(const std::string& moduleDirectory, SpriteFont* outFont);
}

