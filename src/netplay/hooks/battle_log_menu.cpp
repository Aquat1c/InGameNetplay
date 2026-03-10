#include "netplay/core/battle_log_menu.h"

#include "logger.h"
#include "netplay/core/constants.h"
#include "netplay/core/input_utils.h"
#include "netplay/core/text_utils.h"
#include "netplay/hooks/internal/shared.h"
#include "netplay/render/draw_surface.h"
#include "netplay/render/software_font.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string_view>
#include <vector>

namespace netplay::battle_log
{
namespace
{
using NetplayMenuAction = netplay::menu::NetplayMenuAction;
using NetplayMenuEntry = netplay::menu::NetplayMenuEntry;
using NetplayMenuId = netplay::menu::NetplayMenuId;
using NetplayMenuSpec = netplay::menu::NetplayMenuSpec;
namespace hooks = netplay::hooks::internal;

constexpr int kPanelX = 8;
constexpr int kPanelY = 16;
constexpr int kPanelW = 304;
constexpr int kPanelH = 194;
constexpr int kRowX = 12;
constexpr int kRowW = 296;
constexpr int kSummaryRowH = 14;
constexpr int kBrowserRowH = 12;
constexpr int kFilterRowH = 14;
constexpr int kDetailRowH = 12;
constexpr DWORD kStatusDisplayMs = 2200;
constexpr DWORD kCaretBlinkMs = 350;
constexpr size_t kMaxFilterTextBytes = 127;

constexpr std::array<int, 5> kSummaryRowY = {124, 140, 156, 172, 188};
constexpr std::array<int, 10> kBrowserRowY = {74, 87, 100, 113, 126, 139, 157, 170, 183, 196};
constexpr std::array<int, 7> kFiltersRowY = {98, 114, 130, 146, 164, 180, 196};
constexpr std::array<int, 8> kDetailRowY = {80, 93, 106, 119, 132, 160, 176, 192};

enum class FileEncoding : uint8_t
{
    Utf16Le = 0,
    Utf8Bom,
    Utf8,
};

enum class View : uint8_t
{
    Summary = 0,
    Browser,
    Filters,
    SetDetail,
};

enum class FilterEditField : uint8_t
{
    None = 0,
    PlayerName,
    OpponentName,
};

struct FilterEditState
{
    bool active = false;
    FilterEditField field = FilterEditField::None;
    std::string buffer;
    size_t caretByteOffset = 0;
    std::array<uint8_t, 256> keyDown = {};
    bool caretVisible = true;
    DWORD lastCaretTick = 0;
};

struct State
{
    BattleLogDocument document = {};
    BattleLogSummary summary = {};
    BattleLogFilter activeFilter = {};
    BattleLogFilter draftFilter = {};
    View view = View::Summary;
    View filterReturnView = View::Summary;
    int summarySelection = 0;
    int browserSelection = 0;
    int filtersSelection = 0;
    int detailSelection = 0;
    int browserPage = 0;
    int detailPage = 0;
    int detailSessionIndex = -1;
    std::vector<int> filteredSessionIndices;
    std::string currentNickname = "Player";
    std::string statusMessage;
    DWORD statusExpireTick = 0;
    FilterEditState edit = {};
    std::array<NetplayMenuEntry, 10> entries = {};
    NetplayMenuSpec spec = {};
};

State g_state = {};

// Parsing and document helpers.
std::wstring TrimWide(std::wstring_view value);
std::string WideToUtf8(const std::wstring& wide);
std::wstring Utf8ToWide(const std::string& utf8);
bool DecodeUtf8(const std::vector<uint8_t>& bytes, size_t offset, std::wstring* outWide);
bool DecodeAnsi(const std::vector<uint8_t>& bytes, size_t offset, std::wstring* outWide);
bool ReadWideTextFile(const std::string& path, std::wstring* outText, FileEncoding* outEncoding);
std::vector<std::wstring> SplitLines(const std::wstring& text);
bool IsAbsolutePath(const std::string& path);
std::string GetExecutableDirectory();
std::string ResolveRevivalIniPath();
std::string ResolveBattleLogPathFromIni(bool* outSaveEnabled);
bool ParseLeadingDateTime(std::string_view line, std::string* outDate, std::string* outTime, size_t* outTailOffset);
bool SplitVsPair(std::string_view text, std::string* outLeft, std::string* outRight);
bool ParseTwoInts(std::string_view text, int* outLeft, int* outRight);
bool ParseDurationField(std::string_view text, int* outTotalSeconds);
bool LooksLikeMatchRow(std::string_view line);
std::string FormatDateTime(const std::string& date, const std::string& time);
std::string FormatDurationShort(int totalSeconds);
std::string FormatResultPair(int left, int right);
int64_t ParseTimestampKey(const std::string& date, const std::string& time);
std::string NormalizeCharacterToken(std::string token);
std::string AbbreviateForDisplay(std::string text, size_t maxChars);
std::string AbbreviateForChip(std::string text);
int GetSessionFinalScoreLeft(const BattleLogSession& session);
int GetSessionFinalScoreRight(const BattleLogSession& session);
size_t CountDocumentMatches(const BattleLogDocument& document);
bool ParseHeaderLine(const std::string& line, int sessionIndex, int lineNumber, BattleLogSession* outSession);
bool ParseMatchLine(const std::string& line, int matchIndex, int lineNumber, BattleLogMatch* outMatch);
BattleLogDocument ParseBattleLogDocument(const std::string& path, bool saveEnabled);
bool IsAllCharacterValue(const std::string& value);
bool StringEqualsTrimmed(const std::string& left, const std::string& right);
bool MatchOrientationNames(const BattleLogSession& session, const BattleLogFilter& filter, int playerSide);
bool MatchOrientationCharacters(const BattleLogMatch& match, const BattleLogFilter& filter, int playerSide);
bool SessionMatchesFilter(const BattleLogSession& session, const BattleLogFilter& filter);
BattleLogSummary BuildSummaryForNickname(const BattleLogDocument& document, const std::string& nickname);

// State and navigation helpers.
void SetStatusMessage(const char* text);
bool HasStatusMessage();
void ClearStatusMessage();
int* GetSelectionStorage(View view);
const char* GetHeaderLabel();
int GetCurrentContentCount();
int ClampSelectionForCurrentView(int selection);
void SetSelection(uint32_t screenContext, int selection);
int GetBrowserResultCount();
int GetBrowserPageCount();
int GetDetailPageCount();
void NormalizeFilter(BattleLogFilter* filter);
bool HasCharacterOption(const std::string& value);
void SanitizeFilterCharacters(BattleLogFilter* filter);
void RebuildFilteredSessionIndices();
void RefreshParsedDocument();
int GetSessionIndexForVisibleSlot(int slot);
const BattleLogSession* GetSelectedSessionForBrowserSelection(int selection);
const BattleLogSession* GetDetailSession();
const BattleLogMatch* GetMatchForVisibleDetailSlot(int slot);
void EnsureSpecInitialized();
void RebuildMenuEntries();
void SwitchView(uint32_t screenContext, View nextView, int selection = -1);
std::string BuildFilterSummary(const BattleLogFilter& filter);
std::string BuildBrowserPageText();
std::string BuildSummaryRecordText();
std::string BuildSummaryUsageText();
std::string BuildSummaryRecentText();
std::string BuildLoadStatusText();
int FindCharacterOptionIndex(const std::string& value);
void CycleCharacterOption(std::string* value, int delta);

// Inline edit helpers for filter names.
std::string GetFilterFieldValue(FilterEditField field, bool includeCaret);
void PrimeEditKeys();
void BeginEdit(FilterEditField field);
void CancelEdit();
void UpdateCaretBlink();
bool ConsumeEditKeyEdge(int virtualKey);
void EraseLastUtf8Codepoint(std::string& text);
size_t ClampCaretOffset(const std::string& text, size_t offset);
size_t PrevUtf8Boundary(const std::string& text, size_t offset);
size_t NextUtf8Boundary(const std::string& text, size_t offset);
bool IsAllowedFilterChar(char c);
void TouchEditCaret();
void InsertEditBytes(const std::string& bytes);
bool EraseCodepointBeforeCaret();
bool EraseCodepointAtCaret();
void TryAppendUtf8Text(const std::string& utf8);
std::string WcharToUtf8(wchar_t ch);
bool DrainWmCharMessages();
bool CommitEdit();
bool HandleEditInput(uint32_t screenContext, const uint8_t* inputBytes);

// Drawing helpers.
void DrawPanelBox(const netplay::font::IndexedSurfaceView& surface, int x, int y, int w, int h, uint8_t fillColor, uint8_t frameColor);
void DrawRowBox(const netplay::font::IndexedSurfaceView& surface, int y, int h, bool selected, uint8_t fillColor, uint8_t frameColor, uint8_t selectedFillColor);
int DrawChip(const netplay::font::IndexedSurfaceView& surface, const std::string& label, int x, int y, uint8_t fillColor, uint8_t frameColor, uint8_t textColor, bool switched);
void DrawSummaryPanel(const netplay::font::IndexedSurfaceView& surface, uint8_t panelFill, uint8_t panelFrame, uint8_t titleColor, uint8_t textColor, uint8_t dimColor, uint8_t warnColor);
void DrawBrowserPanel(const netplay::font::IndexedSurfaceView& surface, uint8_t panelFill, uint8_t panelFrame, uint8_t titleColor, uint8_t textColor, uint8_t dimColor, uint8_t warnColor);
void DrawFiltersPanel(const netplay::font::IndexedSurfaceView& surface, uint8_t panelFill, uint8_t panelFrame, uint8_t titleColor, uint8_t textColor, uint8_t dimColor);
void DrawDetailPanel(const netplay::font::IndexedSurfaceView& surface, uint8_t panelFill, uint8_t panelFrame, uint8_t titleColor, uint8_t textColor, uint8_t dimColor, uint8_t warnColor);
void DrawScreenTitleBar(const netplay::font::IndexedSurfaceView& surface, uint8_t fillColor, uint8_t frameColor, uint8_t titleColor, uint8_t dimColor);
void DrawSummaryRows(const netplay::font::IndexedSurfaceView& surface, int selection, uint8_t rowFill, uint8_t rowFrame, uint8_t selectedFill, uint8_t normalText, uint8_t selectedText);
void DrawBrowserRows(const netplay::font::IndexedSurfaceView& surface, int selection, uint8_t rowFill, uint8_t rowFrame, uint8_t selectedFill, uint8_t normalText, uint8_t selectedText, uint8_t chipFill, uint8_t chipFrame, uint8_t chipText, uint8_t dimText, uint8_t warnColor);
void DrawFilterRows(const netplay::font::IndexedSurfaceView& surface, int selection, uint8_t rowFill, uint8_t rowFrame, uint8_t selectedFill, uint8_t normalText, uint8_t selectedText, uint8_t dimText);
void DrawDetailRows(const netplay::font::IndexedSurfaceView& surface, int selection, uint8_t rowFill, uint8_t rowFrame, uint8_t selectedFill, uint8_t normalText, uint8_t selectedText, uint8_t chipFill, uint8_t chipFrame, uint8_t chipText, uint8_t dimText);

std::wstring TrimWide(std::wstring_view value)
{
    size_t begin = 0;
    while (begin < value.size() && iswspace(value[begin]) != 0)
    {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && iswspace(value[end - 1]) != 0)
    {
        --end;
    }

    return std::wstring(value.substr(begin, end - begin));
}

std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return {};
    }

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
    {
        return {};
    }

    std::string utf8(static_cast<size_t>(bytes), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr);
    return utf8;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
    {
        return {};
    }

    const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (chars <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<size_t>(chars), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), chars);
    return wide;
}

bool DecodeUtf8(const std::vector<uint8_t>& bytes, size_t offset, std::wstring* outWide)
{
    if (outWide == nullptr)
    {
        return false;
    }

    const char* data = reinterpret_cast<const char*>(bytes.data() + offset);
    const int size = static_cast<int>(bytes.size() - offset);
    const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, nullptr, 0);
    if (chars <= 0)
    {
        return false;
    }

    outWide->assign(static_cast<size_t>(chars), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, outWide->data(), chars);
    return true;
}

