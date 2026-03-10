#include "netplay/core/options_menu.h"

#include "logger.h"
#include "netplay/core/input_utils.h"
#include "netplay/core/options_keybinds.h"
#include "netplay/core/text_utils.h"
#include "netplay/core/validation.h"
#include "netplay/hooks/internal/shared.h"
#include "netplay/render/draw_surface.h"
#include "netplay/render/software_font.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace netplay::options
{
namespace
{
using NetplayMenuAction = netplay::menu::NetplayMenuAction;
using NetplayMenuEntry = netplay::menu::NetplayMenuEntry;
using NetplayMenuId = netplay::menu::NetplayMenuId;
using NetplayMenuSpec = netplay::menu::NetplayMenuSpec;
namespace hooks = netplay::hooks::internal;

constexpr int kBackRow = netplay::menu::RowToIndex(netplay::menu::NetplayObRow::Blank);
constexpr uint32_t kStatusDisplayMs = 1800;
constexpr uint32_t kCaretBlinkMs = 350;
constexpr uint32_t kMaxStringBytes = 255;
constexpr uint32_t kMaxIntDigits = 10;

enum class FileEncoding : uint8_t
{
    Utf16Le = 0,
    Utf8Bom,
    Utf8,
};

enum class ItemKind : uint8_t
{
    SectionHeader = 0,
    String,
    Integer,
    KeyBinding,
    Choice,
    BoolText,
    BoolInt,
    Protocol,
};

struct Item
{
    ItemKind kind = ItemKind::String;
    std::string sectionName;
    std::string keyName;
    std::wstring rawKeyName;
    std::string currentValue;
    std::string originalValue;
    std::string tooltipSummary;
    std::vector<std::string> choiceValues;
    int lineIndex = -1;
};

struct EditState
{
    bool active = false;
    int itemIndex = -1;
    std::string buffer;
    size_t caretByteOffset = 0;
    std::array<uint8_t, 256> keyDown = {};
    bool caretVisible = true;
    DWORD lastCaretTick = 0;
    std::string errorMessage;
    DWORD errorExpireTick = 0;
};

struct Category
{
    std::string sectionName;
    std::string tooltipSummary;
    std::vector<int> itemIndices;
};

enum class VisibleEntryKind : uint8_t
{
    None = 0,
    Category,
    Item,
};

struct VisibleEntry
{
    VisibleEntryKind kind = VisibleEntryKind::None;
    int categoryIndex = -1;
    int itemIndex = -1;
};

enum class ModalKind : uint8_t
{
    None = 0,
    Save,
    Exit,
    Rebind,
};

struct ModalOverlayState
{
    bool active = false;
    ModalKind kind = ModalKind::None;
    int selectedOption = 0;
    int itemIndex = -1;
    std::array<uint8_t, 256> keyDown = {};
    std::array<uint32_t, 32> padButtonsDown = {};
    std::string errorMessage;
};

struct State
{
    std::string iniPath;
    FileEncoding encoding = FileEncoding::Utf16Le;
    std::vector<std::wstring> lines;
    std::vector<Item> items;
    std::vector<Category> categories;
    int currentCategoryIndex = -1;
    int scrollOffset = 0;
    EditState edit = {};
    ModalOverlayState modal = {};
    DWORD statusExpireTick = 0;
    std::string statusMessage;
    std::array<NetplayMenuEntry, kVisibleRowCount + 1> entries = {};
    NetplayMenuSpec spec = {};
};

State g_state = {};
std::string g_otherOfflineVsMode = "Tournament";

void EnsureSpecInitialized();
std::wstring TrimWide(std::wstring_view value);
std::string CollapseAsciiWhitespace(std::string text);
std::string WideToUtf8(const std::wstring& wide);
std::wstring Utf8ToWide(const std::string& utf8);
bool DecodeUtf8(const std::vector<uint8_t>& bytes, size_t offset, std::wstring* outWide);
bool DecodeAnsi(const std::vector<uint8_t>& bytes, size_t offset, std::wstring* outWide);
bool ReadWideTextFile(const std::string& path, std::wstring* outText, FileEncoding* outEncoding);
bool WriteWideTextFile(const std::string& path, const std::wstring& text, FileEncoding encoding);
std::vector<std::wstring> SplitLines(const std::wstring& text);
std::wstring JoinLines(const std::vector<std::wstring>& lines);
bool IsDecorativeCommentLine(const std::wstring& line);
std::wstring CleanCommentLine(const std::wstring& raw);
std::string TruncateForFooter(std::string text);
std::string SummarizeComments(const std::vector<std::wstring>& commentLines, const char* fallback);
std::string ResolveRevivalIniPath();
ItemKind InferItemKind(const std::string& key, const std::string& value);
std::string ResolveTooltipSummary(const std::string& sectionName, const std::string& keyName, const std::vector<std::wstring>& commentLines);
bool IsSectionHeader(const Item& item);
bool IsTextEditable(const Item& item);
bool IsKeyBindingEditable(const Item& item);
bool IsToggleEditable(const Item& item);
bool IsDirty(const Item& item);
bool HasDirtyItemsInCategory(int categoryIndex);
bool HasUnsavedChanges();
bool IsRootCategoryView();
bool IsValidCategoryIndex(int categoryIndex);
int GetCurrentContentCount();
int GetVisibleContentCount();
VisibleEntry GetVisibleEntryForSlot(int slot);
VisibleEntry GetVisibleEntryForAction(NetplayMenuAction action);
int GetItemIndexForAction(NetplayMenuAction action);
void SetStatusMessage(const char* text);
void ClearStatusMessage();
void RebuildMenuEntries();
bool LoadItemsFromIni();
void AppendSyntheticItems();
void ApplyRuntimeNetplaySettings();
bool SaveItemsToDisk();
void SetSelection(uint32_t screenContext, int selection);
void EnterCategoryView(uint32_t screenContext, int categoryIndex);
void ReturnToCategoryRoot(uint32_t screenContext, int focusCategoryIndex);
void OpenModal(ModalKind kind);
void OpenRebindModal(int itemIndex);
void CloseModal();
bool CommitExit(uint32_t screenContext, bool saveChanges);
void ExitOptionsMenu(uint32_t screenContext);
void RequestExitOptionsMenu(uint32_t screenContext);
int FindBindingConflict(int itemIndex, const std::string& value);
std::string FormatDisplayValue(const Item& item);
std::string PrettySectionLabel(const std::string& sectionName);
std::string PrettyKeyLabel(const std::string& key);
std::string TruncateLabel(std::string text, size_t maxChars);
size_t GetStringEditLimit(const Item& item);
void ClearEditError();
void SetEditError(const char* error);
void UpdateCaretBlink();
bool ConsumeEditKeyEdge(int virtualKey);
void PrimeEditKeys();
void EraseLastUtf8Codepoint(std::string& text);
bool IsAllowedEditChar(const Item& item, char c);
void AppendEditChar(const Item& item, char c);
void AppendUtf8Text(const Item& item, const std::string& text);
std::string WcharToUtf8(wchar_t ch);
void DrainWmCharMessages(const Item& item);
void BeginEdit(int itemIndex);
void CancelEdit();
bool ValidateUnsignedInteger(const std::string& text);
bool CommitEdit();
void ToggleValue(Item& item);
std::string GetEditedDisplayValue(const Item& item);
std::string BuildCategoryLabel(int categoryIndex);
std::string BuildSettingLabel(const Item& item);
bool HandleEditInput(uint32_t screenContext, const uint8_t* inputBytes, bool* escapeDown);
bool HandleRebindOverlayInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter, bool* escapeDown);
bool HandleModalOverlayInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter, bool* escapeDown);
int FindCategoryIndexBySectionName(const char* sectionName);
int FindItemIndexBySectionAndKey(const char* sectionName, const char* keyName);
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
}

bool EnterMenu()
{
    ResetState();
    const bool loaded = LoadItemsFromIni();
    RebuildMenuEntries();
    return loaded;
}

void LeaveMenu()
{
    ResetState();
}

bool IsVisibleRowAction(NetplayMenuAction action)
{
    return action >= NetplayMenuAction::OptionRow0
        && action <= NetplayMenuAction::OptionRow7;
}

