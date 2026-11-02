#pragma once

#include <string>

namespace netplay::version
{
struct ParsedVersion
{
    int parts[4] = {0, 0, 0, 0};
    int partCount = 0;
    std::string suffix;
};

bool Parse(const std::string& text, ParsedVersion* out);

// <0 when a<b, 0 when equal, >0 when a>b.
int Compare(const std::string& a, const std::string& b);

bool IsNewer(const std::string& candidate, const std::string& current);
}