bool DecodeAnsi(const std::vector<uint8_t>& bytes, size_t offset, std::wstring* outWide)
{
    if (outWide == nullptr)
    {
        return false;
    }

    const char* data = reinterpret_cast<const char*>(bytes.data() + offset);
    const int size = static_cast<int>(bytes.size() - offset);
    const int chars = MultiByteToWideChar(CP_ACP, 0, data, size, nullptr, 0);
    if (chars <= 0)
    {
        return false;
    }

    outWide->assign(static_cast<size_t>(chars), L'\0');
    (void)MultiByteToWideChar(CP_ACP, 0, data, size, outWide->data(), chars);
    return true;
}

bool ReadWideTextFile(const std::string& path, std::wstring* outText, FileEncoding* outEncoding)
{
    if (outText == nullptr || outEncoding == nullptr)
    {
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }

    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 2 && bytes[0] == 0xFFu && bytes[1] == 0xFEu)
    {
        *outEncoding = FileEncoding::Utf16Le;
        const size_t codeUnits = (bytes.size() - 2) / 2;
        outText->assign(codeUnits, L'\0');
        if (codeUnits > 0)
        {
            std::memcpy(outText->data(), bytes.data() + 2, codeUnits * sizeof(wchar_t));
        }
        return true;
    }

    if (bytes.size() >= 3 && bytes[0] == 0xEFu && bytes[1] == 0xBBu && bytes[2] == 0xBFu)
    {
        *outEncoding = FileEncoding::Utf8Bom;
        return DecodeUtf8(bytes, 3, outText);
    }

    *outEncoding = FileEncoding::Utf8;
    if (DecodeUtf8(bytes, 0, outText))
    {
        return true;
    }
    return DecodeAnsi(bytes, 0, outText);
}

std::vector<std::wstring> SplitLines(const std::wstring& text)
{
    std::vector<std::wstring> lines;
    std::wstring current;
    for (size_t i = 0; i < text.size(); ++i)
    {
        const wchar_t ch = text[i];
        if (ch == L'\r')
        {
            if (i + 1 < text.size() && text[i + 1] == L'\n')
            {
                ++i;
            }
            lines.push_back(current);
            current.clear();
            continue;
        }
        if (ch == L'\n')
        {
            lines.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(ch);
    }
    lines.push_back(current);
    return lines;
}

bool IsAbsolutePath(const std::string& path)
{
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) != 0 && path[1] == ':')
    {
        return true;
    }
    return !path.empty() && (path[0] == '\\' || path[0] == '/');
}

std::string GetExecutableDirectory()
{
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, static_cast<DWORD>(std::size(exePath))) == 0)
    {
        return ".";
    }

    std::string path = exePath;
    const size_t sep = path.find_last_of("\\/");
    if (sep == std::string::npos)
    {
        return ".";
    }
    path.resize(sep);
    return path;
}

std::string ResolveRevivalIniPath()
{
    const std::string exeDir = GetExecutableDirectory();
    const std::string candidate = exeDir + "\\EfzRevival.ini";
    const DWORD attrs = GetFileAttributesA(candidate.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return candidate;
    }

    const DWORD localAttrs = GetFileAttributesA("EfzRevival.ini");
    if (localAttrs != INVALID_FILE_ATTRIBUTES && (localAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return "EfzRevival.ini";
    }

    return candidate;
}

std::string ResolveBattleLogPathFromIni(bool* outSaveEnabled)
{
    if (outSaveEnabled != nullptr)
    {
        *outSaveEnabled = true;
    }

    std::string logPath = "BattleLog.txt";
    std::wstring text;
    FileEncoding encoding = FileEncoding::Utf8;
    if (ReadWideTextFile(ResolveRevivalIniPath(), &text, &encoding))
    {
        std::string currentSection;
        for (const std::wstring& rawLine : SplitLines(text))
        {
            const std::wstring trimmed = TrimWide(rawLine);
            if (trimmed.empty() || trimmed.front() == L';')
            {
                continue;
            }
            if (trimmed.front() == L'[' && trimmed.back() == L']' && trimmed.size() >= 2)
            {
                currentSection = WideToUtf8(TrimWide(std::wstring_view(trimmed).substr(1, trimmed.size() - 2)));
                continue;
            }

            const size_t eq = trimmed.find(L'=');
            if (eq == std::wstring::npos)
            {
                continue;
            }

            const std::string key = WideToUtf8(TrimWide(std::wstring_view(trimmed).substr(0, eq)));
            const std::string value = WideToUtf8(TrimWide(std::wstring_view(trimmed).substr(eq + 1)));
            if (currentSection == "Network" && key == "BattleLogFile" && !value.empty())
            {
                logPath = value;
            }
            else if (currentSection == "Network" && key == "SaveBattleLog" && outSaveEnabled != nullptr)
            {
                *outSaveEnabled = (value != "0");
            }
        }
    }

    if (IsAbsolutePath(logPath))
    {
        return logPath;
    }
    return GetExecutableDirectory() + "\\" + logPath;
}

bool ParseLeadingDateTime(std::string_view line, std::string* outDate, std::string* outTime, size_t* outTailOffset)
{
    if (line.size() < 19 || outDate == nullptr || outTime == nullptr || outTailOffset == nullptr)
    {
        return false;
    }

    const auto isDigitAt = [&](size_t index) -> bool
    {
        return index < line.size()
            && std::isdigit(static_cast<unsigned char>(line[index])) != 0;
    };

    for (size_t index : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u})
    {
        if (!isDigitAt(index))
        {
            return false;
        }
    }

    if (line[4] != '/' || line[7] != '/' || line[10] != ' '
        || line[13] != ':' || line[16] != ':')
    {
        return false;
    }

    *outDate = std::string(line.substr(0, 10));
    *outTime = std::string(line.substr(11, 8));

    size_t tailOffset = 19;
    while (tailOffset < line.size()
        && std::isspace(static_cast<unsigned char>(line[tailOffset])) != 0)
    {
        ++tailOffset;
    }

    *outTailOffset = tailOffset;
    return tailOffset <= line.size();
}

bool SplitVsPair(std::string_view text, std::string* outLeft, std::string* outRight)
{
    if (outLeft == nullptr || outRight == nullptr)
    {
        return false;
    }

    const size_t marker = text.find(" VS ");
    if (marker == std::string_view::npos)
    {
        return false;
    }

    const std::string left = netplay::text::TrimAscii(std::string(text.substr(0, marker)));
    const std::string right = netplay::text::TrimAscii(std::string(text.substr(marker + 4)));
    if (left.empty() || right.empty())
    {
        return false;
    }

    *outLeft = left;
    *outRight = right;
    return true;
}

bool ParseTwoInts(std::string_view text, int* outLeft, int* outRight)
{
    if (outLeft == nullptr || outRight == nullptr)
    {
        return false;
    }

    std::string normalized;
    normalized.reserve(text.size());
    for (char ch : text)
    {
        normalized.push_back(
            std::isspace(static_cast<unsigned char>(ch)) != 0 ? ' ' : ch);
    }

    const std::string trimmed = netplay::text::TrimAscii(std::move(normalized));
    if (trimmed.empty())
    {
        return false;
    }

    const size_t separator = trimmed.find(' ');
    if (separator == std::string::npos)
    {
        return false;
    }

    const std::string leftToken = netplay::text::TrimAscii(trimmed.substr(0, separator));
    const std::string rightToken = netplay::text::TrimAscii(trimmed.substr(separator + 1));
    return netplay::text::ParseIntToken(leftToken, outLeft)
        && netplay::text::ParseIntToken(rightToken, outRight);
}

bool ParseDurationField(std::string_view text, int* outTotalSeconds)
{
    if (outTotalSeconds == nullptr)
    {
        return false;
    }

    const std::string trimmed = netplay::text::TrimAscii(std::string(text));
    if (trimmed.empty())
    {
        return false;
    }

    const size_t firstColon = trimmed.find(':');
    if (firstColon == std::string::npos)
    {
        return false;
    }

    const size_t secondColon = trimmed.find(':', firstColon + 1);
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    if (secondColon == std::string::npos)
    {
        if (!netplay::text::ParseIntToken(trimmed.substr(0, firstColon), &minutes)
            || !netplay::text::ParseIntToken(trimmed.substr(firstColon + 1), &seconds))
        {
            return false;
        }
    }
    else
    {
        if (!netplay::text::ParseIntToken(trimmed.substr(0, firstColon), &hours)
            || !netplay::text::ParseIntToken(trimmed.substr(firstColon + 1, secondColon - firstColon - 1), &minutes)
            || !netplay::text::ParseIntToken(trimmed.substr(secondColon + 1), &seconds))
        {
            return false;
        }
    }

    if (minutes < 0 || seconds < 0 || seconds >= 60)
    {
        return false;
    }

    *outTotalSeconds = hours * 3600 + minutes * 60 + seconds;
    return true;
}

bool LooksLikeMatchRow(std::string_view line)
{
    return line.find("Rounds:") != std::string_view::npos
        && line.find("Duration:") != std::string_view::npos
        && line.find("Score:") != std::string_view::npos;
}

std::string FormatDateTime(const std::string& date, const std::string& time)
{
    if (date.empty())
    {
        return time;
    }
    if (time.empty())
    {
        return date;
    }
    return date + " " + time;
}

std::string FormatDurationShort(int totalSeconds)
{
    if (totalSeconds <= 0)
    {
        return "0:00";
    }

    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds / 60) % 60;
    const int seconds = totalSeconds % 60;
    char buffer[32] = {};
    if (hours > 0)
    {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours, minutes, seconds);
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, seconds);
    }
    return buffer;
}

std::string FormatResultPair(int left, int right)
{
    char buffer[24] = {};
    std::snprintf(buffer, sizeof(buffer), "%d-%d", left, right);
    return buffer;
}

int64_t ParseTimestampKey(const std::string& date, const std::string& time)
{
    char digits[15] = {};
    size_t out = 0;
    for (char ch : date)
    {
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0 && out < sizeof(digits) - 1)
        {
            digits[out++] = ch;
        }
    }
    for (char ch : time)
    {
        if (std::isdigit(static_cast<unsigned char>(ch)) != 0 && out < sizeof(digits) - 1)
        {
            digits[out++] = ch;
        }
    }
    digits[out] = '\0';
    return std::strtoll(digits, nullptr, 10);
}

std::string NormalizeCharacterToken(std::string token)
{
    token = netplay::text::TrimAscii(std::move(token));
    if (token == "NayukiA")
    {
        return "Nayuki A";
    }
    if (token == "NayukiS")
    {
        return "Nayuki S";
    }
    return token;
}

std::string AbbreviateForDisplay(std::string text, size_t maxChars)
{
    if (text.size() <= maxChars)
    {
        return text;
    }
    if (maxChars == 0)
    {
        return {};
    }
    if (maxChars == 1)
    {
        return ".";
    }
    text.resize(maxChars - 1);
    text.push_back('.');
    return text;
}

std::string AbbreviateForChip(std::string text)
{
    return AbbreviateForDisplay(std::move(text), 8);
}

int GetSessionFinalScoreLeft(const BattleLogSession& session)
{
    return session.matches.empty() ? 0 : session.matches.back().p1Score;
}

int GetSessionFinalScoreRight(const BattleLogSession& session)
{
    return session.matches.empty() ? 0 : session.matches.back().p2Score;
}

size_t CountDocumentMatches(const BattleLogDocument& document)
{
    size_t count = 0;
    for (const BattleLogSession& session : document.sessions)
    {
        count += session.matches.size();
    }
    return count;
}

bool ParseHeaderLine(const std::string& line, int sessionIndex, int lineNumber, BattleLogSession* outSession)
{
    if (outSession == nullptr)
    {
        return false;
    }

    if (LooksLikeMatchRow(line))
    {
        return false;
    }

    std::string date;
    std::string time;
    size_t tailOffset = 0;
    if (!ParseLeadingDateTime(line, &date, &time, &tailOffset))
    {
        return false;
    }

    std::string p1Name;
    std::string p2Name;
    if (!SplitVsPair(std::string_view(line).substr(tailOffset), &p1Name, &p2Name))
    {
        return false;
    }

    outSession->sessionIndex = sessionIndex;
    outSession->lineNumber = lineNumber;
    outSession->date = std::move(date);
    outSession->time = std::move(time);
    outSession->p1Name = std::move(p1Name);
    outSession->p2Name = std::move(p2Name);
    outSession->sortKey = ParseTimestampKey(outSession->date, outSession->time);
    return true;
}

