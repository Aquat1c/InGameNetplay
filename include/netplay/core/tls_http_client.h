#pragma once

#include <cstdint>
#include <string>

namespace netplay::tls
{
// Returns true when the embedded TLS backend was compiled in and is usable.
bool IsAvailable();

// Performs a blocking HTTPS GET. Returns true and fills outBody on success.
// Returns false and fills outError on failure.
bool HttpGet(
    const std::string& url,
    bool verifyPeer,
    uint32_t connectTimeoutMs,
    uint32_t receiveTimeoutMs,
    std::string* outBody,
    std::string* outError);
} // namespace netplay::tls
