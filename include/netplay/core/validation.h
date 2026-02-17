#pragma once

#include <cstdint>
#include <string>

namespace netplay::validation
{
bool ParsePort(const std::string& text, uint16_t* outPort);
bool IsValidJoinAddress(const std::string& address);
bool IsValidNickname(const std::string& nickname);
}

