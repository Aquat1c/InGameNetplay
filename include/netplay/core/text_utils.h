#pragma once

#include <string>

namespace netplay::text
{
std::string TrimAscii(std::string value);
bool ParseIntToken(const std::string& token, int* outValue);
}

