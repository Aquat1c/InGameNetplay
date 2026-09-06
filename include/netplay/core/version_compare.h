#pragma once

#include <string>

// Mod version-string ordering ("0.4.2", "0.4.2_beta1", "v0.5.0"). Used by the
// GitHub update check to decide whether a release tag is newer than the build.
namespace netplay::version
{
struct ParsedVersion
{
    int parts[4] = {0, 0, 0, 0};
    int partCount = 0;
    // Everything after the dotted numbers, e.g. "_beta1" / "-rc2" / "b3".
    // Empty for a final release.
    std::string suffix;
};

// Accepts an optional leading 'v'/'V', 1-4 dotted numeric components and an
// optional pre-release suffix. Returns false when there is no leading number.
bool Parse(const std::string& text, ParsedVersion* out);

// <0 when a<b, 0 when equal, >0 when a>b. Numeric components compare first
// (missing = 0); for an equal tuple a final release outranks any pre-release,
// and two pre-releases order by their alphabetic tag ("alpha" < "beta" < "rc")
// then by their trailing number ("beta1" < "beta2"). Unparsable strings rank
// below everything parsable (and equal to each other).
int Compare(const std::string& a, const std::string& b);

// True when |candidate| parses and is strictly newer than |current|.
bool IsNewer(const std::string& candidate, const std::string& current);
} // namespace netplay::version