namespace
{
void EnsureSpecInitialized()
{
    if (g_state.spec.entries != nullptr)
    {
        return;
    }

    g_state.spec.menuId = NetplayMenuId::Options;
    g_state.spec.headerLabel = "OPTIONS";
    g_state.spec.entries = g_state.entries.data();
    g_state.spec.entryCount = 1;
    g_state.spec.defaultSelection = 0;
    g_state.entries[0] = {NetplayMenuAction::BackToMain, kBackRow, "BACK"};
}

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

std::string CollapseAsciiWhitespace(std::string text)
{
    std::string out;
    out.reserve(text.size());
    bool lastWasSpace = false;
    for (unsigned char c : text)
    {
        if (std::isspace(c) != 0)
        {
            if (!lastWasSpace)
            {
                out.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }
        out.push_back(static_cast<char>(c));
        lastWasSpace = false;
    }
    return netplay::text::TrimAscii(std::move(out));
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

bool WriteWideTextFile(const std::string& path, const std::wstring& text, FileEncoding encoding)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        return false;
    }

    if (encoding == FileEncoding::Utf16Le)
    {
        const uint8_t bom[2] = {0xFFu, 0xFEu};
        file.write(reinterpret_cast<const char*>(bom), sizeof(bom));
        if (!text.empty())
        {
            file.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
        }
        return file.good();
    }

    const std::string utf8 = WideToUtf8(text);
    if (encoding == FileEncoding::Utf8Bom)
    {
        const uint8_t bom[3] = {0xEFu, 0xBBu, 0xBFu};
        file.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    }
    if (!utf8.empty())
    {
        file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    }
    return file.good();
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

std::wstring JoinLines(const std::vector<std::wstring>& lines)
{
    std::wstring text;
    for (size_t i = 0; i < lines.size(); ++i)
    {
        text += lines[i];
        if (i + 1 < lines.size())
        {
            text += L"\r\n";
        }
    }
    if (!lines.empty())
    {
        text += L"\r\n";
    }
    return text;
}

bool IsDecorativeCommentLine(const std::wstring& line)
{
    for (wchar_t ch : line)
    {
        if (iswalnum(ch) != 0 || ch >= 0x80)
        {
            return false;
        }
    }
    return true;
}

std::wstring CleanCommentLine(const std::wstring& raw)
{
    std::wstring line = TrimWide(raw);
    if (!line.empty() && line.front() == L';')
    {
        line.erase(line.begin());
    }
    line = TrimWide(line);

    bool changed = true;
    while (changed)
    {
        changed = false;
        if (line.rfind(L"/*", 0) == 0)
        {
            line.erase(0, 2);
            line = TrimWide(line);
            changed = true;
        }
        if (!line.empty() && line.front() == L'*')
        {
            line.erase(line.begin());
            line = TrimWide(line);
            changed = true;
        }
        if (line.size() >= 2 && line.substr(line.size() - 2) == L"*/")
        {
            line.erase(line.size() - 2);
            line = TrimWide(line);
            changed = true;
        }
    }

    if (line.empty() || IsDecorativeCommentLine(line))
    {
        return {};
    }
    return line;
}

std::string TruncateForFooter(std::string text)
{
    text = CollapseAsciiWhitespace(std::move(text));
    constexpr size_t kMaxChars = 58;
    if (text.size() <= kMaxChars)
    {
        return text;
    }
    text.resize(kMaxChars - 3);
    text += "...";
    return text;
}

std::string SummarizeComments(const std::vector<std::wstring>& commentLines, const char* fallback)
{
    std::wstring joined;
    for (const std::wstring& raw : commentLines)
    {
        const std::wstring cleaned = CleanCommentLine(raw);
        if (cleaned.empty())
        {
            continue;
        }
        if (!joined.empty())
        {
            joined.push_back(L' ');
        }
        joined += cleaned;
    }

    std::string summary = CollapseAsciiWhitespace(WideToUtf8(joined));
    if (summary.empty())
    {
        summary = fallback != nullptr ? fallback : "";
    }

    const size_t sentenceEnd = summary.find(". ");
    if (sentenceEnd != std::string::npos)
    {
        summary.resize(sentenceEnd + 1);
    }
    return CollapseAsciiWhitespace(std::move(summary));
}

std::string ResolveRevivalIniPath()
{
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, static_cast<DWORD>(std::size(exePath))) == 0)
    {
        return "EfzRevival.ini";
    }

    std::string iniPath = exePath;
    const size_t sep = iniPath.find_last_of("\\/");
    if (sep == std::string::npos)
    {
        return "EfzRevival.ini";
    }

    iniPath.resize(sep + 1);
    iniPath += "EfzRevival.ini";

