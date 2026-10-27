#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Menu translations, compiled in. The English text is the key: Tr("HOST
// SETTINGS") returns the active language's text, or its argument when the
// language is English or has no entry for it.
// Marks a literal as a translation key where Tr() cannot run (static tables
// built before a language is chosen). Translate it where it is displayed.
#define I18N_KEY(text) text

namespace netplay::i18n
{
const char* Tr(const char* english);
std::string Tr(const std::string& english);

// Selects the built-in table named by [Others] Language. Game thread.
void Reload();

// True when a language other than English is selected.
bool IsTranslationActive();

// Every translated string of the active language (font glyph ranges).
std::vector<const char*> ActiveValues();

struct Entry
{
    const char* english;
    const char* translated;   // UTF-8
};
struct LanguageTable
{
    const char* name;         // [Others] Language value
    const Entry* entries;
    std::size_t count;
};
const LanguageTable* BuiltInTables(std::size_t* count);

// A translated printf format must use the same conversions, in order.
bool FormatConversionsMatch(const char* english, const char* translated);
} // namespace netplay::i18n