bool ParseMatchLine(const std::string& line, int matchIndex, int lineNumber, BattleLogMatch* outMatch)
{
    if (outMatch == nullptr)
    {
        return false;
    }

    if (!LooksLikeMatchRow(line))
    {
        return false;
    }

    std::string date;
    std::string time;
    size_t tailOffset = 0;
    if (!ParseLeadingDateTime(line, &date, &time, &tailOffset))
    {
        return false;
    }

    const std::string_view tail = std::string_view(line).substr(tailOffset);
    const size_t roundsPos = tail.find("Rounds:");
    const size_t durationPos = tail.find("Duration:", roundsPos == std::string_view::npos ? 0 : roundsPos);
    const size_t scorePos = tail.find("Score:", durationPos == std::string_view::npos ? 0 : durationPos);
    if (roundsPos == std::string_view::npos
        || durationPos == std::string_view::npos
        || scorePos == std::string_view::npos
        || roundsPos >= durationPos
        || durationPos >= scorePos)
    {
        return false;
    }

    std::string p1Character;
    std::string p2Character;
    if (!SplitVsPair(tail.substr(0, roundsPos), &p1Character, &p2Character))
    {
        return false;
    }

    int p1Rounds = 0;
    int p2Rounds = 0;
    if (!ParseTwoInts(
            tail.substr(roundsPos + std::strlen("Rounds:"), durationPos - roundsPos - std::strlen("Rounds:")),
            &p1Rounds,
            &p2Rounds))
    {
        return false;
    }

    int durationSeconds = 0;
    if (!ParseDurationField(
            tail.substr(durationPos + std::strlen("Duration:"), scorePos - durationPos - std::strlen("Duration:")),
            &durationSeconds))
    {
        return false;
    }

    int p1Score = 0;
    int p2Score = 0;
    if (!ParseTwoInts(tail.substr(scorePos + std::strlen("Score:")), &p1Score, &p2Score))
    {
        return false;
    }

    outMatch->lineNumber = lineNumber;
    outMatch->matchIndex = matchIndex;
    outMatch->date = std::move(date);
    outMatch->time = std::move(time);
    outMatch->p1CharacterRaw = std::move(p1Character);
    outMatch->p2CharacterRaw = std::move(p2Character);
    outMatch->p1CharacterDisplay = NormalizeCharacterToken(outMatch->p1CharacterRaw);
    outMatch->p2CharacterDisplay = NormalizeCharacterToken(outMatch->p2CharacterRaw);
    outMatch->p1Rounds = p1Rounds;
    outMatch->p2Rounds = p2Rounds;
    outMatch->durationSeconds = durationSeconds;
    outMatch->p1Score = p1Score;
    outMatch->p2Score = p2Score;
    return true;
}

BattleLogDocument ParseBattleLogDocument(const std::string& path, bool saveEnabled)
{
    BattleLogDocument document = {};
    document.sourcePath = path;
    document.saveBattleLogEnabled = saveEnabled;
    document.characterOptions.push_back("All");

    const DWORD attrs = GetFileAttributesA(path.c_str());
    document.fileExists = (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
    if (!document.fileExists)
    {
        document.sourceEncoding = "missing";
        document.warnings.push_back({0, "Battle log file was not found."});
        return document;
    }

    std::wstring text;
    FileEncoding encoding = FileEncoding::Utf8;
    if (!ReadWideTextFile(path, &text, &encoding))
    {
        document.sourceEncoding = "unreadable";
        document.warnings.push_back({0, "Battle log file could not be read."});
        return document;
    }

    document.sourceEncoding =
        encoding == FileEncoding::Utf16Le ? "utf16le" :
        encoding == FileEncoding::Utf8Bom ? "utf8-bom" :
        "utf8";

    std::set<std::string> characters;
    BattleLogSession* currentSession = nullptr;
    int sessionIndex = 0;
    int matchIndex = 0;
    int lineNumber = 0;
    for (const std::wstring& rawLine : SplitLines(text))
    {
        ++lineNumber;
        std::string line = netplay::text::TrimAscii(WideToUtf8(rawLine));
        if (line.empty())
        {
            currentSession = nullptr;
            continue;
        }

        const bool looksLikeMatchRow = LooksLikeMatchRow(line);
        if (looksLikeMatchRow)
        {
            BattleLogMatch parsedMatch;
            if (ParseMatchLine(line, matchIndex, lineNumber, &parsedMatch))
            {
                if (currentSession == nullptr)
                {
                    document.warnings.push_back({lineNumber, "Match row was found before any session header."});
                    continue;
                }

                currentSession->totalDurationSeconds += parsedMatch.durationSeconds;
                currentSession->matches.push_back(parsedMatch);
                characters.insert(parsedMatch.p1CharacterDisplay);
                characters.insert(parsedMatch.p2CharacterDisplay);
                ++matchIndex;
                continue;
            }

            document.warnings.push_back({lineNumber, "Malformed match row was skipped."});
            if (currentSession != nullptr)
            {
                ++currentSession->warningCount;
            }
            continue;
        }

        BattleLogSession session;
        if (ParseHeaderLine(line, sessionIndex, lineNumber, &session))
        {
            document.sessions.push_back(std::move(session));
            currentSession = &document.sessions.back();
            ++sessionIndex;
            continue;
        }

        document.warnings.push_back({lineNumber, "Unrecognized battle log line was skipped."});
        if (currentSession != nullptr)
        {
            ++currentSession->warningCount;
        }
    }

    for (BattleLogSession& session : document.sessions)
    {
        if (session.matches.empty())
        {
            continue;
        }

        session.finalP1Character = session.matches.back().p1CharacterDisplay;
        session.finalP2Character = session.matches.back().p2CharacterDisplay;
        std::set<std::string> p1Chars;
        std::set<std::string> p2Chars;
        for (const BattleLogMatch& match : session.matches)
        {
            p1Chars.insert(match.p1CharacterDisplay);
            p2Chars.insert(match.p2CharacterDisplay);
        }
        session.p1SwitchedCharacter = p1Chars.size() > 1;
        session.p2SwitchedCharacter = p2Chars.size() > 1;
    }

    for (const std::string& character : characters)
    {
        if (!character.empty())
        {
            document.characterOptions.push_back(character);
        }
    }
    return document;
}

bool IsAllCharacterValue(const std::string& value)
{
    return value.empty() || value == "All";
}

bool StringEqualsTrimmed(const std::string& left, const std::string& right)
{
    return netplay::text::TrimAscii(left) == netplay::text::TrimAscii(right);
}

bool MatchOrientationNames(const BattleLogSession& session, const BattleLogFilter& filter, int playerSide)
{
    const std::string& playerName = playerSide == 0 ? session.p1Name : session.p2Name;
    const std::string& opponentName = playerSide == 0 ? session.p2Name : session.p1Name;
    if (!filter.playerName.empty() && !StringEqualsTrimmed(playerName, filter.playerName))
    {
        return false;
    }
    if (!filter.opponentName.empty() && !StringEqualsTrimmed(opponentName, filter.opponentName))
    {
        return false;
    }
    return true;
}

bool MatchOrientationCharacters(const BattleLogMatch& match, const BattleLogFilter& filter, int playerSide)
{
    const std::string& playerCharacter = playerSide == 0 ? match.p1CharacterDisplay : match.p2CharacterDisplay;
    const std::string& opponentCharacter = playerSide == 0 ? match.p2CharacterDisplay : match.p1CharacterDisplay;
    if (!IsAllCharacterValue(filter.playerCharacter) && playerCharacter != filter.playerCharacter)
    {
        return false;
    }
    if (!IsAllCharacterValue(filter.opponentCharacter) && opponentCharacter != filter.opponentCharacter)
    {
        return false;
    }
    return true;
}

bool SessionMatchesFilter(const BattleLogSession& session, const BattleLogFilter& filter)
{
    const bool anyNameFilter = !filter.playerName.empty() || !filter.opponentName.empty();
    const bool anyCharacterFilter =
        !IsAllCharacterValue(filter.playerCharacter) || !IsAllCharacterValue(filter.opponentCharacter);
    if (!anyNameFilter && !anyCharacterFilter)
    {
        return true;
    }

    for (int playerSide = 0; playerSide < 2; ++playerSide)
    {
        if (!MatchOrientationNames(session, filter, playerSide))
        {
            continue;
        }
        if (!anyCharacterFilter)
        {
            return true;
        }
        for (const BattleLogMatch& match : session.matches)
        {
            if (MatchOrientationCharacters(match, filter, playerSide))
            {
                return true;
            }
        }
    }

    return false;
}

BattleLogSummary BuildSummaryForNickname(const BattleLogDocument& document, const std::string& nickname)
{
    BattleLogSummary summary = {};
    summary.nickname = nickname;

    int64_t latestSortKey = 0;
    std::map<std::string, int> characterCounts;
    for (const BattleLogSession& session : document.sessions)
    {
        const bool nicknameIsP1 = StringEqualsTrimmed(session.p1Name, nickname);
        const bool nicknameIsP2 = StringEqualsTrimmed(session.p2Name, nickname);
        if (!nicknameIsP1 && !nicknameIsP2)
        {
            continue;
        }

        summary.hasSessions = true;
        ++summary.matchingSessions;
        if (session.sortKey >= latestSortKey)
        {
            latestSortKey = session.sortKey;
            summary.recentOpponent = nicknameIsP1 ? session.p2Name : session.p1Name;
            summary.recentTimestamp = FormatDateTime(session.date, session.time);
        }

        if (session.matches.empty())
        {
            continue;
        }

        const int playerSetScore = nicknameIsP1 ? GetSessionFinalScoreLeft(session) : GetSessionFinalScoreRight(session);
        const int opponentSetScore = nicknameIsP1 ? GetSessionFinalScoreRight(session) : GetSessionFinalScoreLeft(session);
        if (playerSetScore > opponentSetScore)
        {
            ++summary.setWins;
        }
        else if (playerSetScore < opponentSetScore)
        {
            ++summary.setLosses;
        }

        for (const BattleLogMatch& match : session.matches)
        {
            summary.totalDurationSeconds += match.durationSeconds;
            const int playerRounds = nicknameIsP1 ? match.p1Rounds : match.p2Rounds;
            const int opponentRounds = nicknameIsP1 ? match.p2Rounds : match.p1Rounds;
            if (playerRounds > opponentRounds)
            {
                ++summary.gameWins;
            }
            else if (playerRounds < opponentRounds)
            {
                ++summary.gameLosses;
            }

            const std::string& character = nicknameIsP1 ? match.p1CharacterDisplay : match.p2CharacterDisplay;
            if (!character.empty())
            {
                ++characterCounts[character];
            }
        }
    }

    int bestCount = 0;
    for (const auto& [character, count] : characterCounts)
    {
        if (count > bestCount)
        {
            bestCount = count;
            summary.mostUsedCharacter = character;
        }
    }

    return summary;
}
std::string Basename(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos)
    {
        return path;
    }
    return path.substr(slash + 1);
}

std::string TrimUtf8(const std::string& text)
{
    return WideToUtf8(TrimWide(Utf8ToWide(text)));
}

const BattleLogSession* GetSessionByIndex(int sessionIndex)
{
    if (sessionIndex < 0
        || sessionIndex >= static_cast<int>(g_state.document.sessions.size()))
    {
        return nullptr;
    }
    return &g_state.document.sessions[static_cast<size_t>(sessionIndex)];
}

const BattleLogMatch* GetMatchByIndex(const BattleLogSession* session, int matchIndex)
{
    if (session == nullptr
        || matchIndex < 0
        || matchIndex >= static_cast<int>(session->matches.size()))
    {
        return nullptr;
    }
    return &session->matches[static_cast<size_t>(matchIndex)];
}

void SetStatusMessage(const char* text)
{
    g_state.statusMessage = text != nullptr ? text : "";
    g_state.statusExpireTick = g_state.statusMessage.empty()
        ? 0
        : (GetTickCount() + kStatusDisplayMs);
}

bool HasStatusMessage()
{
    return !g_state.statusMessage.empty()
        && GetTickCount() < g_state.statusExpireTick;
}

void ClearStatusMessage()
{
    g_state.statusMessage.clear();
    g_state.statusExpireTick = 0;
}