    const DWORD exeAttrs = GetFileAttributesA(iniPath.c_str());
    if (exeAttrs != INVALID_FILE_ATTRIBUTES && (exeAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return iniPath;
    }

    const DWORD localAttrs = GetFileAttributesA("EfzRevival.ini");
    if (localAttrs != INVALID_FILE_ATTRIBUTES && (localAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return "EfzRevival.ini";
    }

    return iniPath;
}

ItemKind InferItemKind(const std::string& key, const std::string& value)
{
    if (keybinds::IsBindableValue(value))
    {
        return ItemKind::KeyBinding;
    }
    if (value == "True" || value == "False")
    {
        return ItemKind::BoolText;
    }
    if (value == "0" || value == "1")
    {
        return ItemKind::BoolInt;
    }
    if (value == "IPv4" || value == "IPv6")
    {
        return ItemKind::Protocol;
    }
    if (key == "Port" || key == "MaxRollback"
        || key.find("Window") != std::string::npos
        || key.find("BackBuffer") != std::string::npos)
    {
        return ItemKind::Integer;
    }
    return ItemKind::String;
}

std::string ResolveTooltipSummary(const std::string& sectionName, const std::string& keyName, const std::vector<std::wstring>& commentLines)
{
    const std::string parsed = SummarizeComments(commentLines, "");
    const auto matches = [&](const char* section, const char* key)
    {
        return sectionName == section && keyName == key;
    };

    if (matches("Network", "HolePunchingServer"))
    {
        return "Address of the UDP hole punching server used to help peers connect.";
    }
    if (matches("Network", "IncreaseInputDelay"))
    {
        return "Increase the match input delay. Higher delay reduces rollbacks but adds more input latency.";
    }
    if (matches("Network", "DecreaseInputDelay"))
    {
        return "Decrease the match input delay. Lower delay feels more responsive but may increase rollbacks.";
    }
    if (matches("Network", "IncreaseSpecSpeed"))
    {
        return "Increase spectator catch-up speed while watching an online match.";
    }
    if (matches("Network", "DecreaseSpecSpeed"))
    {
        return "Decrease spectator catch-up speed for smoother playback after you have caught up.";
    }
    if (matches("Network", "ToggleRemotePalettes"))
    {
        return "Toggle whether the opponent's selected palette is shown during netplay.";
    }
    if (matches("Network", "ReplayFolder"))
    {
        return "Folder used when forced netplay replays are saved.";
    }
    if (matches("Network", "BattleLogFile"))
    {
        return "File name used to append online match results.";
    }
    if (matches("Practice", "Pause"))
    {
        return "Pause the current local-play match.";
    }
    if (matches("Practice", "StepFrame"))
    {
        return "Advance the paused match by one frame.";
    }
    if (matches("Practice", "Save"))
    {
        return "Save the current local-play state.";
    }
    if (matches("Practice", "Load"))
    {
        return "Load the previously saved local-play state.";
    }
    if (matches("Practice", "ToggleRecord"))
    {
        return "Start or stop recording the dummy's actions in the current buffer.";
    }
    if (matches("Practice", "ToggleReplay"))
    {
        return "Start or stop replaying the current buffer.";
    }
    if (matches("Practice", "SwitchStartCondition"))
    {
        return "Change when record and replay begin in VS Player.";
    }
    if (sectionName == "Practice" && keyName.rfind("SetBuffer", 0) == 0)
    {
        const std::string bufferIndex = keyName.substr(std::strlen("SetBuffer"));
        return "Select replay buffer " + bufferIndex + " for recording and playback.";
    }
    if (matches("Practice", "ToggleReplayRandom"))
    {
        return "Replay a random recorded buffer instead of the current one.";
    }
    if (matches("Practice", "ToggleDisplay"))
    {
        return "Show or hide the Revival practice overlay.";
    }
    if (matches("Practice", "ToggleHitBoxes"))
    {
        return "Show or hide hitboxes.";
    }
    if (matches("Practice", "ToggleHurtBoxes"))
    {
        return "Show or hide hurtboxes.";
    }
    if (matches("Practice", "ToggleCollisionBoxes"))
    {
        return "Show or hide collision boxes and the cross-up guide line.";
    }
    if (matches("Practice", "SwitchPlayers"))
    {
        return "Swap the player and dummy sides in VS Player.";
    }
    if (matches("Practice", "MirrorPlayers"))
    {
        return "Mirror the player's position to practice the other side faster.";
    }
    if (matches("Practice", "IncreaseFPS"))
    {
        return "Increase the local-play game speed in 16 FPS steps.";
    }
    if (matches("Practice", "DecreaseFPS"))
    {
        return "Decrease the local-play game speed in 16 FPS steps.";
    }
    if (matches("Practice", "ResetFPS"))
    {
        return "Restore the local-play game speed to normal.";
    }
    if (matches("Tournament", "P1_Name"))
    {
        return "Set the tournament overlay name for player 1.";
    }
    if (matches("Tournament", "P2_Name"))
    {
        return "Set the tournament overlay name for player 2.";
    }
    if (matches("Tournament", "TournamentFolder"))
    {
        return "Folder used for tournament replays and the tournament log.";
    }
    if (matches("Global", "WindowX"))
    {
        return "Horizontal window position at startup.";
    }
    if (matches("Global", "WindowY"))
    {
        return "Vertical window position at startup.";
    }
    if (matches("Global", "WindowWidth"))
    {
        return "Window width at startup.";
    }
    if (matches("Global", "WindowHeight"))
    {
        return "Window height at startup.";
    }
    if (matches("Global", "BackBufferWidth"))
    {
        return "Internal backbuffer width used for rendering.";
    }
    if (matches("Global", "BackBufferHeight"))
    {
        return "Internal backbuffer height used for rendering.";
    }
    if (matches("Global", "ToggleBGM"))
    {
        return "Toggle the game's BGM on or off.";
    }
    if (matches("Global", "MuteBGM"))
    {
        return "Mute all in-game BGM by default.";
    }
    if (matches("Global", "BGMFolder"))
    {
        return "Folder used for external BGM files when Revival loads custom music.";
    }

    if (!parsed.empty())
    {
        return parsed;
    }

    return "Adjust " + PrettyKeyLabel(keyName) + ".";
}

bool IsSectionHeader(const Item& item)
{
    return item.kind == ItemKind::SectionHeader;
}

bool IsTextEditable(const Item& item)
{
    return item.kind == ItemKind::String || item.kind == ItemKind::Integer;
}

bool IsKeyBindingEditable(const Item& item)
{
    return item.kind == ItemKind::KeyBinding;
}

bool IsToggleEditable(const Item& item)
{
    return item.kind == ItemKind::BoolText
        || item.kind == ItemKind::BoolInt
        || item.kind == ItemKind::Protocol
        || item.kind == ItemKind::Choice;
}

bool IsDirty(const Item& item)
{
    return item.currentValue != item.originalValue;
}

bool HasDirtyItemsInCategory(int categoryIndex)
{
    if (!IsValidCategoryIndex(categoryIndex))
    {
        return false;
    }

    const Category& category = g_state.categories[static_cast<size_t>(categoryIndex)];
    for (int itemIndex : category.itemIndices)
    {
        if (itemIndex >= 0
            && itemIndex < static_cast<int>(g_state.items.size())
            && IsDirty(g_state.items[static_cast<size_t>(itemIndex)]))
        {
            return true;
        }
    }
    return false;
}

bool HasUnsavedChanges()
{
    for (const Item& item : g_state.items)
    {
        if (IsDirty(item))
        {
            return true;
        }
    }
    return false;
}

bool IsRootCategoryView()
{
    return g_state.currentCategoryIndex < 0;
}

bool IsValidCategoryIndex(int categoryIndex)
{
    return categoryIndex >= 0 && categoryIndex < static_cast<int>(g_state.categories.size());
}

int GetCurrentContentCount()
{
    return IsRootCategoryView()
        ? static_cast<int>(g_state.categories.size())
        : (IsValidCategoryIndex(g_state.currentCategoryIndex)
            ? static_cast<int>(g_state.categories[static_cast<size_t>(g_state.currentCategoryIndex)].itemIndices.size())
            : 0);
}

int GetVisibleContentCount()
{
    if (g_state.scrollOffset < 0)
    {
        return 0;
    }

    const int contentCount = GetCurrentContentCount();
    const int remaining = contentCount - g_state.scrollOffset;
    return (std::max)(0, (std::min)(remaining, kVisibleRowCount));
}

VisibleEntry GetVisibleEntryForSlot(int slot)
{
    VisibleEntry visible = {};
    if (slot < 0 || slot >= GetVisibleContentCount())
    {
        return visible;
    }

    const int contentIndex = g_state.scrollOffset + slot;
    if (IsRootCategoryView())
    {
        if (contentIndex >= 0 && contentIndex < static_cast<int>(g_state.categories.size()))
        {
            visible.kind = VisibleEntryKind::Category;
            visible.categoryIndex = contentIndex;
        }
        return visible;
    }

    if (!IsValidCategoryIndex(g_state.currentCategoryIndex))
    {
        return visible;
    }

    const Category& category = g_state.categories[static_cast<size_t>(g_state.currentCategoryIndex)];
    if (contentIndex < 0 || contentIndex >= static_cast<int>(category.itemIndices.size()))
    {
        return visible;
    }

    visible.kind = VisibleEntryKind::Item;
    visible.categoryIndex = g_state.currentCategoryIndex;
    visible.itemIndex = category.itemIndices[static_cast<size_t>(contentIndex)];
    return visible;
}

VisibleEntry GetVisibleEntryForAction(NetplayMenuAction action)
{
    VisibleEntry visible = {};
    if (!IsVisibleRowAction(action))
    {
        return visible;
    }
    const int slot = static_cast<int>(action) - static_cast<int>(NetplayMenuAction::OptionRow0);
    return GetVisibleEntryForSlot(slot);
}

int GetItemIndexForAction(NetplayMenuAction action)
{
    const VisibleEntry visible = GetVisibleEntryForAction(action);
    return visible.kind == VisibleEntryKind::Item ? visible.itemIndex : -1;
}

void SetStatusMessage(const char* text)
{
    g_state.statusMessage = text != nullptr ? text : "";
    g_state.statusExpireTick = GetTickCount() + kStatusDisplayMs;
}

void ClearStatusMessage()
{
    g_state.statusMessage.clear();
    g_state.statusExpireTick = 0;
}

void RebuildMenuEntries()
{
    EnsureSpecInitialized();

    const int contentCount = GetCurrentContentCount();
    const int maxScroll = (std::max)(0, contentCount - kVisibleRowCount);
    if (g_state.scrollOffset < 0)
    {
        g_state.scrollOffset = 0;
    }
    if (g_state.scrollOffset > maxScroll)
    {
        g_state.scrollOffset = maxScroll;
    }

    const int visibleCount = GetVisibleContentCount();
    for (int i = 0; i < visibleCount; ++i)
    {
        g_state.entries[i] = {
            static_cast<NetplayMenuAction>(static_cast<int>(NetplayMenuAction::OptionRow0) + i),
            kBackRow,
            "OPTION_ROW",
        };
    }

    g_state.entries[visibleCount] = {
        NetplayMenuAction::BackToMain,
        kBackRow,
        "BACK",
    };

    g_state.spec.menuId = NetplayMenuId::Options;
    g_state.spec.headerLabel = "OPTIONS";
    g_state.spec.entries = g_state.entries.data();
    g_state.spec.entryCount = visibleCount + 1;
    g_state.spec.defaultSelection = 0;

    hooks::g_netplayMenuState.optionCount = g_state.spec.entryCount;
    hooks::g_netplayMenuState.backIndex = g_state.spec.entryCount > 0 ? (g_state.spec.entryCount - 1) : 0;
}

void SetSelection(uint32_t screenContext, int selection)
{
    int clamped = selection;
    if (clamped < 0)
    {
        clamped = 0;
    }
    if (g_state.spec.entryCount > 0 && clamped >= g_state.spec.entryCount)
    {
        clamped = g_state.spec.entryCount - 1;
    }

    *reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection) = static_cast<int8_t>(clamped);
    *reinterpret_cast<uint16_t*>(screenContext + netplay::constants::kOffsetMenuAnimCounter) = 0;
    hooks::g_lastLoggedSelection = static_cast<int8_t>(clamped);
}

void EnterCategoryView(uint32_t screenContext, int categoryIndex)
{
    if (!IsValidCategoryIndex(categoryIndex))
    {
        return;
    }

    g_state.currentCategoryIndex = categoryIndex;
    g_state.scrollOffset = 0;
    RebuildMenuEntries();
    SetSelection(screenContext, 0);
    mod::Log("OptionsMenu: enter category '%s'", g_state.categories[static_cast<size_t>(categoryIndex)].sectionName.c_str());
}

void ReturnToCategoryRoot(uint32_t screenContext, int focusCategoryIndex)
{
    g_state.currentCategoryIndex = -1;
    g_state.scrollOffset = 0;
    RebuildMenuEntries();
    SetSelection(screenContext, focusCategoryIndex);
    mod::Log("OptionsMenu: return to category root focus=%d", focusCategoryIndex);
}

void OpenModal(ModalKind kind)
{
    g_state.modal = {};
    g_state.modal.active = true;
    g_state.modal.kind = kind;
    g_state.modal.selectedOption = 0;
}

void OpenRebindModal(int itemIndex)
{
    if (itemIndex < 0 || itemIndex >= static_cast<int>(g_state.items.size()))
    {
        return;
    }

    g_state.modal = {};
    g_state.modal.active = true;
    g_state.modal.kind = ModalKind::Rebind;
    g_state.modal.itemIndex = itemIndex;
    netplay::input::PrimeKeyState(&g_state.modal.keyDown);
    keybinds::PrimePadButtonState(&g_state.modal.padButtonsDown);
}

void CloseModal()
{
    g_state.modal = {};
}

bool CommitExit(uint32_t screenContext, bool saveChanges)
{
    if (saveChanges && !SaveItemsToDisk())
    {
        g_state.modal.errorMessage = "Save failed.";
        return false;
    }

    CloseModal();
    ExitOptionsMenu(screenContext);
    return true;
}

void ExitOptionsMenu(uint32_t screenContext)
{
    hooks::StartMenuSlideTransition(screenContext, NetplayMenuId::Main, hooks::g_netplayMenuState.mainSelection, -1);
}

void RequestExitOptionsMenu(uint32_t screenContext)
{
    if (HasUnsavedChanges())
    {
        OpenModal(ModalKind::Exit);
        return;
    }

    ExitOptionsMenu(screenContext);
}

int FindBindingConflict(int itemIndex, const std::string& value)
{
    if (itemIndex < 0 || itemIndex >= static_cast<int>(g_state.items.size()))
    {
        return -1;
    }

    const Item& source = g_state.items[static_cast<size_t>(itemIndex)];
    const std::string normalized = keybinds::NormalizeBindingValue(value);
    for (size_t i = 0; i < g_state.items.size(); ++i)
    {
        if (static_cast<int>(i) == itemIndex)
        {
            continue;
        }

        const Item& other = g_state.items[i];
        if (!IsKeyBindingEditable(other))
        {
            continue;
        }

        if (other.sectionName != source.sectionName)
        {
            continue;
        }

        if (keybinds::NormalizeBindingValue(other.currentValue) == normalized)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string PrettySectionLabel(const std::string& sectionName)
{
    if (sectionName.empty())
    {
        return "Section";
    }
    return sectionName;
}

std::string PrettyKeyLabel(const std::string& key)
{
    if (key.empty())
    {
        return "Option";
    }

    std::string label;
    label.reserve(key.size() + 6);
    for (size_t i = 0; i < key.size(); ++i)
    {
        const char c = key[i];
        if (c == '_')
        {
            if (!label.empty() && label.back() != ' ')
            {
                label.push_back(' ');
            }
            continue;
        }

        const bool isUpper = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool prevIsLowerOrDigit =
            i > 0
            && (std::islower(static_cast<unsigned char>(key[i - 1])) != 0
                || std::isdigit(static_cast<unsigned char>(key[i - 1])) != 0);
        if (isUpper && prevIsLowerOrDigit && !label.empty() && label.back() != ' ')
        {
            label.push_back(' ');
        }
        label.push_back(c);
    }
    return label;
}

std::string TruncateLabel(std::string text, size_t maxChars)
{
    if (text.size() <= maxChars)
    {
        return text;
    }
    if (maxChars <= 3)
    {
        return text.substr(0, maxChars);
    }
    text.resize(maxChars - 3);
    text += "...";
    return text;
}

bool LoadItemsFromIni()
{
    g_state.iniPath = ResolveRevivalIniPath();
    g_state.items.clear();
    g_state.categories.clear();
    g_state.lines.clear();
    g_state.currentCategoryIndex = -1;
    g_state.scrollOffset = 0;
    g_state.edit = {};
    g_state.modal = {};
    ClearStatusMessage();

    std::wstring text;
    FileEncoding encoding = FileEncoding::Utf16Le;
    if (!ReadWideTextFile(g_state.iniPath, &text, &encoding))
    {
        SetStatusMessage("EfzRevival.ini not found.");
        mod::Log("OptionsMenu: failed to load '%s'", g_state.iniPath.c_str());
        return false;
    }

    g_state.encoding = encoding;
    g_state.lines = SplitLines(text);

    std::string currentSection;
    int currentCategoryIndex = -1;
    std::vector<std::wstring> commentLines;
    for (int lineIndex = 0; lineIndex < static_cast<int>(g_state.lines.size()); ++lineIndex)
    {
        const std::wstring& rawLine = g_state.lines[static_cast<size_t>(lineIndex)];
        const std::wstring trimmed = TrimWide(rawLine);
        if (trimmed.empty())
        {
            commentLines.clear();
            continue;
        }

        if (trimmed.front() == L';')
        {
            commentLines.push_back(trimmed);
            continue;
        }

        if (trimmed.front() == L'[' && trimmed.back() == L']')
        {
            currentSection = WideToUtf8(TrimWide(std::wstring_view(trimmed).substr(1, trimmed.size() - 2)));
            Category category;
            category.sectionName = currentSection;
            const std::string fallback = "Adjust " + PrettySectionLabel(currentSection) + " settings.";
            category.tooltipSummary = SummarizeComments(commentLines, fallback.c_str());
            g_state.categories.push_back(std::move(category));
            currentCategoryIndex = static_cast<int>(g_state.categories.size()) - 1;

            commentLines.clear();
            continue;
        }

        const size_t equals = trimmed.find(L'=');
        if (equals == std::wstring::npos || equals == 0)
        {
            commentLines.clear();
            continue;
        }

        const std::wstring keyWide = TrimWide(std::wstring_view(trimmed).substr(0, equals));
        const std::wstring valueWide = TrimWide(std::wstring_view(trimmed).substr(equals + 1));
        const std::string key = WideToUtf8(keyWide);
        const std::string value = WideToUtf8(valueWide);

        Item item;
        item.kind = InferItemKind(key, value);
        item.sectionName = currentSection;
        item.keyName = key;
        item.rawKeyName = keyWide;
        item.currentValue = value;
        item.originalValue = value;
        item.tooltipSummary = ResolveTooltipSummary(currentSection, key, commentLines);
        item.lineIndex = lineIndex;
        g_state.items.push_back(std::move(item));
        if (currentCategoryIndex >= 0)
        {
            g_state.categories[static_cast<size_t>(currentCategoryIndex)].itemIndices.push_back(
                static_cast<int>(g_state.items.size()) - 1);
        }

        commentLines.clear();
    }

    AppendSyntheticItems();

    mod::Log(
        "OptionsMenu: loaded %zu items across %zu categories from '%s'",
        g_state.items.size(),
        g_state.categories.size(),
        g_state.iniPath.c_str());
    return true;
}

void AppendSyntheticItems()
{
    int categoryIndex = FindCategoryIndexBySectionName("Others");
    if (categoryIndex < 0)
    {
        Category category;
        category.sectionName = "Others";
        category.tooltipSummary = "Mod-only settings used by efz_netplay_mod.";
        g_state.categories.push_back(std::move(category));
        categoryIndex = static_cast<int>(g_state.categories.size()) - 1;
    }

    const int existingItemIndex = FindItemIndexBySectionAndKey("Others", "OfflineVsHumanMode");
    if (existingItemIndex >= 0)
    {
        Item& item = g_state.items[static_cast<size_t>(existingItemIndex)];
        item.kind = ItemKind::Choice;
        item.choiceValues = {"Tournament", "VS Human"};
        item.tooltipSummary =
            "Choose whether the title-screen VS Human option launches tournament mode or regular VS Human.";
        if (std::find(item.choiceValues.begin(), item.choiceValues.end(), item.currentValue) == item.choiceValues.end())
        {
            item.currentValue = g_otherOfflineVsMode;
            item.originalValue = g_otherOfflineVsMode;
        }
        else
        {
            g_otherOfflineVsMode = item.currentValue;
        }
        return;
    }

    Item item;
    item.kind = ItemKind::Choice;
    item.sectionName = "Others";
    item.keyName = "OfflineVsHumanMode";
    item.rawKeyName = L"OfflineVsHumanMode";
    item.currentValue = g_otherOfflineVsMode;
    item.originalValue = g_otherOfflineVsMode;
    item.tooltipSummary =
        "Choose whether the title-screen VS Human option launches tournament mode or regular VS Human.";
    item.choiceValues = {"Tournament", "VS Human"};
    item.lineIndex = -1;

    g_state.items.push_back(std::move(item));
    g_state.categories[static_cast<size_t>(categoryIndex)].itemIndices.push_back(
        static_cast<int>(g_state.items.size()) - 1);
}

void ApplyRuntimeNetplaySettings()
{
    for (const Item& item : g_state.items)
    {
        if (item.sectionName != "Network")
        {
            continue;
        }

        if (item.keyName == "Name")
        {
            hooks::g_netplayMenuState.nickname = item.currentValue;
        }
        else if (item.keyName == "Port")
        {
            uint16_t port = 0;
            if (netplay::validation::ParsePort(item.currentValue, &port))
            {
                hooks::g_netplayMenuState.hostPort = port;
                hooks::g_netplayMenuState.joinPort = port;
            }
        }
    }
}

bool SaveItemsToDisk()
{
    if (g_state.iniPath.empty() || g_state.lines.empty())
    {
        return false;
    }

    std::vector<std::wstring> updatedLines = g_state.lines;
    for (Item& item : g_state.items)
    {
        if (item.lineIndex >= 0 && item.lineIndex < static_cast<int>(updatedLines.size()))
        {
            updatedLines[static_cast<size_t>(item.lineIndex)] = item.rawKeyName + L"=" + Utf8ToWide(item.currentValue);
            continue;
        }

        if (item.sectionName == "Others" && item.keyName == "OfflineVsHumanMode")
        {
            if (!updatedLines.empty() && !TrimWide(updatedLines.back()).empty())
            {
                updatedLines.push_back(L"");
            }
            updatedLines.push_back(L"[Others]");
            item.lineIndex = static_cast<int>(updatedLines.size());
            updatedLines.push_back(item.rawKeyName + L"=" + Utf8ToWide(item.currentValue));
        }
    }

    const std::wstring joined = JoinLines(updatedLines);
    if (!WriteWideTextFile(g_state.iniPath, joined, g_state.encoding))
    {
        mod::Log("OptionsMenu: failed to save '%s'", g_state.iniPath.c_str());
        return false;
    }

    g_state.lines = std::move(updatedLines);
    for (Item& item : g_state.items)
    {
        item.originalValue = item.currentValue;
        if (item.sectionName == "Others" && item.keyName == "OfflineVsHumanMode")
        {
            g_otherOfflineVsMode = item.currentValue;
        }
    }
    ApplyRuntimeNetplaySettings();
    SetStatusMessage("Settings saved.");
    mod::Log("OptionsMenu: saved '%s'", g_state.iniPath.c_str());
    return true;
}

int FindCategoryIndexBySectionName(const char* sectionName)
{
    if (sectionName == nullptr)
    {
        return -1;
    }

    for (size_t i = 0; i < g_state.categories.size(); ++i)
    {
        if (g_state.categories[i].sectionName == sectionName)
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int FindItemIndexBySectionAndKey(const char* sectionName, const char* keyName)
{
    if (sectionName == nullptr || keyName == nullptr)
    {
        return -1;
    }

    for (size_t i = 0; i < g_state.items.size(); ++i)
    {
        const Item& item = g_state.items[i];
        if (item.sectionName == sectionName && item.keyName == keyName)
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

std::string FormatDisplayValue(const Item& item)
{
    std::string value = item.currentValue;
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    {
        value = value.substr(1, value.size() - 2);
    }

    switch (item.kind)
    {
    case ItemKind::KeyBinding:
        return keybinds::FormatBindingValue(value);
    case ItemKind::BoolText:
        return value == "True" ? "On" : "Off";
    case ItemKind::BoolInt:
        return value == "1" ? "On" : "Off";
    case ItemKind::Choice:
        return value.empty() ? "(empty)" : value;
    case ItemKind::Protocol:
        return value;
    case ItemKind::Integer:
    case ItemKind::String:
        return value.empty() ? "(empty)" : value;
    case ItemKind::SectionHeader:
        return "[" + PrettySectionLabel(item.sectionName) + "]";
    default:
        return value;
    }
}

std::string BuildCategoryLabel(int categoryIndex)
{
    if (!IsValidCategoryIndex(categoryIndex))
    {
        return "[Category]";
    }

    const Category& category = g_state.categories[static_cast<size_t>(categoryIndex)];
    const std::string prefix = HasDirtyItemsInCategory(categoryIndex) ? "* " : "";
    return prefix + "[" + PrettySectionLabel(category.sectionName) + "]";
}

std::string BuildSettingLabel(const Item& item)
{
    const std::string prefix = IsDirty(item) ? "* " : "";
    return prefix + PrettyKeyLabel(item.keyName);
}

size_t GetStringEditLimit(const Item& item)
{
    if (item.sectionName == "Network" && item.keyName == "Name")
    {
        return 63;
    }
    return kMaxStringBytes;
}

void ClearEditError()
{
    g_state.edit.errorMessage.clear();
    g_state.edit.errorExpireTick = 0;
}

void SetEditError(const char* error)
{
    g_state.edit.errorMessage = error != nullptr ? error : "";
    g_state.edit.errorExpireTick = GetTickCount() + kStatusDisplayMs;
}

void UpdateCaretBlink()
{
    if (!g_state.edit.active)
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (g_state.edit.lastCaretTick == 0)
    {
        g_state.edit.lastCaretTick = now;
        g_state.edit.caretVisible = true;
        return;
    }

    if (now - g_state.edit.lastCaretTick >= kCaretBlinkMs)
    {
        g_state.edit.lastCaretTick = now;
        g_state.edit.caretVisible = !g_state.edit.caretVisible;
    }

    if (!g_state.edit.errorMessage.empty() && now >= g_state.edit.errorExpireTick)
    {
        ClearEditError();
    }
}

bool ConsumeEditKeyEdge(int virtualKey)
{
    return netplay::input::ConsumeKeyEdge(&g_state.edit.keyDown, virtualKey);
}

void PrimeEditKeys()
{
    netplay::input::PrimeKeyState(&g_state.edit.keyDown);
}

void EraseLastUtf8Codepoint(std::string& text)
{
    if (text.empty())
    {
        return;
    }

    size_t i = text.size();
    while (i > 0 && (static_cast<unsigned char>(text[i - 1]) & 0xC0u) == 0x80u)
    {
        --i;
    }
    if (i > 0)
    {
        --i;
    }
    text.erase(i);
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

bool IsAllowedEditChar(const Item& item, char c)
{
    const unsigned char uc = static_cast<unsigned char>(c);
    if (item.kind == ItemKind::Integer)
    {
        return c >= '0' && c <= '9';
    }
    return (c >= 32 && c <= 126) || uc >= 0x80u;
}

void AppendEditChar(const Item& item, char c)
{
    if (!g_state.edit.active || !IsAllowedEditChar(item, c))
    {
        return;
    }

    const size_t maxLength = (item.kind == ItemKind::Integer) ? kMaxIntDigits : GetStringEditLimit(item);
    if (maxLength > 0 && g_state.edit.buffer.size() >= maxLength)
    {
        return;
    }

    InsertEditBytes(std::string(1, c));
    ClearEditError();
}

void AppendUtf8Text(const Item& item, const std::string& text)
{
    for (char c : text)
    {
        AppendEditChar(item, c);
    }
}

std::string WcharToUtf8(wchar_t ch)
{
    if (ch == 0)
    {
        return {};
    }

    wchar_t buffer[2] = {ch, L'\0'};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, 1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
    {
        return {};
    }

    std::string utf8(static_cast<size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, buffer, 1, utf8.data(), needed, nullptr, nullptr) <= 0)
    {
        return {};
    }
    return utf8;
}

void DrainWmCharMessages(const Item& item)
{
    if (!g_state.edit.active || item.kind == ItemKind::Integer)
    {
        return;
    }

    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, WM_CHAR, WM_CHAR, PM_REMOVE))
    {
        const wchar_t ch = static_cast<wchar_t>(msg.wParam);
        if (ch < 128)
        {
            continue;
        }
        AppendUtf8Text(item, WcharToUtf8(ch));
    }

    while (PeekMessageW(&msg, nullptr, WM_IME_CHAR, WM_IME_CHAR, PM_REMOVE))
    {
        const wchar_t ch = static_cast<wchar_t>(msg.wParam);
        if (ch < 128)
        {
            continue;
        }
        AppendUtf8Text(item, WcharToUtf8(ch));
    }
}

void BeginEdit(int itemIndex)
{
    if (itemIndex < 0 || itemIndex >= static_cast<int>(g_state.items.size()))
    {
        return;
    }

    const Item& item = g_state.items[static_cast<size_t>(itemIndex)];
    if (!IsTextEditable(item))
    {
        return;
    }

    g_state.edit = {};
    g_state.edit.active = true;
    g_state.edit.itemIndex = itemIndex;
    g_state.edit.buffer = item.currentValue;
    g_state.edit.caretByteOffset = g_state.edit.buffer.size();
    g_state.edit.caretVisible = true;
    g_state.edit.lastCaretTick = GetTickCount();
    PrimeEditKeys();
    ClearEditError();

    mod::Log(
        "OptionsMenu: begin edit section='%s' key='%s' value='%s'",
        item.sectionName.c_str(),
        item.keyName.c_str(),
        item.currentValue.c_str());
}

void CancelEdit()
{
    if (!g_state.edit.active)
    {
        return;
    }
    mod::Log("OptionsMenu: cancel edit item=%d", g_state.edit.itemIndex);
    g_state.edit = {};
}

bool ValidateUnsignedInteger(const std::string& text)
{
    if (text.empty() || text.size() > kMaxIntDigits)
    {
        return false;
    }

    for (char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
    }

    int parsed = 0;
    return netplay::text::ParseIntToken(text, &parsed) && parsed >= 0;
}

bool CommitEdit()
{
    if (!g_state.edit.active || g_state.edit.itemIndex < 0 || g_state.edit.itemIndex >= static_cast<int>(g_state.items.size()))
    {
        return false;
    }

    Item& item = g_state.items[static_cast<size_t>(g_state.edit.itemIndex)];
    std::string value = g_state.edit.buffer;

    if (item.kind == ItemKind::Integer)
    {
        value = netplay::text::TrimAscii(value);
        if (item.keyName == "Port")
        {
            uint16_t port = 0;
            if (!netplay::validation::ParsePort(value, &port))
            {
                SetEditError("Invalid port");
                return false;
            }
        }
        else if (!ValidateUnsignedInteger(value))
        {
            SetEditError("Invalid number");
            return false;
        }
    }
    else if (item.sectionName == "Network" && item.keyName == "Name")
    {
        value = netplay::text::TrimAscii(value);
        if (!netplay::validation::IsValidNickname(value))
        {
            SetEditError("Invalid nickname");
            return false;
        }
    }

    item.currentValue = value;
    mod::Log(
        "OptionsMenu: commit section='%s' key='%s' value='%s'",
        item.sectionName.c_str(),
        item.keyName.c_str(),
        item.currentValue.c_str());
    g_state.edit = {};
    return true;
}

void ToggleValue(Item& item)
{
    switch (item.kind)
    {
    case ItemKind::BoolText:
        item.currentValue = (item.currentValue == "True") ? "False" : "True";
        break;
    case ItemKind::BoolInt:
        item.currentValue = (item.currentValue == "1") ? "0" : "1";
        break;
    case ItemKind::Choice:
    {
        if (item.choiceValues.empty())
        {
            break;
        }

        auto it = std::find(item.choiceValues.begin(), item.choiceValues.end(), item.currentValue);
        if (it == item.choiceValues.end())
        {
            item.currentValue = item.choiceValues.front();
            break;
        }

        ++it;
        if (it == item.choiceValues.end())
        {
            it = item.choiceValues.begin();
        }
        item.currentValue = *it;
        break;
    }
    case ItemKind::Protocol:
        item.currentValue = (item.currentValue == "IPv6") ? "IPv4" : "IPv6";
        break;
    default:
        break;
    }
}

std::string GetEditedDisplayValue(const Item& item)
{
    std::string value = g_state.edit.buffer;
    size_t caret = g_state.edit.caretByteOffset;
    if (value.empty())
    {
        value = item.kind == ItemKind::Integer ? "0" : "";
        if (item.kind == ItemKind::Integer)
        {
            caret = value.size();
        }
    }
    if (g_state.edit.caretVisible)
    {
        value.insert(ClampCaretOffset(value, caret), 1, '_');
    }
    return value;
}

bool HandleEditInput(uint32_t screenContext, const uint8_t* inputBytes, bool* escapeDown)
{
    if (!g_state.edit.active || inputBytes == nullptr)
    {
        return false;
    }

    if (escapeDown != nullptr)
    {
        *escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    }
    (void)inputBytes;

    const int itemIndex = g_state.edit.itemIndex;
    if (itemIndex < 0 || itemIndex >= static_cast<int>(g_state.items.size()))
    {
        g_state.edit = {};
        return true;
    }

    const Item& item = g_state.items[static_cast<size_t>(itemIndex)];
    UpdateCaretBlink();

    if (ConsumeEditKeyEdge(VK_ESCAPE))
    {
        hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
        CancelEdit();
        return true;
    }

    if (ConsumeEditKeyEdge(VK_RETURN))
    {
        if (CommitEdit())
        {
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
        }
        else
        {
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
        }
        return true;
    }

    bool changed = false;
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
            ClearEditError();
            changed = true;
        }
    }
    if (ConsumeEditKeyEdge(VK_DELETE))
    {
        if (EraseCodepointAtCaret())
        {
            ClearEditError();
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
        AppendEditChar(item, c);
        changed = changed || g_state.edit.buffer.size() != before;
    };

    if (ConsumeEditKeyEdge('V'))
    {
        if (netplay::input::IsCtrlPressed())
        {
            const HWND owner = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + netplay::constants::kOffsetWindowHandle));
            std::string clipboardText;
            const bool readOk = (item.kind == ItemKind::Integer)
                ? netplay::input::TryReadClipboardAsciiText(owner, &clipboardText)
                : netplay::input::TryReadClipboardUtf8Text(owner, &clipboardText);
            if (readOk)
            {
                const size_t before = g_state.edit.buffer.size();
                AppendUtf8Text(item, clipboardText);
                changed = changed || g_state.edit.buffer.size() != before;
            }
        }
        else
        {
            char c = 0;
            if (netplay::input::TryTranslateVirtualKeyToAscii('V', &c))
            {
                const size_t before = g_state.edit.buffer.size();
                AppendEditChar(item, c);
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

    DrainWmCharMessages(item);
    return true;
}

bool HandleRebindOverlayInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter, bool* escapeDown)
{
    if (!g_state.modal.active || g_state.modal.kind != ModalKind::Rebind || inactivityCounter == nullptr)
    {
        return false;
    }

    if (escapeDown != nullptr)
    {
        *escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    }

    ++(*inactivityCounter);

    bool cancelRequested = hooks::ConsumeNetplayEscapeEdge();
    cancelRequested = cancelRequested || netplay::input::ConsumeKeyEdge(&g_state.modal.keyDown, VK_ESCAPE);
    if (inputBytes != nullptr)
    {
        for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
        {
            if (inputBytes[playerIndex + 18] == 1)
            {
                cancelRequested = true;
            }
        }
    }

    if (cancelRequested)
    {
        *inactivityCounter = 0;
        hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
        CloseModal();
        return true;
    }

    std::string reboundValue;
    std::string reboundDisplay;
    const bool capturedKeyboard =
        keybinds::TryCaptureKeyboardBinding(&g_state.modal.keyDown, &reboundValue, &reboundDisplay);
    const bool capturedPad =
        !capturedKeyboard
        && keybinds::TryCapturePadBinding(&g_state.modal.padButtonsDown, &reboundValue, &reboundDisplay);
    if (!capturedKeyboard && !capturedPad)
    {
        return true;
    }

    const int itemIndex = g_state.modal.itemIndex;
    if (itemIndex < 0 || itemIndex >= static_cast<int>(g_state.items.size()))
    {
        *inactivityCounter = 0;
        CloseModal();
        return true;
    }

    Item& item = g_state.items[static_cast<size_t>(itemIndex)];
    if (keybinds::NormalizeBindingValue(item.currentValue) == keybinds::NormalizeBindingValue(reboundValue))
    {
        *inactivityCounter = 0;
        hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
        CloseModal();
        SetStatusMessage("Binding unchanged.");
        return true;
    }

    const int conflictIndex = FindBindingConflict(itemIndex, reboundValue);
    if (conflictIndex >= 0)
    {
        *inactivityCounter = 0;
        hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
        g_state.modal.errorMessage =
            reboundDisplay + " is already used by "
            + TruncateLabel(PrettyKeyLabel(g_state.items[static_cast<size_t>(conflictIndex)].keyName), 14)
            + ".";
        return true;
    }

    item.currentValue = reboundValue;
    *inactivityCounter = 0;
    hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
    CloseModal();
    const std::string status = PrettyKeyLabel(item.keyName) + " set to " + reboundDisplay + ".";
    SetStatusMessage(status.c_str());
    mod::Log(
        "OptionsMenu: rebound section='%s' key='%s' value='%s'",
        item.sectionName.c_str(),
        item.keyName.c_str(),
        item.currentValue.c_str());
    return true;
}

bool HandleModalOverlayInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter, bool* escapeDown)
{
    if (!g_state.modal.active || inputBytes == nullptr || inactivityCounter == nullptr)
    {
        return false;
    }

    if (g_state.modal.kind == ModalKind::Rebind)
    {
        return HandleRebindOverlayInput(screenContext, inputBytes, inactivityCounter, escapeDown);
    }

    if (escapeDown != nullptr)
    {
        *escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    }

    bool cancelRequested = hooks::ConsumeNetplayEscapeEdge();
    bool confirmRequested = false;
    const int optionCount = g_state.modal.kind == ModalKind::Exit ? 3 : 2;
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch =
            reinterpret_cast<uint8_t*>(screenContext + netplay::constants::kOffsetInputLatchP1 + playerIndex);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);
        int step = 0;
        if (vertical > 0)
        {
            step = 1;
        }
        else if (vertical < 0)
        {
            step = -1;
        }

        if (step != 0)
        {
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                const int oldSelection = g_state.modal.selectedOption;
                int newSelection = oldSelection + step;
                if (newSelection < 0)
                {
                    newSelection = optionCount - 1;
                }
                if (newSelection >= optionCount)
                {
                    newSelection = 0;
                }
                if (newSelection != oldSelection)
                {
                    g_state.modal.selectedOption = newSelection;
                    hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
                }
                *inputLatch = 1;
            }
        }
        else
        {
            *inputLatch = 0;
        }

        if (inputBytes[playerIndex + 16] == 1)
        {
            confirmRequested = true;
        }
        if (inputBytes[playerIndex + 18] == 1)
        {
            cancelRequested = true;
        }
    }

