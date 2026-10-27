#include "netplay/core/i18n.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace mod
{
void Log(const char*, ...) {}
}

namespace netplay::mod_settings
{
std::string g_language = "English";
const std::string& Language()
{
    return g_language;
}
}

namespace
{
int g_failures = 0;

void Expect(bool cond, const char* name)
{
    if (!cond)
    {
        std::printf("FAIL: %s\n", name);
        ++g_failures;
    }
}

bool IsValidUtf8(const char* s)
{
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    while (*p != 0)
    {
        int extra = 0;
        if (*p < 0x80) extra = 0;
        else if ((*p & 0xE0) == 0xC0) extra = 1;
        else if ((*p & 0xF0) == 0xE0) extra = 2;
        else if ((*p & 0xF8) == 0xF0) extra = 3;
        else return false;
        ++p;
        for (int i = 0; i < extra; ++i, ++p)
        {
            if ((*p & 0xC0) != 0x80) return false;
        }
    }
    return true;
}
} // namespace

int main()
{
    using netplay::i18n::FormatConversionsMatch;

    Expect(FormatConversionsMatch("Accept failed for '%s'.", "'%s' failed"), "same %s");
    Expect(FormatConversionsMatch("%d of %s", "%3d / %-10s"), "flags and width may differ");
    Expect(!FormatConversionsMatch("%d of %s", "%s of %d"), "reordered conversions rejected");
    Expect(!FormatConversionsMatch("%s", "no conversion"), "dropped conversion rejected");
    Expect(!FormatConversionsMatch("plain", "%s"), "added conversion rejected");
    Expect(FormatConversionsMatch("100%%", "100%%"), "%% is literal");
    Expect(!FormatConversionsMatch("%lu", "%u"), "length modifier must match");
    Expect(FormatConversionsMatch("%.*s", "%.*s"), "star precision");
    Expect(!FormatConversionsMatch("%.*s", "%s"), "star precision consumes an argument");
    Expect(FormatConversionsMatch("%I64u", "%I64u"), "MSVC I64");
    Expect(FormatConversionsMatch("trailing %", "trailing %"), "dangling percent");

    // Every built-in entry: format-safe, valid UTF-8, non-empty, unique key.
    std::size_t tableCount = 0;
    const auto* tables = netplay::i18n::BuiltInTables(&tableCount);
    for (std::size_t t = 0; t < tableCount; ++t)
    {
        std::set<std::string> keys;
        for (std::size_t i = 0; i < tables[t].count; ++i)
        {
            const auto& e = tables[t].entries[i];
            const bool ok = e.english != nullptr && e.translated != nullptr
                && e.english[0] != '\0' && e.translated[0] != '\0'
                && FormatConversionsMatch(e.english, e.translated)
                && IsValidUtf8(e.translated)
                && keys.insert(e.english).second;
            if (!ok)
            {
                std::printf("FAIL: %s entry %u '%s'\n", tables[t].name,
                            static_cast<unsigned>(i), e.english ? e.english : "(null)");
                ++g_failures;
            }
        }
    }

    // Language selection.
    const char* literal = "HOST";
    netplay::i18n::Reload();
    Expect(netplay::i18n::Tr(literal) == literal, "English: same pointer");
    netplay::mod_settings::g_language = "Japanese";
    netplay::i18n::Reload();
    Expect(std::strcmp(netplay::i18n::Tr(literal), literal) != 0, "Japanese: translated");
    Expect(netplay::i18n::Tr(std::string("no such key")) == "no such key", "missing key falls back");
    Expect(!netplay::i18n::ActiveValues().empty(), "active values listed");
    netplay::mod_settings::g_language = "English";
    netplay::i18n::Reload();
    Expect(netplay::i18n::Tr(literal) == literal, "back to English");

    if (g_failures == 0)
    {
        std::printf("i18n_tests: all passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
