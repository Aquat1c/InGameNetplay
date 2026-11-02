#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Compiled-in translations; the English text is the key.
#define I18N_KEY(text) text

namespace netplay::i18n
{
const char* Tr(const char* english);
std::string Tr(const std::string& english);

void Reload();

bool IsTranslationActive();

std::vector<const char*> ActiveValues();

struct Entry
{
    const char* english;
    const char* translated;
};
struct LanguageTable
{
    const char* name;
    const Entry* entries;
    std::size_t count;
};
const LanguageTable* BuiltInTables(std::size_t* count);

// A translated printf format must use the same conversions, in order.
bool FormatConversionsMatch(const char* english, const char* translated);
}