    ++(*inactivityCounter);

    if (cancelRequested)
    {
        hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
        CloseModal();
        return true;
    }

    if (!confirmRequested)
    {
        return true;
    }

    if (g_state.modal.kind == ModalKind::Save)
    {
        if (g_state.modal.selectedOption == 0)
        {
            if (SaveItemsToDisk())
            {
                hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
                CloseModal();
            }
            else
            {
                hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
                g_state.modal.errorMessage = "Save failed.";
            }
        }
        else
        {
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
            CloseModal();
        }
        return true;
    }

    switch (g_state.modal.selectedOption)
    {
    case 0:
        if (CommitExit(screenContext, true))
        {
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
        }
        else
        {
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
        }
        return true;
    case 1:
        hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
        (void)CommitExit(screenContext, false);
        return true;
    default:
        hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
        CloseModal();
        return true;
    }
}

} // namespace

std::string BuildRowPrimaryText(NetplayMenuAction action)
{
    if (action == NetplayMenuAction::BackToMain)
    {
        return "Back";
    }

    const VisibleEntry visible = GetVisibleEntryForAction(action);
    if (visible.kind == VisibleEntryKind::Category)
    {
        return BuildCategoryLabel(visible.categoryIndex);
    }
    if (visible.kind != VisibleEntryKind::Item
        || visible.itemIndex < 0
        || visible.itemIndex >= static_cast<int>(g_state.items.size()))
    {
        return {};
    }

    return BuildSettingLabel(g_state.items[static_cast<size_t>(visible.itemIndex)]);
}