int* GetSelectionStorage(View view)
{
    switch (view)
    {
    case View::Summary:
        return &g_state.summarySelection;
    case View::Browser:
        return &g_state.browserSelection;
    case View::Filters:
        return &g_state.filtersSelection;
    case View::SetDetail:
        return &g_state.detailSelection;
    default:
        return &g_state.summarySelection;
    }
}

const char* GetHeaderLabel()
{
    switch (g_state.view)
    {
    case View::Summary:
        return "BATTLE LOG";
    case View::Browser:
        return "SET BROWSER";
    case View::Filters:
        return "SEARCH / FILTER";
    case View::SetDetail:
        return "SET DETAIL";
    default:
        return "BATTLE LOG";
    }
}

int GetCurrentContentCount()
{
    switch (g_state.view)
    {
    case View::Summary:
        return 5;
    case View::Browser:
        return 10;
    case View::Filters:
        return 7;
    case View::SetDetail:
        return 8;
    default:
        return 0;
    }
}

int ClampSelectionForCurrentView(int selection)
{
    const int count = GetCurrentContentCount();
    if (count <= 0)
    {
        return 0;
    }
    if (selection < 0)
    {
        return 0;
    }
    if (selection >= count)
    {
        return count - 1;
    }
    return selection;
}

void SetSelection(uint32_t screenContext, int selection)
{
    const int clamped = ClampSelectionForCurrentView(selection);
    *GetSelectionStorage(g_state.view) = clamped;

    if (screenContext == 0)
    {
        return;
    }

    *reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection) =
        static_cast<int8_t>(clamped);
    *reinterpret_cast<uint16_t*>(screenContext + netplay::constants::kOffsetMenuAnimCounter) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + netplay::constants::kOffsetInputLatchP1) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + netplay::constants::kOffsetInputLatchP2) = 0;
    *reinterpret_cast<uint32_t*>(screenContext + netplay::constants::kOffsetInactivityCounter) = 0;
    hooks::g_lastLoggedSelection = static_cast<int8_t>(clamped);
}

int GetBrowserResultCount()
{
    return static_cast<int>(g_state.filteredSessionIndices.size());
}

int GetBrowserPageCount()
{
    const int results = GetBrowserResultCount();
    if (results <= 0)
    {
        return 1;
    }
    return (results + netplay::menu::kBattleLogVisibleSessionRows - 1)
        / netplay::menu::kBattleLogVisibleSessionRows;
}

int GetDetailPageCount()
{
    const BattleLogSession* session = GetDetailSession();
    if (session == nullptr || session->matches.empty())
    {
        return 1;
    }
    return (static_cast<int>(session->matches.size()) + netplay::menu::kBattleLogVisibleGameRows - 1)
        / netplay::menu::kBattleLogVisibleGameRows;
}

void NormalizeFilter(BattleLogFilter* filter)
{
    if (filter == nullptr)
    {
        return;
    }

    filter->playerName = TrimUtf8(filter->playerName);
    filter->opponentName = TrimUtf8(filter->opponentName);
    filter->playerCharacter = TrimUtf8(filter->playerCharacter);
    filter->opponentCharacter = TrimUtf8(filter->opponentCharacter);

    if (filter->playerCharacter.empty())
    {
        filter->playerCharacter = "All";
    }
    if (filter->opponentCharacter.empty())
    {
        filter->opponentCharacter = "All";
    }
}

bool HasCharacterOption(const std::string& value)
{
    if (value.empty())
    {
        return false;
    }

    for (const std::string& option : g_state.document.characterOptions)
    {
        if (option == value)
        {
            return true;
        }
    }
    return false;
}

void SanitizeFilterCharacters(BattleLogFilter* filter)
{
    if (filter == nullptr)
    {
        return;
    }
    if (!HasCharacterOption(filter->playerCharacter))
    {
        filter->playerCharacter = "All";
    }
    if (!HasCharacterOption(filter->opponentCharacter))
    {
        filter->opponentCharacter = "All";
    }
}

void RebuildFilteredSessionIndices()
{
    NormalizeFilter(&g_state.activeFilter);
    SanitizeFilterCharacters(&g_state.activeFilter);

    g_state.filteredSessionIndices.clear();
    for (const BattleLogSession& session : g_state.document.sessions)
    {
        if (SessionMatchesFilter(session, g_state.activeFilter))
        {
            g_state.filteredSessionIndices.push_back(session.sessionIndex);
        }
    }

    std::stable_sort(
        g_state.filteredSessionIndices.begin(),
        g_state.filteredSessionIndices.end(),
        [](int leftIndex, int rightIndex) -> bool
        {
            const BattleLogSession* left = GetSessionByIndex(leftIndex);
            const BattleLogSession* right = GetSessionByIndex(rightIndex);
            if (left == nullptr || right == nullptr)
            {
                return leftIndex > rightIndex;
            }
            if (left->sortKey != right->sortKey)
            {
                return left->sortKey > right->sortKey;
            }
            return false;
        });

    const int browserPages = GetBrowserPageCount();
    if (g_state.browserPage >= browserPages)
    {
        g_state.browserPage = browserPages - 1;
    }
    if (g_state.browserPage < 0)
    {
        g_state.browserPage = 0;
    }

    const int detailPages = GetDetailPageCount();
    if (g_state.detailPage >= detailPages)
    {
        g_state.detailPage = detailPages - 1;
    }
    if (g_state.detailPage < 0)
    {
        g_state.detailPage = 0;
    }

    g_state.summary = BuildSummaryForNickname(g_state.document, g_state.currentNickname);
}

void RefreshParsedDocument()
{
    bool saveEnabled = true;
    const std::string battleLogPath = ResolveBattleLogPathFromIni(&saveEnabled);
    g_state.document = ParseBattleLogDocument(battleLogPath, saveEnabled);
    NormalizeFilter(&g_state.activeFilter);
    NormalizeFilter(&g_state.draftFilter);
    SanitizeFilterCharacters(&g_state.activeFilter);
    SanitizeFilterCharacters(&g_state.draftFilter);
    RebuildFilteredSessionIndices();

    if (!g_state.document.saveBattleLogEnabled)
    {
        SetStatusMessage("SaveBattleLog is disabled in EfzRevival.ini.");
    }
    else if (!g_state.document.fileExists)
    {
        SetStatusMessage("Battle log file was not found.");
    }
    else if (!g_state.document.warnings.empty())
    {
        char buffer[64] = {};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "Parsed %zu warning(s) from the battle log.",
            g_state.document.warnings.size());
        SetStatusMessage(buffer);
    }
    else
    {
        char buffer[64] = {};
        const size_t matchCount = CountDocumentMatches(g_state.document);
        std::snprintf(
            buffer,
            sizeof(buffer),
            "Loaded %zu set(s), %zu game(s) from %s.",
            g_state.document.sessions.size(),
            matchCount,
            Basename(g_state.document.sourcePath).c_str());
        SetStatusMessage(buffer);
    }
}

int GetSessionIndexForVisibleSlot(int slot)
{
    if (slot < 0 || slot >= netplay::menu::kBattleLogVisibleSessionRows)
    {
        return -1;
    }

    const int absoluteIndex =
        g_state.browserPage * netplay::menu::kBattleLogVisibleSessionRows + slot;
    if (absoluteIndex < 0 || absoluteIndex >= GetBrowserResultCount())
    {
        return -1;
    }

    return g_state.filteredSessionIndices[static_cast<size_t>(absoluteIndex)];
}

const BattleLogSession* GetSelectedSessionForBrowserSelection(int selection)
{
    if (selection < 0 || selection >= netplay::menu::kBattleLogVisibleSessionRows)
    {
        return nullptr;
    }
    return GetSessionByIndex(GetSessionIndexForVisibleSlot(selection));
}

const BattleLogSession* GetDetailSession()
{
    return GetSessionByIndex(g_state.detailSessionIndex);
}

const BattleLogMatch* GetMatchForVisibleDetailSlot(int slot)
{
    if (slot < 0 || slot >= netplay::menu::kBattleLogVisibleGameRows)
    {
        return nullptr;
    }

    const BattleLogSession* session = GetDetailSession();
    if (session == nullptr)
    {
        return nullptr;
    }

    const int matchIndex =
        g_state.detailPage * netplay::menu::kBattleLogVisibleGameRows + slot;
    return GetMatchByIndex(session, matchIndex);
}

void EnsureSpecInitialized()
{
    if (g_state.spec.entries != nullptr)
    {
        return;
    }

    const int blankRow = netplay::menu::RowToIndex(netplay::menu::NetplayObRow::Blank);
    g_state.entries[0] = {NetplayMenuAction::BattleLogBack, blankRow, "BATTLELOG_BACK"};
    g_state.spec.menuId = NetplayMenuId::BattleLog;
    g_state.spec.headerLabel = GetHeaderLabel();
    g_state.spec.entries = g_state.entries.data();
    g_state.spec.entryCount = 1;
    g_state.spec.defaultSelection = 0;
}

void RebuildMenuEntries()
{
    EnsureSpecInitialized();

    int entryIndex = 0;
    const int blankRow = netplay::menu::RowToIndex(netplay::menu::NetplayObRow::Blank);
    switch (g_state.view)
    {
    case View::Summary:
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogBrowseMine, blankRow, "BATTLELOG_BROWSE_MINE"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogSearchFilters, blankRow, "BATTLELOG_FILTERS"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogBrowseAll, blankRow, "BATTLELOG_BROWSE_ALL"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogRefresh, blankRow, "BATTLELOG_REFRESH"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogBack, blankRow, "BATTLELOG_BACK"};
        break;

    case View::Browser:
        for (int slot = 0; slot < netplay::menu::kBattleLogVisibleSessionRows; ++slot)
        {
            g_state.entries[entryIndex++] = {
                netplay::menu::BattleLogSessionAction(slot),
                blankRow,
                "BATTLELOG_SESSION",
            };
        }
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogBrowserPrevPage, blankRow, "BATTLELOG_PREV"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogBrowserNextPage, blankRow, "BATTLELOG_NEXT"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogBrowserFilters, blankRow, "BATTLELOG_BROWSER_FILTERS"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogBack, blankRow, "BATTLELOG_BACK"};
        break;

    case View::Filters:
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogEditPlayerName, blankRow, "BATTLELOG_PLAYER_NAME"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogEditOpponentName, blankRow, "BATTLELOG_OPPONENT_NAME"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogPlayerCharacter, blankRow, "BATTLELOG_PLAYER_CHAR"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogOpponentCharacter, blankRow, "BATTLELOG_OPPONENT_CHAR"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogApplyFilters, blankRow, "BATTLELOG_APPLY"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogResetFilters, blankRow, "BATTLELOG_RESET"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogBack, blankRow, "BATTLELOG_BACK"};
        break;

    case View::SetDetail:
        for (int slot = 0; slot < netplay::menu::kBattleLogVisibleGameRows; ++slot)
        {
            g_state.entries[entryIndex++] = {
                netplay::menu::BattleLogGameAction(slot),
                blankRow,
                "BATTLELOG_GAME",
            };
        }
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogDetailPrevPage, blankRow, "BATTLELOG_DETAIL_PREV"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogDetailNextPage, blankRow, "BATTLELOG_DETAIL_NEXT"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogBack, blankRow, "BATTLELOG_BACK"};
        break;
    }

    g_state.spec.menuId = NetplayMenuId::BattleLog;
    g_state.spec.headerLabel = GetHeaderLabel();
    g_state.spec.entries = g_state.entries.data();
    g_state.spec.entryCount = entryIndex;
    g_state.spec.defaultSelection = ClampSelectionForCurrentView(*GetSelectionStorage(g_state.view));
    hooks::g_netplayMenuState.optionCount = entryIndex;
    hooks::g_netplayMenuState.backIndex = entryIndex > 0 ? (entryIndex - 1) : 0;
}

void SwitchView(uint32_t screenContext, View nextView, int selection)
{
    if (g_state.edit.active)
    {
        CancelEdit();
    }

    g_state.view = nextView;
    RebuildMenuEntries();

    int nextSelection = selection;
    if (nextSelection < 0)
    {
        nextSelection = *GetSelectionStorage(nextView);
    }
    SetSelection(screenContext, nextSelection);
}

