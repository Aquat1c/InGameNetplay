#include "netplay/render/sprite_font_map.h"

#include "logger.h"
#include "netplay/assets/assets.h"
#include "netplay/core/text_utils.h"

#include <array>
#include <fstream>

namespace netplay::fontmap
{
bool LoadSpriteFontMapFromFile(const std::string& path, SpriteFont* outFont)
{
    if (outFont == nullptr)
    {
        return false;
    }

    std::ifstream file(path);
    if (!file)
    {
        return false;
    }

    SpriteFont parsed = {};
    std::string line;
    int lineNo = 0;
    while (std::getline(file, line))
    {
        ++lineNo;
        line = netplay::text::TrimAscii(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        const size_t eqPos = line.find('=');
        if (eqPos == std::string::npos || eqPos == 0 || eqPos + 1 >= line.size())
        {
            continue;
        }

        std::string key = netplay::text::TrimAscii(line.substr(0, eqPos));
        std::string value = netplay::text::TrimAscii(line.substr(eqPos + 1));
        if (key.empty() || value.empty())
        {
            continue;
        }

        if (key == "line_height")
        {
            int v = 0;
            if (netplay::text::ParseIntToken(value, &v) && v > 0)
            {
                parsed.lineHeight = v;
            }
            continue;
        }
        if (key == "letter_spacing")
        {
            int v = 0;
            if (netplay::text::ParseIntToken(value, &v) && v >= 0)
            {
                parsed.letterSpacing = v;
            }
            continue;
        }
        if (key == "uppercase_input")
        {
            parsed.uppercaseInput = !(value == "0" || value == "false" || value == "False");
            continue;
        }

        if (key.size() != 1)
        {
            continue;
        }

        std::array<int, 5> numbers = {};
        int numberCount = 0;
        size_t cursor = 0;
        while (cursor < value.size() && numberCount < static_cast<int>(numbers.size()))
        {
            size_t comma = value.find(',', cursor);
            std::string token = (comma == std::string::npos) ? value.substr(cursor) : value.substr(cursor, comma - cursor);
            token = netplay::text::TrimAscii(token);
            int parsedValue = 0;
            if (!netplay::text::ParseIntToken(token, &parsedValue))
            {
                numberCount = 0;
                break;
            }
            numbers[static_cast<size_t>(numberCount)] = parsedValue;
            ++numberCount;
            if (comma == std::string::npos)
            {
                break;
            }
            cursor = comma + 1;
        }

        if (numberCount < 4)
        {
            mod::Log("LoadSpriteFontMap: ignored malformed glyph at line %d", lineNo);
            continue;
        }

        SpriteGlyph glyph = {};
        glyph.srcX = numbers[0];
        glyph.srcY = numbers[1];
        glyph.width = numbers[2];
        glyph.height = numbers[3];
        glyph.advance = (numberCount >= 5) ? numbers[4] : numbers[2];
        if (glyph.width <= 0 || glyph.height <= 0 || glyph.advance <= 0)
        {
            continue;
        }
        parsed.glyphs[key[0]] = glyph;
    }

    parsed.loaded = !parsed.glyphs.empty();
    if (!parsed.loaded)
    {
        return false;
    }

    *outFont = parsed;
    return true;
}

bool LoadNetplaySpriteFont(const std::string& moduleDirectory, SpriteFont* outFont)
{
    if (outFont == nullptr)
    {
        return false;
    }

    *outFont = {};

    const std::array<const char*, 3> candidates = {
        "assets\\netplay_font_map.txt",
        "assets\\font_map.txt",
        "netplay_font_map.txt",
    };

    // Tier 1: DLL directory (GetModuleFileNameA-derived).
    for (const char* candidate : candidates)
    {
        const std::string path = netplay::assets::JoinPath(moduleDirectory, candidate);
        if (!netplay::assets::FileExists(path))
        {
            continue;
        }

        if (LoadSpriteFontMapFromFile(path, outFont))
        {
            mod::Log(
                "LoadNetplaySpriteFont: loaded '%s' glyphs=%zu lineHeight=%d spacing=%d uppercase=%d",
                path.c_str(),
                outFont->glyphs.size(),
                outFont->lineHeight,
                outFont->letterSpacing,
                outFont->uppercaseInput ? 1 : 0);
            return true;
        }
        mod::Log("LoadNetplaySpriteFont: failed to parse '%s'", path.c_str());
    }

    // Tier 2: mods\<modname>\ relative to working directory (Wine fallback).
    const std::string modsRelDir = netplay::assets::DeriveModsRelativeDirectory(moduleDirectory);
    if (!modsRelDir.empty())
    {
        for (const char* candidate : candidates)
        {
            const std::string path = netplay::assets::JoinPath(modsRelDir, candidate);
            if (!netplay::assets::FileExists(path))
            {
                continue;
            }

            if (LoadSpriteFontMapFromFile(path, outFont))
            {
                mod::Log(
                    "LoadNetplaySpriteFont: loaded '%s' (mods-dir fallback) glyphs=%zu lineHeight=%d spacing=%d uppercase=%d",
                    path.c_str(),
                    outFont->glyphs.size(),
                    outFont->lineHeight,
                    outFont->letterSpacing,
                    outFont->uppercaseInput ? 1 : 0);
                return true;
            }
            mod::Log("LoadNetplaySpriteFont: failed to parse '%s'", path.c_str());
        }
    }

    // Tier 3: working-directory loose files.
    for (const char* candidate : candidates)
    {
        if (!netplay::assets::FileExists(candidate))
        {
            continue;
        }

        if (LoadSpriteFontMapFromFile(candidate, outFont))
        {
            mod::Log(
                "LoadNetplaySpriteFont: loaded '%s' (cwd fallback) glyphs=%zu lineHeight=%d spacing=%d uppercase=%d",
                candidate,
                outFont->glyphs.size(),
                outFont->lineHeight,
                outFont->letterSpacing,
                outFont->uppercaseInput ? 1 : 0);
            return true;
        }
        mod::Log("LoadNetplaySpriteFont: failed to parse '%s'", candidate);
    }

    mod::Log("LoadNetplaySpriteFont: no sprite font map found (runtime text disabled)");
    return false;
}
}


