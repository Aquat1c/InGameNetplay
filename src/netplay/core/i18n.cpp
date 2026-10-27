#include "netplay/core/i18n.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <string_view>
#include <unordered_map>

#include "logger.h"
#include "netplay/core/mod_settings.h"

namespace netplay::i18n
{
extern const Entry kJapanese[];
extern const std::size_t kJapaneseCount;

namespace
{
const LanguageTable kTables[] = {
    {"Japanese", kJapanese, kJapaneseCount},
};
constexpr std::size_t kTableCount = sizeof(kTables) / sizeof(kTables[0]);

struct ActiveTable
{
    std::unordered_map<std::string_view, const char*> map;
    std::vector<const char*> values;
};

// One lookup map per language, built on first use and kept for the process
// lifetime, so pointers Tr() returned stay valid when the language changes.
ActiveTable* g_built[kTableCount] = {};
std::atomic<const ActiveTable*> g_active{nullptr};

ActiveTable* Build(const LanguageTable& table)
{
    auto* active = new ActiveTable();
    active->map.reserve(table.count);
    for (std::size_t i = 0; i < table.count; ++i)
    {
        const Entry& e = table.entries[i];
        if (!FormatConversionsMatch(e.english, e.translated))
        {
            mod::Log("I18n: %s entry skipped, printf conversions differ: '%s'",
                     table.name, e.english);
            continue;
        }
        if (active->map.emplace(e.english, e.translated).second)
        {
            active->values.push_back(e.translated);
        }
    }
    return active;
}

// Signature of the printf conversions in `s` ("s;d;" ...): length modifiers and
// conversion characters in order. Flags, width and precision may differ.
std::string Conversions(const char* s)
{
    std::string out;
    while (s != nullptr && *s != '\0')
    {
        if (*s++ != '%')
        {
            continue;
        }
        if (*s == '%')
        {
            ++s;
            continue;
        }
        while (*s != '\0' && std::strchr("-+ #0", *s) != nullptr)
        {
            ++s;
        }
        if (*s == '*')
        {
            out += "*;";
            ++s;
        }
        while (std::isdigit(static_cast<unsigned char>(*s)))
        {
            ++s;
        }
        if (*s == '.')
        {
            ++s;
            if (*s == '*')
            {
                out += "*;";
                ++s;
            }
            while (std::isdigit(static_cast<unsigned char>(*s)))
            {
                ++s;
            }
        }
        while (*s != '\0' && std::strchr("hljztLIw", *s) != nullptr)
        {
            const bool msvcSized = (*s == 'I');
            out += *s++;
            while (msvcSized && std::isdigit(static_cast<unsigned char>(*s)))
            {
                out += *s++;
            }
        }
        if (*s == '\0')
        {
            break;
        }
        out += *s++;
        out += ';';
    }
    return out;
}
} // namespace

bool FormatConversionsMatch(const char* english, const char* translated)
{
    return Conversions(english) == Conversions(translated);
}

const LanguageTable* BuiltInTables(std::size_t* count)
{
    if (count != nullptr)
    {
        *count = kTableCount;
    }
    return kTables;
}

const char* Tr(const char* english)
{
    const ActiveTable* table = g_active.load(std::memory_order_acquire);
    if (table == nullptr || english == nullptr)
    {
        return english;
    }
    const auto it = table->map.find(std::string_view(english));
    return it == table->map.end() ? english : it->second;
}

std::string Tr(const std::string& english)
{
    const char* translated = Tr(english.c_str());
    return translated == english.c_str() ? english : std::string(translated);
}

bool IsTranslationActive()
{
    return g_active.load(std::memory_order_acquire) != nullptr;
}

std::vector<const char*> ActiveValues()
{
    const ActiveTable* table = g_active.load(std::memory_order_acquire);
    return table != nullptr ? table->values : std::vector<const char*>();
}

void Reload()
{
    const std::string& language = netplay::mod_settings::Language();
    const ActiveTable* selected = nullptr;
    for (std::size_t i = 0; i < kTableCount; ++i)
    {
        if (_stricmp(language.c_str(), kTables[i].name) == 0)
        {
            if (g_built[i] == nullptr)
            {
                g_built[i] = Build(kTables[i]);
            }
            selected = g_built[i];
            break;
        }
    }
    if (selected != g_active.load(std::memory_order_acquire))
    {
        mod::Log("I18n: menu language %s", selected != nullptr ? language.c_str() : "English");
    }
    g_active.store(selected, std::memory_order_release);
}
} // namespace netplay::i18n