std::string BuildRowSecondaryText(NetplayMenuAction action)
{
    if (action == NetplayMenuAction::BackToMain)
    {
        return {};
    }

    const VisibleEntry visible = GetVisibleEntryForAction(action);
    if (visible.kind == VisibleEntryKind::Category)
    {
        return ">";
    }
    if (visible.kind != VisibleEntryKind::Item
        || visible.itemIndex < 0
        || visible.itemIndex >= static_cast<int>(g_state.items.size()))
    {
        return {};
    }

    const Item& item = g_state.items[static_cast<size_t>(visible.itemIndex)];
    return (g_state.edit.active && g_state.edit.itemIndex == visible.itemIndex)
        ? GetEditedDisplayValue(item)
        : FormatDisplayValue(item);
}

std::string BuildRowLabel(NetplayMenuAction action)
{
    const std::string primary = BuildRowPrimaryText(action);
    const std::string secondary = BuildRowSecondaryText(action);
    if (secondary.empty())
    {
        return TruncateLabel(primary, 26);
    }

    const std::string separator = (secondary == ">" || secondary == "<") ? " " : ": ";
    return TruncateLabel(primary + separator + secondary, 26);
}

std::string BuildFooterText(NetplayMenuAction selectedAction)
{
    if (!g_state.statusMessage.empty() && GetTickCount() < g_state.statusExpireTick)
    {
        return g_state.statusMessage;
    }

    if (g_state.edit.active && g_state.edit.itemIndex >= 0 && g_state.edit.itemIndex < static_cast<int>(g_state.items.size()))
    {
        const Item& item = g_state.items[static_cast<size_t>(g_state.edit.itemIndex)];
        if (!g_state.edit.errorMessage.empty() && GetTickCount() < g_state.edit.errorExpireTick)
        {
            return g_state.edit.errorMessage + "\nEnter=Save Esc=Cancel";
        }

        std::string actionText = "Enter=Save Esc=Cancel";
        if (item.kind != ItemKind::Integer)
        {
            actionText += " Ctrl+V=Paste";
        }
        return "Edit " + PrettyKeyLabel(item.keyName) + "\n" + actionText;
    }

    if (selectedAction == NetplayMenuAction::BackToMain)
    {
        if (IsRootCategoryView())
        {
            if (HasUnsavedChanges())
            {
                return "Exit the options menu.\nA/B=Prompt C=Revert D=Save";
            }
            return "Exit the options menu.\nA/B=Exit C=Revert D=Save";
        }
        return "Return to category list.\nA/B=Back C=Revert D=Save";
    }

    const VisibleEntry visible = GetVisibleEntryForAction(selectedAction);
    if (visible.kind == VisibleEntryKind::Category)
    {
        std::string help;
        if (IsValidCategoryIndex(visible.categoryIndex))
        {
            help = g_state.categories[static_cast<size_t>(visible.categoryIndex)].tooltipSummary;
        }
        if (help.empty())
        {
            help = "Open this category.";
        }
        return help + "\nA=Open C=Revert D=Save";
    }

    if (visible.kind != VisibleEntryKind::Item
        || visible.itemIndex < 0
        || visible.itemIndex >= static_cast<int>(g_state.items.size()))
    {
        return {};
    }

    const Item& item = g_state.items[static_cast<size_t>(visible.itemIndex)];
    std::string help = item.tooltipSummary;
    if (help.empty())
    {
        help = "Adjust this setting.";
    }
    if (IsKeyBindingEditable(item))
    {
        return help + " Pad bindings only work when that controller is assigned to a player.\nA=Rebind C=Revert D=Save";
    }
    if (IsToggleEditable(item))
    {
        return help + "\nA=Change C=Revert D=Save";
    }
    return help + "\nA=Edit C=Revert D=Save";
}