std::string BuildFilterSummary(const BattleLogFilter& filter)
{
    std::vector<std::string> parts;
    if (!filter.playerName.empty())
    {
        parts.push_back("Player=" + filter.playerName);
    }
    if (!filter.opponentName.empty())
    {
        parts.push_back("Opponent=" + filter.opponentName);
    }
    if (!IsAllCharacterValue(filter.playerCharacter))
    {
        parts.push_back("PChar=" + filter.playerCharacter);
    }
    if (!IsAllCharacterValue(filter.opponentCharacter))
    {
        parts.push_back("OChar=" + filter.opponentCharacter);
    }

    if (parts.empty())
    {
        return "All sets";
    }

    std::string joined;
    for (size_t index = 0; index < parts.size(); ++index)
    {
        if (index != 0)
        {
            joined += ", ";
        }
        joined += parts[index];
    }
    return joined;
}

std::string BuildBrowserPageText()
{
    char buffer[80] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "Page %d/%d  Results %d",
        g_state.browserPage + 1,
        GetBrowserPageCount(),
        GetBrowserResultCount());
    return buffer;
}

std::string BuildSummaryRecordText()
{
    char buffer[96] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "Sets %d-%d   Games %d-%d",
        g_state.summary.setWins,
        g_state.summary.setLosses,
        g_state.summary.gameWins,
        g_state.summary.gameLosses);
    return buffer;
}

std::string BuildSummaryUsageText()
{
    const std::string character = g_state.summary.mostUsedCharacter.empty()
        ? "N/A"
        : g_state.summary.mostUsedCharacter;
    return "Play time " + FormatDurationShort(g_state.summary.totalDurationSeconds)
        + "   Most played: " + character;
}

std::string BuildSummaryRecentText()
{
    if (!g_state.summary.hasSessions)
    {
        return "No sessions found for '" + g_state.currentNickname + "'.";
    }

    return "Last vs "
        + (g_state.summary.recentOpponent.empty() ? "?" : g_state.summary.recentOpponent)
        + " at "
        + (g_state.summary.recentTimestamp.empty() ? "unknown time" : g_state.summary.recentTimestamp);
}

std::string BuildLoadStatusText()
{
    const size_t matchCount = CountDocumentMatches(g_state.document);
    if (!g_state.document.saveBattleLogEnabled)
    {
        return "Battle log saving is disabled.";
    }
    if (!g_state.document.fileExists)
    {
        return "Missing: " + Basename(g_state.document.sourcePath);
    }

    std::string text =
        "Loaded " + std::to_string(g_state.document.sessions.size()) + " set(s), "
        + std::to_string(matchCount) + " game(s)";
    if (!g_state.document.warnings.empty())
    {
        text += "  Warnings " + std::to_string(g_state.document.warnings.size());
    }
    return text;
}

int FindCharacterOptionIndex(const std::string& value)
{
    if (g_state.document.characterOptions.empty())
    {
        return 0;
    }

    const std::string needle = value.empty() ? "All" : value;
    for (size_t index = 0; index < g_state.document.characterOptions.size(); ++index)
    {
        if (g_state.document.characterOptions[index] == needle)
        {
            return static_cast<int>(index);
        }
    }
    return 0;
}

void CycleCharacterOption(std::string* value, int delta)
{
    if (value == nullptr)
    {
        return;
    }
    if (g_state.document.characterOptions.empty())
    {
        *value = "All";
        return;
    }

    int index = FindCharacterOptionIndex(*value);
    const int count = static_cast<int>(g_state.document.characterOptions.size());
    index = (index + delta + count) % count;
    *value = g_state.document.characterOptions[static_cast<size_t>(index)];
}
std::string GetFilterFieldValue(FilterEditField field, bool includeCaret)
{
    const bool editingThisField =
        g_state.edit.active && g_state.edit.field == field;
    std::string value;
    switch (field)
    {
    case FilterEditField::PlayerName:
        value = editingThisField ? g_state.edit.buffer : g_state.draftFilter.playerName;
        break;
    case FilterEditField::OpponentName:
        value = editingThisField ? g_state.edit.buffer : g_state.draftFilter.opponentName;
        break;
    default:
        break;
    }

    if (editingThisField && includeCaret && g_state.edit.caretVisible)
    {
        const size_t caret = ClampCaretOffset(value, g_state.edit.caretByteOffset);
        value.insert(caret, 1, '_');
    }
    return value;
}

void PrimeEditKeys()
{
    g_state.edit.keyDown.fill(0);
    for (int virtualKey = 0; virtualKey < 256; ++virtualKey)
    {
        g_state.edit.keyDown[static_cast<size_t>(virtualKey)] =
            (GetAsyncKeyState(virtualKey) & 0x8000) != 0 ? 1u : 0u;
    }
}

void BeginEdit(FilterEditField field)
{
    g_state.edit.active = true;
    g_state.edit.field = field;
    switch (field)
    {
    case FilterEditField::PlayerName:
        g_state.edit.buffer = g_state.draftFilter.playerName;
        break;
    case FilterEditField::OpponentName:
        g_state.edit.buffer = g_state.draftFilter.opponentName;
        break;
    default:
        g_state.edit.buffer.clear();
        break;
    }
    g_state.edit.caretByteOffset = g_state.edit.buffer.size();
    g_state.edit.caretVisible = true;
    g_state.edit.lastCaretTick = GetTickCount();
    PrimeEditKeys();
}

void CancelEdit()
{
    g_state.edit = {};
}

void UpdateCaretBlink()
{
    if (!g_state.edit.active)
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (now - g_state.edit.lastCaretTick >= kCaretBlinkMs)
    {
        g_state.edit.caretVisible = !g_state.edit.caretVisible;
        g_state.edit.lastCaretTick = now;
    }
}

bool ConsumeEditKeyEdge(int virtualKey)
{
    if (virtualKey < 0 || virtualKey >= 256)
    {
        return false;
    }

    const bool down = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    const size_t index = static_cast<size_t>(virtualKey);
    const bool pressed = down && g_state.edit.keyDown[index] == 0;
    g_state.edit.keyDown[index] = down ? 1u : 0u;
    return pressed;
}

void EraseLastUtf8Codepoint(std::string& text)
{
    while (!text.empty())
    {
        const unsigned char byte = static_cast<unsigned char>(text.back());
        text.pop_back();
        if ((byte & 0xC0u) != 0x80u)
        {
            break;
        }
    }
}

size_t ClampCaretOffset(const std::string& text, size_t offset)
{
    if (offset >= text.size())
    {
        return text.size();
    }
    while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u)
    {
        --offset;
    }
    return offset;
}

size_t PrevUtf8Boundary(const std::string& text, size_t offset)
{
    offset = ClampCaretOffset(text, offset);
    if (offset == 0)
    {
        return 0;
    }
    --offset;
    while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u)
    {
        --offset;
    }
    return offset;
}

size_t NextUtf8Boundary(const std::string& text, size_t offset)
{
    offset = ClampCaretOffset(text, offset);
    if (offset >= text.size())
    {
        return text.size();
    }
    ++offset;
    while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0u) == 0x80u)
    {
        ++offset;
    }
    return offset;
}

bool IsAllowedFilterChar(char c)
{
    return static_cast<unsigned char>(c) >= 32u;
}

void TouchEditCaret()
{
    g_state.edit.caretVisible = true;
    g_state.edit.lastCaretTick = GetTickCount();
}

void InsertEditBytes(const std::string& bytes)
{
    if (!g_state.edit.active || bytes.empty())
    {
        return;
    }
    const size_t caret = ClampCaretOffset(g_state.edit.buffer, g_state.edit.caretByteOffset);
    g_state.edit.buffer.insert(caret, bytes);
    g_state.edit.caretByteOffset = caret + bytes.size();
    TouchEditCaret();
}

bool EraseCodepointBeforeCaret()
{
    const size_t caret = ClampCaretOffset(g_state.edit.buffer, g_state.edit.caretByteOffset);
    if (caret == 0)
    {
        return false;
    }
    const size_t start = PrevUtf8Boundary(g_state.edit.buffer, caret);
    g_state.edit.buffer.erase(start, caret - start);
    g_state.edit.caretByteOffset = start;
    TouchEditCaret();
    return true;
}

bool EraseCodepointAtCaret()
{
    const size_t caret = ClampCaretOffset(g_state.edit.buffer, g_state.edit.caretByteOffset);
    if (caret >= g_state.edit.buffer.size())
    {
        return false;
    }
    const size_t end = NextUtf8Boundary(g_state.edit.buffer, caret);
    g_state.edit.buffer.erase(caret, end - caret);
    g_state.edit.caretByteOffset = caret;
    TouchEditCaret();
    return true;
}

void TryAppendUtf8Text(const std::string& utf8)
{
    if (utf8.empty())
    {
        return;
    }

    if (g_state.edit.buffer.size() + utf8.size() > kMaxFilterTextBytes)
    {
        return;
    }

    InsertEditBytes(utf8);
}

std::string WcharToUtf8(wchar_t ch)
{
    return WideToUtf8(std::wstring(1, ch));
}

bool DrainWmCharMessages()
{
    bool changed = false;
    MSG message = {};
    while (PeekMessageW(&message, nullptr, WM_CHAR, WM_CHAR, PM_REMOVE) != 0)
    {
        const wchar_t ch = static_cast<wchar_t>(message.wParam);
        if (ch < 128 || ch == L'\r' || ch == L'\n' || ch == L'\b' || ch == 0x1B)
        {
            continue;
        }

        const std::string utf8 = WcharToUtf8(ch);
        if (utf8.empty())
        {
            continue;
        }

        bool allowed = true;
        if (utf8.size() == 1)
        {
            allowed = IsAllowedFilterChar(utf8[0]);
        }
        if (allowed)
        {
            TryAppendUtf8Text(utf8);
            changed = true;
        }
    }

    while (PeekMessageW(&message, nullptr, WM_IME_CHAR, WM_IME_CHAR, PM_REMOVE) != 0)
    {
        const wchar_t ch = static_cast<wchar_t>(message.wParam);
        if (ch < 128 || ch == L'\r' || ch == L'\n' || ch == L'\b' || ch == 0x1B)
        {
            continue;
        }

        const std::string utf8 = WcharToUtf8(ch);
        if (utf8.empty())
        {
            continue;
        }

        TryAppendUtf8Text(utf8);
        changed = true;
    }

    return changed;
}

bool CommitEdit()
{
    const std::string value = TrimUtf8(g_state.edit.buffer);
    switch (g_state.edit.field)
    {
    case FilterEditField::PlayerName:
        g_state.draftFilter.playerName = value;
        break;
    case FilterEditField::OpponentName:
        g_state.draftFilter.opponentName = value;
        break;
    default:
        break;
    }

    CancelEdit();
    return true;
}

