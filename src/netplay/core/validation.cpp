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
    if (nickname.empty() || nickname.size() > 20)
    {
        return false;
    }

    for (char c : nickname)
    {
        if (c < 32 || c > 126)
        {
            return false;
        }
    }
    return true;
}
}



