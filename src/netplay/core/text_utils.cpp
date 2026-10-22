#include "netplay/core/text_utils.h"

#include <cctype>
#include <cstdlib>

namespace netplay::text
{
std::string TrimAscii(std::string value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
    {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool ParseIntToken(const std::string& token, int* outValue)
{
    if (token.empty() || outValue == nullptr)
    {
        return false;
    }

    char* endPtr = nullptr;
    const long v = std::strtol(token.c_str(), &endPtr, 10);
    if (endPtr == token.c_str() || (endPtr != nullptr && *endPtr != '\0'))
    {
        return false;
    }

    *outValue = static_cast<int>(v);
    return true;
}
}