bool HandleEditInput(uint32_t screenContext, const uint8_t* inputBytes)
{
    if (!g_state.edit.active)
    {
        return false;
    }

    UpdateCaretBlink();
    bool changed = DrainWmCharMessages();

    if (ConsumeEditKeyEdge(VK_LEFT))
    {
        g_state.edit.caretByteOffset = PrevUtf8Boundary(g_state.edit.buffer, g_state.edit.caretByteOffset);
        TouchEditCaret();
    }
    if (ConsumeEditKeyEdge(VK_RIGHT))
    {
        g_state.edit.caretByteOffset = NextUtf8Boundary(g_state.edit.buffer, g_state.edit.caretByteOffset);
        TouchEditCaret();
    }
    if (ConsumeEditKeyEdge(VK_HOME))
    {
        g_state.edit.caretByteOffset = 0;
        TouchEditCaret();
    }
    if (ConsumeEditKeyEdge(VK_END))
    {
        g_state.edit.caretByteOffset = g_state.edit.buffer.size();
        TouchEditCaret();
    }

    if (ConsumeEditKeyEdge(VK_BACK))
    {
        if (EraseCodepointBeforeCaret())
        {
            changed = true;
        }
    }
    if (ConsumeEditKeyEdge(VK_DELETE))
    {
        if (EraseCodepointAtCaret())
        {
            changed = true;
        }
    }

    auto appendPrintable = [&](int virtualKey)
    {
        if (!ConsumeEditKeyEdge(virtualKey))
        {
            return;
        }

        char c = 0;
        if (!netplay::input::TryTranslateVirtualKeyToAscii(virtualKey, &c))
        {
            return;
        }

        const size_t before = g_state.edit.buffer.size();
        if (IsAllowedFilterChar(c))
        {
            TryAppendUtf8Text(std::string(1, c));
        }
        changed = changed || g_state.edit.buffer.size() != before;
    };

    if (ConsumeEditKeyEdge('V'))
    {
        if (netplay::input::IsCtrlPressed())
        {
            const HWND owner =
                reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + netplay::constants::kOffsetWindowHandle));
            std::string clipboardText;
            if (netplay::input::TryReadClipboardUtf8Text(owner, &clipboardText))
            {
                const size_t before = g_state.edit.buffer.size();
                std::string filtered;
                filtered.reserve(clipboardText.size());
                for (char c : clipboardText)
                {
                    if (IsAllowedFilterChar(c))
                    {
                        filtered.push_back(c);
                    }
                }
                TryAppendUtf8Text(filtered);
                changed = changed || g_state.edit.buffer.size() != before;
            }
        }
        else
        {
            char c = 0;
            if (netplay::input::TryTranslateVirtualKeyToAscii('V', &c) && IsAllowedFilterChar(c))
            {
                const size_t before = g_state.edit.buffer.size();
                TryAppendUtf8Text(std::string(1, c));
                changed = changed || g_state.edit.buffer.size() != before;
            }
        }
    }

    for (int vk = 'A'; vk <= 'Z'; ++vk)
    {
        if (vk == 'V')
        {
            continue;
        }
        appendPrintable(vk);
    }
    for (int vk = '0'; vk <= '9'; ++vk)
    {
        appendPrintable(vk);
    }
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk)
    {
        appendPrintable(vk);
    }

    constexpr std::array<int, 12> kExtraKeys = {
        VK_SPACE,
        VK_DECIMAL,
        VK_OEM_PERIOD,
        VK_OEM_MINUS,
        VK_OEM_PLUS,
        VK_OEM_1,
        VK_OEM_2,
        VK_OEM_3,
        VK_OEM_4,
        VK_OEM_5,
        VK_OEM_6,
        VK_OEM_7,
    };
    for (int vk : kExtraKeys)
    {
        appendPrintable(vk);
    }

    bool confirmPressed = ConsumeEditKeyEdge(VK_RETURN);
    bool cancelPressed = ConsumeEditKeyEdge(VK_ESCAPE);
    (void)inputBytes;

    if (confirmPressed)
    {
        (void)CommitEdit();
    }
    else if (cancelPressed)
    {
        CancelEdit();
    }

    (void)changed;
    return true;
}
void DrawPanelBox(
    const netplay::font::IndexedSurfaceView& surface,
    int x,
    int y,
    int w,
    int h,
    uint8_t fillColor,
    uint8_t frameColor)
{
    netplay::font::FillIndexedSurfaceRect(surface, x, y, w, h, fillColor);
    netplay::font::DrawIndexedSurfaceFrame(surface, x, y, w, h, frameColor);
}

void DrawRowBox(
    const netplay::font::IndexedSurfaceView& surface,
    int y,
    int h,
    bool selected,
    uint8_t fillColor,
    uint8_t frameColor,
    uint8_t selectedFillColor)
{
    DrawPanelBox(
        surface,
        kRowX,
        y,
        kRowW,
        h,
        selected ? selectedFillColor : fillColor,
        frameColor);
}

int DrawChip(
    const netplay::font::IndexedSurfaceView& surface,
    const std::string& label,
    int x,
    int y,
    uint8_t fillColor,
    uint8_t frameColor,
    uint8_t textColor,
    bool switched)
{
    std::string chip = label.empty() ? "?" : label;
    if (switched)
    {
        chip += "*";
    }
    const int width = netplay::font::MeasureText5x7Width(chip, 1) + 6;
    DrawPanelBox(surface, x, y, width, 9, fillColor, frameColor);
    netplay::font::DrawTextCentered5x7(surface, chip, x + 1, x + width - 1, y + 1, 1, 1, textColor);
    return x + width;
}

void DrawSummaryPanel(
    const netplay::font::IndexedSurfaceView& surface,
    uint8_t panelFill,
    uint8_t panelFrame,
    uint8_t titleColor,
    uint8_t textColor,
    uint8_t dimColor,
    uint8_t warnColor)
{
    DrawPanelBox(surface, kPanelX, kPanelY, kPanelW, 92, panelFill, panelFrame);
    netplay::font::DrawTextCentered5x7(surface, "PLAYER SUMMARY", kPanelX + 2, kPanelX + kPanelW - 2, kPanelY + 4, 1, 1, titleColor);
    netplay::font::DrawTextCentered5x7(surface, g_state.currentNickname, kPanelX + 2, kPanelX + kPanelW - 2, kPanelY + 18, 1, 1, textColor);
    netplay::font::DrawTextLeft5x7(surface, BuildLoadStatusText(), kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 32, 1, 1, dimColor);
    netplay::font::DrawTextLeft5x7(surface, BuildSummaryRecordText(), kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 46, 1, 1, textColor);
    netplay::font::DrawTextLeft5x7(surface, BuildSummaryUsageText(), kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 60, 1, 1, textColor);
    const uint8_t recentColor = g_state.summary.hasSessions ? textColor : warnColor;
    netplay::font::DrawTextLeft5x7(surface, BuildSummaryRecentText(), kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 74, 1, 1, recentColor);
}

void DrawBrowserPanel(
    const netplay::font::IndexedSurfaceView& surface,
    uint8_t panelFill,
    uint8_t panelFrame,
    uint8_t titleColor,
    uint8_t textColor,
    uint8_t dimColor,
    uint8_t warnColor)
{
    DrawPanelBox(surface, kPanelX, kPanelY, kPanelW, 48, panelFill, panelFrame);
    netplay::font::DrawTextCentered5x7(surface, "SET BROWSER", kPanelX + 2, kPanelX + kPanelW - 2, kPanelY + 4, 1, 1, titleColor);
    netplay::font::DrawTextLeft5x7(surface, BuildFilterSummary(g_state.activeFilter), kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 18, 1, 1, textColor);
    netplay::font::DrawTextLeft5x7(surface, BuildBrowserPageText(), kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 32, 1, 1, dimColor);
    if (!g_state.document.warnings.empty())
    {
        const std::string warnText = "Warnings: " + std::to_string(g_state.document.warnings.size());
        netplay::font::DrawTextRight5x7(surface, warnText, kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 32, 1, 1, warnColor);
    }
}

void DrawFiltersPanel(
    const netplay::font::IndexedSurfaceView& surface,
    uint8_t panelFill,
    uint8_t panelFrame,
    uint8_t titleColor,
    uint8_t textColor,
    uint8_t dimColor)
{
    DrawPanelBox(surface, kPanelX, kPanelY, kPanelW, 72, panelFill, panelFrame);
    netplay::font::DrawTextCentered5x7(surface, "SEARCH / FILTER", kPanelX + 2, kPanelX + kPanelW - 2, kPanelY + 4, 1, 1, titleColor);
    netplay::font::DrawTextLeft5x7(surface, "Draft query:", kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 18, 1, 1, dimColor);
    netplay::font::DrawTextLeft5x7(surface, BuildFilterSummary(g_state.draftFilter), kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 32, 1, 1, textColor);
    netplay::font::DrawTextLeft5x7(surface, "Exact name match, session-level character match.", kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 50, 1, 1, dimColor);
}

void DrawDetailPanel(
    const netplay::font::IndexedSurfaceView& surface,
    uint8_t panelFill,
    uint8_t panelFrame,
    uint8_t titleColor,
    uint8_t textColor,
    uint8_t dimColor,
    uint8_t warnColor)
{
    DrawPanelBox(surface, kPanelX, kPanelY, kPanelW, 56, panelFill, panelFrame);
    netplay::font::DrawTextCentered5x7(surface, "SET DETAIL", kPanelX + 2, kPanelX + kPanelW - 2, kPanelY + 4, 1, 1, titleColor);

    const BattleLogSession* session = GetDetailSession();
    if (session == nullptr)
    {
        netplay::font::DrawTextCentered5x7(surface, "No set selected.", kPanelX + 2, kPanelX + kPanelW - 2, kPanelY + 24, 1, 1, warnColor);
        return;
    }

    const std::string matchup =
        session->p1Name + " " + FormatResultPair(GetSessionFinalScoreLeft(*session), GetSessionFinalScoreRight(*session))
        + " " + session->p2Name;
    netplay::font::DrawTextCentered5x7(surface, matchup, kPanelX + 2, kPanelX + kPanelW - 2, kPanelY + 18, 1, 1, textColor);
    netplay::font::DrawTextLeft5x7(surface, FormatDateTime(session->date, session->time), kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 32, 1, 1, dimColor);
    netplay::font::DrawTextRight5x7(surface, FormatDurationShort(session->totalDurationSeconds), kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 32, 1, 1, dimColor);
    if (session->warningCount > 0)
    {
        const std::string warningText = "Warnings: " + std::to_string(session->warningCount);
        netplay::font::DrawTextLeft5x7(surface, warningText, kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 44, 1, 1, warnColor);
    }
}

void DrawScreenTitleBar(
    const netplay::font::IndexedSurfaceView& surface,
    uint8_t fillColor,
    uint8_t frameColor,
    uint8_t titleColor,
    uint8_t /*dimColor*/)
{
    constexpr int kLogicalWidth = 320;
    DrawPanelBox(surface, 0, 0, kLogicalWidth, 14, fillColor, frameColor);
    netplay::font::DrawTextCentered5x7(surface, "BATTLE LOG", 2, kLogicalWidth - 2, 3, 1, 1, titleColor);
}

void DrawSummaryRows(
    const netplay::font::IndexedSurfaceView& surface,
    int selection,
    uint8_t rowFill,
    uint8_t rowFrame,
    uint8_t selectedFill,
    uint8_t normalText,
    uint8_t selectedText)
{
    static const std::array<const char*, 5> kLabels = {
        "BROWSE MY SETS",
        "SEARCH / FILTER",
        "BROWSE ALL SETS",
        "REFRESH LOG",
        "BACK",
    };

    for (size_t index = 0; index < kLabels.size(); ++index)
    {
        const bool isSelected = static_cast<int>(index) == selection;
        DrawRowBox(surface, kSummaryRowY[index], kSummaryRowH, isSelected, rowFill, rowFrame, selectedFill);
        netplay::font::DrawTextCentered5x7(
            surface,
            kLabels[index],
            kRowX + 4,
            kRowX + kRowW - 4,
            kSummaryRowY[index] + 4,
            1,
            1,
            isSelected ? selectedText : normalText);
    }
}