bool HandleVerticalNavigation(int currentSelection, int delta, int* outNextSelection)
{
    if (outNextSelection == nullptr)
    {
        return false;
    }

    const int visibleCount = GetVisibleContentCount();
    const int backSelection = visibleCount;
    if (visibleCount <= 0)
    {
        *outNextSelection = 0;
        return true;
    }

    if (delta > 0)
    {
        if (currentSelection < visibleCount - 1)
        {
            *outNextSelection = currentSelection + 1;
            return true;
        }
        if (currentSelection == visibleCount - 1)
        {
            const int contentCount = GetCurrentContentCount();
            const int maxScroll = (std::max)(0, contentCount - visibleCount);
            if (g_state.scrollOffset < maxScroll)
            {
                ++g_state.scrollOffset;
                RebuildMenuEntries();
                *outNextSelection = currentSelection;
            }
            else
            {
                *outNextSelection = backSelection;
            }
            return true;
        }
        *outNextSelection = 0;
        return true;
    }

    if (currentSelection > 0 && currentSelection <= backSelection)
    {
        *outNextSelection = currentSelection - 1;
        return true;
    }

    if (g_state.scrollOffset > 0)
    {
        --g_state.scrollOffset;
        RebuildMenuEntries();
        *outNextSelection = currentSelection;
    }
    else
    {
        *outNextSelection = backSelection;
    }
    return true;
}

