#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace netplay::connection_check
{
enum class Result
{
    None,
    Direct,
    Relay,
    Failed,
};

void Start(uint16_t hostPort);
void Cancel();
Result GetResult(uint16_t hostPort);

inline std::string ResolveBaseUrl(const std::string& configured, const char* fallback)
{
    const size_t first = configured.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return fallback;
    }
    std::string url = configured.substr(first, configured.find_last_not_of(" \t\r\n") - first + 1);
    while (!url.empty() && url.back() == '/')
    {
        url.pop_back();
    }
    if (url.compare(0, 8, "https://") != 0 || url.size() == 8)
    {
        return {};
    }
    return url;
}

inline bool JsonBool(const std::string& json, const char* key, bool* out)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
    {
        return false;
    }
    pos = json.find_first_not_of(" \t\r\n", pos + needle.size());
    if (pos == std::string::npos || json[pos] != ':')
    {
        return false;
    }
    pos = json.find_first_not_of(" \t\r\n", pos + 1);
    if (pos == std::string::npos)
    {
        return false;
    }
    if (json.compare(pos, 4, "true") == 0)
    {
        *out = true;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0)
    {
        *out = false;
        return true;
    }
    return false;
}

inline bool ParseRunResult(const std::string& body, bool* outDirect, bool* outRelay)
{
    return JsonBool(body, "direct", outDirect) && JsonBool(body, "relay", outRelay);
}
}