void DrawBrowserRows(
    const netplay::font::IndexedSurfaceView& surface,
    int selection,
    uint8_t rowFill,
    uint8_t rowFrame,
    uint8_t selectedFill,
    uint8_t normalText,
    uint8_t selectedText,
    uint8_t chipFill,
    uint8_t chipFrame,
    uint8_t chipText,
    uint8_t dimText,
    uint8_t warnColor)
{
    for (int slot = 0; slot < netplay::menu::kBattleLogVisibleSessionRows; ++slot)
    {
        const bool isSelected = slot == selection;
        const int rowY = kBrowserRowY[static_cast<size_t>(slot)];
        DrawRowBox(surface, rowY, kBrowserRowH, isSelected, rowFill, rowFrame, selectedFill);

        const BattleLogSession* session = GetSessionByIndex(GetSessionIndexForVisibleSlot(slot));
        if (session == nullptr)
        {
            if (slot == 0 && GetBrowserResultCount() == 0)
            {
                netplay::font::DrawTextCentered5x7(
                    surface,
                    "<no matching sets>",
                    kRowX + 4,
                    kRowX + kRowW - 4,
                    rowY + 2,
                    1,
                    1,
                    isSelected ? selectedText : warnColor);
            }
            continue;
        }

        const std::string shortDate =
            session->date.size() >= 10
                ? session->date.substr(5) + " " + session->time.substr(0, 5)
                : session->time.substr(0, (std::min)(session->time.size(), static_cast<size_t>(5)));
        const std::string leftName = AbbreviateForDisplay(session->p1Name, 8);
        const std::string rightName = AbbreviateForDisplay(session->p2Name, 8);
        const bool hasGames = !session->matches.empty();
        const std::string centerText =
            leftName + " "
            + (hasGames
                   ? FormatResultPair(GetSessionFinalScoreLeft(*session), GetSessionFinalScoreRight(*session))
                   : std::string("--"))
            + " "
            + rightName;
        netplay::font::DrawTextLeft5x7(
            surface,
            shortDate,
            kRowX + 4,
            82,
            rowY + 2,
            1,
            1,
            dimText);
        netplay::font::DrawTextLeft5x7(
            surface,
            centerText,
            84,
            218,
            rowY + 2,
            1,
            1,
            isSelected ? selectedText : normalText);
        int chipX = 220;
        chipX = DrawChip(
            surface,
            AbbreviateForDisplay(hasGames ? session->finalP1Character : std::string("--"), 4),
            chipX,
            rowY + 1,
            chipFill,
            chipFrame,
            chipText,
            session->p1SwitchedCharacter);
        chipX += 2;
        (void)DrawChip(
            surface,
            AbbreviateForDisplay(hasGames ? session->finalP2Character : std::string("--"), 4),
            chipX,
            rowY + 1,
            chipFill,
            chipFrame,
            chipText,
            session->p2SwitchedCharacter);
        if (session->warningCount > 0)
        {
            netplay::font::DrawTextRight5x7(
                surface,
                "!",
                kRowX + 4,
                kRowX + kRowW - 4,
                rowY + 2,
                1,
                1,
                warnColor);
        }
    }

    static const std::array<const char*, 4> kControls = {
        "PREV PAGE",
        "NEXT PAGE",
        "FILTERS",
        "BACK",
    };

    for (int control = 0; control < 4; ++control)
    {
        const int selectionIndex = netplay::menu::kBattleLogVisibleSessionRows + control;
        const bool isSelected = selectionIndex == selection;
        const int rowY = kBrowserRowY[static_cast<size_t>(selectionIndex)];
        DrawRowBox(surface, rowY, kBrowserRowH, isSelected, rowFill, rowFrame, selectedFill);
        netplay::font::DrawTextCentered5x7(
            surface,
            kControls[static_cast<size_t>(control)],
            kRowX + 4,
            kRowX + kRowW - 4,
            rowY + 2,
            1,
            1,
            isSelected ? selectedText : normalText);
    }
}

void DrawFilterRows(
    const netplay::font::IndexedSurfaceView& surface,
    int selection,
    uint8_t rowFill,
    uint8_t rowFrame,
    uint8_t selectedFill,
    uint8_t normalText,
    uint8_t selectedText,
    uint8_t dimText)
{
    struct FilterRow
    {
        const char* label;
        std::string value;
    };

    const std::array<FilterRow, 7> rows = {{
        {"PLAYER NAME", GetFilterFieldValue(FilterEditField::PlayerName, true)},
        {"OPPONENT NAME", GetFilterFieldValue(FilterEditField::OpponentName, true)},
        {"PLAYER CHARACTER", g_state.draftFilter.playerCharacter},
        {"OPPONENT CHARACTER", g_state.draftFilter.opponentCharacter},
        {"APPLY FILTERS", ""},
        {"RESET FILTERS", ""},
        {"BACK", ""},
    }};

    for (size_t index = 0; index < rows.size(); ++index)
    {
        const bool isSelected = static_cast<int>(index) == selection;
        const int rowY = kFiltersRowY[index];
        DrawRowBox(surface, rowY, kFilterRowH, isSelected, rowFill, rowFrame, selectedFill);
        netplay::font::DrawTextLeft5x7(
            surface,
            rows[index].label,
            kRowX + 6,
            160,
            rowY + 4,
            1,
            1,
            isSelected ? selectedText : normalText);
        if (!rows[index].value.empty())
        {
            netplay::font::DrawTextRight5x7(
                surface,
                rows[index].value,
                162,
                kRowX + kRowW - 6,
                rowY + 4,
                1,
                1,
                isSelected ? selectedText : dimText);
        }
    }
}

void DrawDetailRows(
    const netplay::font::IndexedSurfaceView& surface,
    int selection,
    uint8_t rowFill,
    uint8_t rowFrame,
    uint8_t selectedFill,
    uint8_t normalText,
    uint8_t selectedText,
    uint8_t chipFill,
    uint8_t chipFrame,
    uint8_t chipText,
    uint8_t dimText)
{
    (void)chipFill;
    (void)chipFrame;
    (void)chipText;

    for (int slot = 0; slot < netplay::menu::kBattleLogVisibleGameRows; ++slot)
    {
        const bool isSelected = slot == selection;
        const int rowY = kDetailRowY[static_cast<size_t>(slot)];
        DrawRowBox(surface, rowY, kDetailRowH, isSelected, rowFill, rowFrame, selectedFill);

        const BattleLogMatch* match = GetMatchForVisibleDetailSlot(slot);
        if (match == nullptr)
        {
            if (slot == 0)
            {
                netplay::font::DrawTextCentered5x7(
                    surface,
                    "<no games>",
                    kRowX + 4,
                    kRowX + kRowW - 4,
                    rowY + 2,
                    1,
                    1,
                    isSelected ? selectedText : dimText);
            }
            continue;
        }

        const int absoluteMatchIndex =
            g_state.detailPage * netplay::menu::kBattleLogVisibleGameRows + slot + 1;
        const std::string label = "#" + std::to_string(absoluteMatchIndex) + " " + match->time;
        const std::string centerText =
            AbbreviateForChip(match->p1CharacterDisplay) + " "
            + FormatResultPair(match->p1Rounds, match->p2Rounds) + " "
            + AbbreviateForChip(match->p2CharacterDisplay);
        const std::string rightText =
            FormatResultPair(match->p1Score, match->p2Score) + "  "
            + FormatDurationShort(match->durationSeconds);

        netplay::font::DrawTextLeft5x7(surface, label, kRowX + 4, 76, rowY + 2, 1, 1, dimText);
        netplay::font::DrawTextCentered5x7(
            surface,
            centerText,
            78,
            224,
            rowY + 2,
            1,
            1,
            isSelected ? selectedText : normalText);
        netplay::font::DrawTextRight5x7(surface, rightText, 182, kRowX + kRowW - 6, rowY + 2, 1, 1, dimText);
    }

    static const std::array<const char*, 3> kControls = {
        "PREV PAGE",
        "NEXT PAGE",
        "BACK",
    };
    for (int control = 0; control < 3; ++control)
    {
        const int selectionIndex = netplay::menu::kBattleLogVisibleGameRows + control;
        const bool isSelected = selectionIndex == selection;
        const int rowY = kDetailRowY[static_cast<size_t>(selectionIndex)];
        DrawRowBox(surface, rowY, kDetailRowH, isSelected, rowFill, rowFrame, selectedFill);
        netplay::font::DrawTextCentered5x7(
            surface,
            kControls[static_cast<size_t>(control)],
            kRowX + 4,
            kRowX + kRowW - 4,
            rowY + 2,
            1,
            1,
            isSelected ? selectedText : normalText);
    }
}
} // namespace

const NetplayMenuSpec* GetMenuSpec()
{
    EnsureSpecInitialized();
    return &g_state.spec;
}

void ResetState()
{
    g_state = {};
    EnsureSpecInitialized();
    g_state.activeFilter.playerCharacter = "All";
    g_state.activeFilter.opponentCharacter = "All";
    g_state.draftFilter = g_state.activeFilter;
}

bool EnterMenu()
{
    ResetState();
    g_state.currentNickname = hooks::g_netplayMenuState.nickname.empty()
        ? std::string("Player")
        : hooks::g_netplayMenuState.nickname;
    g_state.activeFilter.playerName = g_state.currentNickname;
    g_state.draftFilter = g_state.activeFilter;
    RefreshParsedDocument();
    g_state.view = View::Summary;
    RebuildMenuEntries();
    return true;
}

void LeaveMenu()
{
    ResetState();
}

std::string BuildRowLabel(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::BattleLogBrowseMine:
        return "BROWSE MY SETS";
    case NetplayMenuAction::BattleLogSearchFilters:
        return "SEARCH / FILTER";
    case NetplayMenuAction::BattleLogBrowseAll:
        return "BROWSE ALL SETS";
    case NetplayMenuAction::BattleLogRefresh:
        return "REFRESH LOG";
    case NetplayMenuAction::BattleLogEditPlayerName:
        return "PLAYER NAME";
    case NetplayMenuAction::BattleLogEditOpponentName:
        return "OPPONENT NAME";
    case NetplayMenuAction::BattleLogPlayerCharacter:
        return "PLAYER CHARACTER";
    case NetplayMenuAction::BattleLogOpponentCharacter:
        return "OPPONENT CHARACTER";
    case NetplayMenuAction::BattleLogApplyFilters:
        return "APPLY FILTERS";
    case NetplayMenuAction::BattleLogResetFilters:
        return "RESET FILTERS";
    case NetplayMenuAction::BattleLogBrowserPrevPage:
    case NetplayMenuAction::BattleLogDetailPrevPage:
        return "PREV PAGE";
    case NetplayMenuAction::BattleLogBrowserNextPage:
    case NetplayMenuAction::BattleLogDetailNextPage:
        return "NEXT PAGE";
    case NetplayMenuAction::BattleLogBrowserFilters:
        return "FILTERS";
    case NetplayMenuAction::BattleLogBack:
        return "BACK";
    default:
        break;
    }

    if (action >= NetplayMenuAction::BattleLogSession0
        && action <= NetplayMenuAction::BattleLogSession5)
    {
        const BattleLogSession* session = GetSessionByIndex(
            GetSessionIndexForVisibleSlot(
                static_cast<int>(action) - static_cast<int>(NetplayMenuAction::BattleLogSession0)));
        if (session == nullptr)
        {
            return "--";
        }
        return session->p1Name + " vs " + session->p2Name;
    }

    if (action >= NetplayMenuAction::BattleLogGame0
        && action <= NetplayMenuAction::BattleLogGame4)
    {
        const int slot =
            static_cast<int>(action) - static_cast<int>(NetplayMenuAction::BattleLogGame0);
        const BattleLogMatch* match = GetMatchForVisibleDetailSlot(slot);
        if (match == nullptr)
        {
            return "--";
        }
        return "GAME " + std::to_string(
            g_state.detailPage * netplay::menu::kBattleLogVisibleGameRows + slot + 1);
    }

    return "BATTLE LOG";
}

std::string BuildRowPrimaryText(NetplayMenuAction action)
{
    return BuildRowLabel(action);
}

std::string BuildRowSecondaryText(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::BattleLogEditPlayerName:
        return GetFilterFieldValue(FilterEditField::PlayerName, true);
    case NetplayMenuAction::BattleLogEditOpponentName:
        return GetFilterFieldValue(FilterEditField::OpponentName, true);
    case NetplayMenuAction::BattleLogPlayerCharacter:
        return g_state.draftFilter.playerCharacter;
    case NetplayMenuAction::BattleLogOpponentCharacter:
        return g_state.draftFilter.opponentCharacter;
    default:
        return {};
    }
}