bool HandleInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter, bool* escapeDown)
{
    if (hooks::g_netplayMenuState.menuId != NetplayMenuId::Options || inputBytes == nullptr || inactivityCounter == nullptr)
    {
        return false;
    }

    if (HandleEditInput(screenContext, inputBytes, escapeDown))
    {
        *inactivityCounter = 0;
        return true;
    }

    if (HandleModalOverlayInput(screenContext, inputBytes, inactivityCounter, escapeDown))
    {
        return true;
    }

    const bool escapePressed = hooks::ConsumeNetplayEscapeEdge();
    bool cancelPressed = escapePressed;
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch =
            reinterpret_cast<uint8_t*>(screenContext + netplay::constants::kOffsetInputLatchP1 + playerIndex);
        const int8_t horizontal = static_cast<int8_t>(inputBytes[playerIndex + 12]);
        if (horizontal != 0 && !IsRootCategoryView() && GetCurrentContentCount() > kVisibleRowCount)
        {
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                const int pageSize = kVisibleRowCount;
                const int maxScroll = (std::max)(0, GetCurrentContentCount() - pageSize);
                const int currentPageStart = (g_state.scrollOffset / pageSize) * pageSize;
                int nextScroll = currentPageStart;
                if (horizontal > 0)
                {
                    nextScroll = (std::min)(maxScroll, currentPageStart + pageSize);
                }
                else
                {
                    nextScroll = (std::max)(0, currentPageStart - pageSize);
                }

                if (nextScroll != g_state.scrollOffset)
                {
                    const int currentSelection =
                        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection));
                    g_state.scrollOffset = nextScroll;
                    RebuildMenuEntries();
                    SetSelection(screenContext, (std::min)(currentSelection, GetVisibleContentCount() - 1));
                    hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
                    mod::Log(
                        "OptionsMenu: page %s scroll=%d",
                        horizontal > 0 ? "forward" : "back",
                        g_state.scrollOffset);
                }
                *inputLatch = 1;
            }
            return true;
        }

        if (inputBytes[playerIndex + 20] == 1)
        {
            for (Item& item : g_state.items)
            {
                item.currentValue = item.originalValue;
            }
            SetStatusMessage("Changes reverted.");
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
            *inactivityCounter = 0;
            return true;
        }

        if (inputBytes[playerIndex + 22] == 1)
        {
            OpenModal(ModalKind::Save);
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
            *inactivityCounter = 0;
            return true;
        }

        if (inputBytes[playerIndex + 18] == 1)
        {
            cancelPressed = true;
        }
    }

    if (cancelPressed)
    {
        hooks::PlayUiSound(screenContext, netplay::constants::kSfxConfirm);
        *inactivityCounter = 0;
        if (IsRootCategoryView())
        {
            RequestExitOptionsMenu(screenContext);
        }
        else
        {
            ReturnToCategoryRoot(screenContext, g_state.currentCategoryIndex);
        }
        return true;
    }

    return false;
}

