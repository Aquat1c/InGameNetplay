#pragma once

#include <string>

namespace netplay::version
{
struct ParsedVersion
{
    int parts[4] = {0, 0, 0, 0};
    int partCount = 0;
    // Text after the dotted numbers ("_beta1", "-rc2", "b3"); empty for a final release.
    std::string suffix;
};

// Accepts an optional 'v'/'V', 1-4 dotted numeric components, and an optional pre-release suffix.
bool Parse(const std::string& text, ParsedVersion* out);

// <0 when a<b, 0 when equal, >0 when a>b. Final releases outrank pre-releases;
// pre-release tags order alphabetically then by trailing number; unparsable ranks lowest.
int Compare(const std::string& a, const std::string& b);

bool IsNewer(const std::string& candidate, const std::string& current);
} // namespace netplay::version
