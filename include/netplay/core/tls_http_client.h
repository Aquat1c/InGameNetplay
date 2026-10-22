#pragma once

#include <cstdint>
#include <string>

namespace netplay::tls
{
bool IsAvailable();

bool HttpGet(
    const std::string& url,
    bool verifyPeer,
    uint32_t connectTimeoutMs,
    uint32_t receiveTimeoutMs,
    std::string* outBody,
    std::string* outError);
} // namespace netplay::tls