bool ExecuteAction(uint32_t screenContext, NetplayMenuAction action)
{
    if (action == NetplayMenuAction::BackToMain)
    {
        if (IsRootCategoryView())
        {
            RequestExitOptionsMenu(screenContext);
        }
        else
        {
            ReturnToCategoryRoot(screenContext, g_state.currentCategoryIndex);
        }
        return true;
    }

    if (!IsVisibleRowAction(action))
    {
        return false;
    }

    const VisibleEntry visible = GetVisibleEntryForAction(action);
    if (visible.kind == VisibleEntryKind::Category)
    {
        EnterCategoryView(screenContext, visible.categoryIndex);
        return true;
    }

    if (visible.kind != VisibleEntryKind::Item
        || visible.itemIndex < 0
        || visible.itemIndex >= static_cast<int>(g_state.items.size()))
    {
        return true;
    }

    Item& item = g_state.items[static_cast<size_t>(visible.itemIndex)];

    if (IsToggleEditable(item))
    {
        ToggleValue(item);
        mod::Log(
            "OptionsMenu: toggled section='%s' key='%s' value='%s'",
            item.sectionName.c_str(),
            item.keyName.c_str(),
            item.currentValue.c_str());
        return true;
    }

    if (IsKeyBindingEditable(item))
    {
        OpenRebindModal(visible.itemIndex);
        return true;
    }

    if (IsTextEditable(item))
    {
        BeginEdit(visible.itemIndex);
        return true;
    }

    (void)screenContext;
    return true;
}

bool DrawSaveOverlayGdi(uint32_t screenContext, bool /*allowWindowDc*/)
{
    if (!g_state.modal.active)
    {
        return false;
    }

    netplay::draw::LockedMenuSurface lockedSurface;
    if (!netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        return false;
    }

    const netplay::font::IndexedSurfaceView sv = {
        lockedSurface.pixels,
        lockedSurface.width,
        lockedSurface.height,
        lockedSurface.pitch,
    };

    const uint8_t bgColor = netplay::draw::ResolveBestPaletteColor(screenContext, 8, 16, 28);
    const uint8_t frameColor = netplay::draw::ResolveBestPaletteColor(screenContext, 96, 210, 200);
    const uint8_t titleColor = netplay::draw::ResolveBestPaletteColor(screenContext, 220, 245, 245);
    const uint8_t textColor = netplay::draw::ResolveBestPaletteColor(screenContext, 180, 208, 208);
    const uint8_t brightColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 255, 255);
    const uint8_t dimColor = netplay::draw::ResolveBestPaletteColor(screenContext, 160, 180, 180);
    const uint8_t hlColor = netplay::draw::ResolveBestPaletteColor(screenContext, 40, 100, 96);
    const uint8_t errorColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 130, 130);

    const bool isExitModal = (g_state.modal.kind == ModalKind::Exit);
    const bool isRebindModal = (g_state.modal.kind == ModalKind::Rebind);
    const int optionCount = isExitModal ? 3 : 2;
    constexpr int panelW = 214;
    const int panelH = isRebindModal ? 92 : (isExitModal ? 100 : 84);
    constexpr int panelX = (320 - panelW) / 2;
    const int panelY = (240 - panelH) / 2;
    netplay::font::FillIndexedSurfaceRect(sv, panelX, panelY, panelW, panelH, bgColor);
    netplay::font::DrawIndexedSurfaceFrame(sv, panelX, panelY, panelW, panelH, frameColor);

    const int textLeft = panelX + 6;
    const int textRight = panelX + panelW - 6;
    auto drawModalText = [&](const std::string& line, int left, int right, int y, uint8_t color)
    {
        const int availableWidth = right - left;
        if (availableWidth <= 0)
        {
            return;
        }

        const int textWidth = netplay::font::MeasureText5x7Width(line, 1);
        bool hasNonAscii = false;
        for (unsigned char c : line)
        {
            if ((c & 0x80u) != 0)
            {
                hasNonAscii = true;
                break;
            }
        }

        if (textWidth <= availableWidth || hasNonAscii)
        {
            netplay::font::DrawTextCentered5x7(sv, line, left, right, y, 1, 1, color);
            return;
        }

        constexpr DWORD kScrollStepMs = 180;
        constexpr size_t kPadChars = 6;
        const size_t visibleChars = (std::max)(static_cast<size_t>(1), static_cast<size_t>(availableWidth / 6));
        const std::string spacer(kPadChars, ' ');
        const std::string marquee = line + spacer + line + spacer;
        const size_t cycle = line.size() + spacer.size();
        const size_t start = (GetTickCount() / kScrollStepMs) % cycle;
        const std::string window = marquee.substr(start, (std::min)(visibleChars + 2, marquee.size() - start));
        netplay::font::DrawTextCentered5x7(sv, window, left, right, y, 1, 1, color);
    };

    if (isRebindModal)
    {
        std::string keyLabel = "Option";
        std::string currentValue = "(empty)";
        if (g_state.modal.itemIndex >= 0 && g_state.modal.itemIndex < static_cast<int>(g_state.items.size()))
        {
            const Item& item = g_state.items[static_cast<size_t>(g_state.modal.itemIndex)];
            keyLabel = PrettyKeyLabel(item.keyName);
            currentValue = FormatDisplayValue(item);
        }

        drawModalText("REBIND KEY", textLeft, textRight, panelY + 6, titleColor);
        drawModalText(keyLabel, textLeft, textRight, panelY + 22, brightColor);
        drawModalText("Press a key or pad button to rebind...", textLeft, textRight, panelY + 38, textColor);
        drawModalText("Current: " + currentValue, textLeft, textRight, panelY + 52, dimColor);
        drawModalText("Esc/B=Cancel", textLeft, textRight, panelY + 66, dimColor);
    }
    else
    {
        drawModalText(isExitModal ? "EXIT OPTIONS?" : "SAVE CHANGES?", textLeft, textRight, panelY + 6, titleColor);
        drawModalText(isExitModal ? "Save before returning?" : "Write EfzRevival.ini?", textLeft, textRight, panelY + 24, textColor);

        const std::array<const char*, 3> labels = {"Yes", "No", "Cancel"};
        const int baseY = panelY + 42;
        for (int optionIndex = 0; optionIndex < optionCount; ++optionIndex)
        {
            const bool isSelected = (g_state.modal.selectedOption == optionIndex);
            const int rowY = baseY + optionIndex * 16;
            if (isSelected)
            {
                netplay::font::FillIndexedSurfaceRect(sv, panelX + 38, rowY, 138, 12, hlColor);
            }
            drawModalText(
                std::string(isSelected ? "> " : "  ") + labels[static_cast<size_t>(optionIndex)],
                panelX + 38,
                panelX + panelW - 38,
                rowY + 2,
                isSelected ? brightColor : dimColor);
        }
    }

    if (!g_state.modal.errorMessage.empty())
    {
        drawModalText(g_state.modal.errorMessage, textLeft, textRight, panelY + panelH - 10, errorColor);
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

bool IsSaveOverlayActive()
{
    return g_state.modal.active;
}

bool IsBusy()
{
    return g_state.edit.active || g_state.modal.active;
}

bool UseTournamentModeForOfflineVsHuman()
{
    return g_otherOfflineVsMode != "VS Human";
}
} // namespace netplay::options