std::string BuildFooterText(NetplayMenuAction selectedAction)
{
    std::string footer;
    if (g_state.edit.active)
    {
        footer = "Typing filter name. Enter/A=Apply  B/Esc=Cancel  Backspace=Delete";
    }
    else
    {
        switch (selectedAction)
        {
        case NetplayMenuAction::BattleLogBrowseMine:
            footer = "Show only sets involving your configured nickname.";
            break;
        case NetplayMenuAction::BattleLogSearchFilters:
        case NetplayMenuAction::BattleLogBrowserFilters:
            footer = "Edit player/opponent names and characters.";
            break;
        case NetplayMenuAction::BattleLogBrowseAll:
            footer = "Clear filters and browse the full battle log.";
            break;
        case NetplayMenuAction::BattleLogRefresh:
            footer = "Re-read BattleLog.txt from disk.";
            break;
        case NetplayMenuAction::BattleLogEditPlayerName:
        case NetplayMenuAction::BattleLogEditOpponentName:
            footer = "Press A or Enter to edit this field.";
            break;
        case NetplayMenuAction::BattleLogPlayerCharacter:
        case NetplayMenuAction::BattleLogOpponentCharacter:
            footer = "Use left/right or A to cycle through parsed characters.";
            break;
        case NetplayMenuAction::BattleLogApplyFilters:
            footer = "Run this query in the set browser.";
            break;
        case NetplayMenuAction::BattleLogResetFilters:
            footer = "Clear the draft filter and keep browsing.";
            break;
        case NetplayMenuAction::BattleLogBrowserPrevPage:
        case NetplayMenuAction::BattleLogDetailPrevPage:
            footer = "Show the previous page.";
            break;
        case NetplayMenuAction::BattleLogBrowserNextPage:
        case NetplayMenuAction::BattleLogDetailNextPage:
            footer = "Show the next page.";
            break;
        case NetplayMenuAction::BattleLogBack:
            switch (g_state.view)
            {
            case View::Summary:
                footer = "Return to the netplay main menu.";
                break;
            case View::Browser:
                footer = "Return to the summary screen.";
                break;
            case View::Filters:
                footer = "Return without applying draft changes.";
                break;
            case View::SetDetail:
                footer = "Return to the set browser.";
                break;
            }
            break;
        default:
            if (selectedAction >= NetplayMenuAction::BattleLogSession0
                && selectedAction <= NetplayMenuAction::BattleLogSession5)
            {
                footer = "Open the highlighted set.";
            }
            else if (selectedAction >= NetplayMenuAction::BattleLogGame0
                && selectedAction <= NetplayMenuAction::BattleLogGame4)
            {
                footer = "Per-game log entry inside this set.";
            }
            break;
        }
    }

    if (HasStatusMessage())
    {
        if (footer.empty())
        {
            return g_state.statusMessage;
        }
        return g_state.statusMessage + "\n" + footer;
    }
    return footer;
}

bool HandleVerticalNavigation(int /*currentSelection*/, int /*delta*/, int* /*outNextSelection*/)
{
    return false;
}

bool HandleInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter)
{
    if (inputBytes == nullptr || inactivityCounter == nullptr)
    {
        return false;
    }

    if (g_state.edit.active)
    {
        *inactivityCounter = 0;
        return HandleEditInput(screenContext, inputBytes);
    }

    const int selectedIndex = ClampSelectionForCurrentView(
        static_cast<int>(
            *reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection)));
    const NetplayMenuAction selectedAction =
        selectedIndex < g_state.spec.entryCount
        ? g_state.entries[static_cast<size_t>(selectedIndex)].action
        : NetplayMenuAction::BattleLogBack;

    bool consumed = false;
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch =
            reinterpret_cast<uint8_t*>(screenContext + netplay::constants::kOffsetInputLatchP1 + playerIndex);
        const int8_t horizontal = static_cast<int8_t>(inputBytes[playerIndex + 12]);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);

        if ((selectedAction == NetplayMenuAction::BattleLogPlayerCharacter
                || selectedAction == NetplayMenuAction::BattleLogOpponentCharacter)
            && horizontal != 0)
        {
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                const int delta = horizontal > 0 ? 1 : -1;
                if (selectedAction == NetplayMenuAction::BattleLogPlayerCharacter)
                {
                    CycleCharacterOption(&g_state.draftFilter.playerCharacter, delta);
                }
                else
                {
                    CycleCharacterOption(&g_state.draftFilter.opponentCharacter, delta);
                }
                hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
                *inputLatch = 1;
                consumed = true;
            }
        }
        else if (horizontal == 0 && vertical == 0)
        {
            *inputLatch = 0;
        }
    }

    return consumed;
}

bool HandleCancel(uint32_t screenContext)
{
    if (g_state.edit.active)
    {
        CancelEdit();
        return true;
    }

    switch (g_state.view)
    {
    case View::Summary:
        hooks::StartMenuSlideTransition(
            screenContext,
            NetplayMenuId::Main,
            hooks::g_netplayMenuState.mainSelection,
            -1);
        return true;
    case View::Browser:
        SwitchView(screenContext, View::Summary, -1);
        return true;
    case View::Filters:
        SwitchView(screenContext, g_state.filterReturnView, -1);
        return true;
    case View::SetDetail:
        SwitchView(screenContext, View::Browser, -1);
        return true;
    default:
        return false;
    }
}

bool ExecuteAction(uint32_t screenContext, NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::BattleLogBrowseMine:
        g_state.activeFilter = {};
        g_state.activeFilter.playerName = g_state.currentNickname;
        g_state.activeFilter.playerCharacter = "All";
        g_state.activeFilter.opponentCharacter = "All";
        g_state.draftFilter = g_state.activeFilter;
        g_state.browserPage = 0;
        RebuildFilteredSessionIndices();
        if (GetBrowserResultCount() == 0)
        {
            SetStatusMessage("No sets matched your nickname.");
        }
        SwitchView(screenContext, View::Browser, 0);
        return true;

    case NetplayMenuAction::BattleLogSearchFilters:
        g_state.filterReturnView = g_state.view;
        g_state.draftFilter = g_state.activeFilter;
        SanitizeFilterCharacters(&g_state.draftFilter);
        SwitchView(screenContext, View::Filters, 0);
        return true;

    case NetplayMenuAction::BattleLogBrowseAll:
        g_state.activeFilter = {};
        g_state.activeFilter.playerCharacter = "All";
        g_state.activeFilter.opponentCharacter = "All";
        g_state.draftFilter = g_state.activeFilter;
        g_state.browserPage = 0;
        RebuildFilteredSessionIndices();
        SwitchView(screenContext, View::Browser, 0);
        return true;

    case NetplayMenuAction::BattleLogRefresh:
        RefreshParsedDocument();
        RebuildMenuEntries();
        return true;

    case NetplayMenuAction::BattleLogEditPlayerName:
        BeginEdit(FilterEditField::PlayerName);
        return true;

    case NetplayMenuAction::BattleLogEditOpponentName:
        BeginEdit(FilterEditField::OpponentName);
        return true;

    case NetplayMenuAction::BattleLogPlayerCharacter:
        CycleCharacterOption(&g_state.draftFilter.playerCharacter, +1);
        return true;

    case NetplayMenuAction::BattleLogOpponentCharacter:
        CycleCharacterOption(&g_state.draftFilter.opponentCharacter, +1);
        return true;

    case NetplayMenuAction::BattleLogApplyFilters:
        NormalizeFilter(&g_state.draftFilter);
        SanitizeFilterCharacters(&g_state.draftFilter);
        g_state.activeFilter = g_state.draftFilter;
        g_state.browserPage = 0;
        RebuildFilteredSessionIndices();
        if (GetBrowserResultCount() == 0)
        {
            SetStatusMessage("No sets matched the current filter.");
        }
        SwitchView(screenContext, View::Browser, 0);
        return true;

    case NetplayMenuAction::BattleLogResetFilters:
        g_state.draftFilter = {};
        g_state.draftFilter.playerCharacter = "All";
        g_state.draftFilter.opponentCharacter = "All";
        SetStatusMessage("Draft filters cleared.");
        RebuildMenuEntries();
        return true;

    case NetplayMenuAction::BattleLogBrowserPrevPage:
        if (g_state.browserPage > 0)
        {
            --g_state.browserPage;
            SetStatusMessage("Previous page.");
        }
        RebuildMenuEntries();
        SetSelection(screenContext, *GetSelectionStorage(View::Browser));
        return true;

    case NetplayMenuAction::BattleLogBrowserNextPage:
        if (g_state.browserPage + 1 < GetBrowserPageCount())
        {
            ++g_state.browserPage;
            SetStatusMessage("Next page.");
        }
        RebuildMenuEntries();
        SetSelection(screenContext, *GetSelectionStorage(View::Browser));
        return true;

    case NetplayMenuAction::BattleLogBrowserFilters:
        g_state.filterReturnView = View::Browser;
        g_state.draftFilter = g_state.activeFilter;
        SanitizeFilterCharacters(&g_state.draftFilter);
        SwitchView(screenContext, View::Filters, 0);
        return true;

    case NetplayMenuAction::BattleLogDetailPrevPage:
        if (g_state.detailPage > 0)
        {
            --g_state.detailPage;
            SetStatusMessage("Previous page.");
        }
        RebuildMenuEntries();
        SetSelection(screenContext, *GetSelectionStorage(View::SetDetail));
        return true;

    case NetplayMenuAction::BattleLogDetailNextPage:
        if (g_state.detailPage + 1 < GetDetailPageCount())
        {
            ++g_state.detailPage;
            SetStatusMessage("Next page.");
        }
        RebuildMenuEntries();
        SetSelection(screenContext, *GetSelectionStorage(View::SetDetail));
        return true;

    case NetplayMenuAction::BattleLogBack:
        return HandleCancel(screenContext);

    default:
        break;
    }

    if (action >= NetplayMenuAction::BattleLogSession0
        && action <= NetplayMenuAction::BattleLogSession5)
    {
        const int slot =
            static_cast<int>(action) - static_cast<int>(NetplayMenuAction::BattleLogSession0);
        const int sessionIndex = GetSessionIndexForVisibleSlot(slot);
        if (sessionIndex >= 0)
        {
            g_state.detailSessionIndex = sessionIndex;
            g_state.detailPage = 0;
            SwitchView(screenContext, View::SetDetail, 0);
        }
        return true;
    }

    if (action >= NetplayMenuAction::BattleLogGame0
        && action <= NetplayMenuAction::BattleLogGame4)
    {
        return true;
    }

    return false;
}

bool DrawOverlayGdi(uint32_t screenContext, bool /*allowWindowDc*/)
{
    netplay::draw::LockedMenuSurface lockedSurface;
    if (!netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        return false;
    }

    const netplay::font::IndexedSurfaceView surface = {
        lockedSurface.pixels,
        lockedSurface.width,
        lockedSurface.height,
        lockedSurface.pitch,
    };

    const uint8_t panelFill = netplay::draw::ResolveBestPaletteColor(screenContext, 0, 0, 0);
    const uint8_t panelFrame = netplay::draw::ResolveBestPaletteColor(screenContext, 120, 170, 210);
    const uint8_t rowFill = netplay::draw::ResolveBestPaletteColor(screenContext, 0, 0, 0);
    const uint8_t rowFrame = netplay::draw::ResolveBestPaletteColor(screenContext, 72, 100, 140);
    const uint8_t selectedFill = netplay::draw::ResolveBestPaletteColor(screenContext, 18, 22, 30);
    const uint8_t titleColor = netplay::draw::ResolveBestPaletteColor(screenContext, 240, 242, 255);
    const uint8_t textColor = netplay::draw::ResolveBestPaletteColor(screenContext, 212, 220, 230);
    const uint8_t selectedText = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 255, 255);
    const uint8_t dimColor = netplay::draw::ResolveBestPaletteColor(screenContext, 150, 168, 190);
    const uint8_t warnColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 176, 110);
    const uint8_t chipFill = netplay::draw::ResolveBestPaletteColor(screenContext, 28, 40, 62);
    const uint8_t chipFrame = netplay::draw::ResolveBestPaletteColor(screenContext, 135, 180, 220);
    const uint8_t chipText = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 230, 160);
    const uint8_t titleFill = netplay::draw::ResolveBestPaletteColor(screenContext, 0, 0, 0);

    const int selection = ClampSelectionForCurrentView(
        static_cast<int>(
            *reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection)));

    DrawScreenTitleBar(surface, titleFill, panelFrame, titleColor, dimColor);

    switch (g_state.view)
    {
    case View::Summary:
        DrawSummaryPanel(surface, panelFill, panelFrame, titleColor, textColor, dimColor, warnColor);
        DrawSummaryRows(surface, selection, rowFill, rowFrame, selectedFill, textColor, selectedText);
        break;
    case View::Browser:
        DrawBrowserPanel(surface, panelFill, panelFrame, titleColor, textColor, dimColor, warnColor);
        DrawBrowserRows(surface, selection, rowFill, rowFrame, selectedFill, textColor, selectedText, chipFill, chipFrame, chipText, dimColor, warnColor);
        break;
    case View::Filters:
        DrawFiltersPanel(surface, panelFill, panelFrame, titleColor, textColor, dimColor);
        DrawFilterRows(surface, selection, rowFill, rowFrame, selectedFill, textColor, selectedText, dimColor);
        break;
    case View::SetDetail:
        DrawDetailPanel(surface, panelFill, panelFrame, titleColor, textColor, dimColor, warnColor);
        DrawDetailRows(surface, selection, rowFill, rowFrame, selectedFill, textColor, selectedText, chipFill, chipFrame, chipText, dimColor);
        break;
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}
} // namespace netplay::battle_log
