#include "netplay/core/validation.h"

#include <cctype>
#include <cstdlib>

namespace netplay::validation
{
bool ParsePort(const std::string& text, uint16_t* outPort)
{
    if (outPort == nullptr || text.empty() || text.size() > 5)
    {
        return false;
    }

    for (char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
    }

    const int parsed = std::atoi(text.c_str());
    if (parsed <= 0 || parsed > 65535)
    {
        return false;
    }

    *outPort = static_cast<uint16_t>(parsed);
    return true;
}

bool IsValidJoinAddress(const std::string& address)
{
    if (address.empty() || address.size() > 63)
    {
        return false;
    }

    for (char c : address)
    {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == ':' || c == '-' || c == '_';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

bool IsValidNickname(const std::string& nickname)
{
    if (nickname.empty() || nickname.size() > 63)
    {
        return false;
    }

    size_t codepoints = 0;
    size_t i = 0;
    while (i < nickname.size())
    {
        const unsigned char byte = static_cast<unsigned char>(nickname[i]);
        size_t seqLen = 0;
        if (byte < 0x80u)
        {
            // ASCII: reject control characters (below space) except allow printable
            if (byte < 32)
            {
                return false;
            }
            seqLen = 1;
        }
        else if ((byte & 0xE0u) == 0xC0u)
        {
            seqLen = 2;
        }
        else if ((byte & 0xF0u) == 0xE0u)
        {
            seqLen = 3;
        }
        else if ((byte & 0xF8u) == 0xF0u)
        {
            seqLen = 4;
        }
        else
        {
            return false; // invalid UTF-8 lead byte
        }

        if (i + seqLen > nickname.size())
        {
            return false; // truncated sequence
        }

        // Validate continuation bytes
        for (size_t j = 1; j < seqLen; ++j)
        {
            if ((static_cast<unsigned char>(nickname[i + j]) & 0xC0u) != 0x80u)
            {
                return false;
            }
        }

        i += seqLen;
        ++codepoints;
    }

    return codepoints > 0 && codepoints <= 20;
}
}



