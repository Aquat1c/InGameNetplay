#include "netplay/core/battle_log_menu.h"

#include "efz_netplay_state.h"
#include "logger.h"
#include "netplay/assets/assets.h"
#include "netplay/core/constants.h"
#include "netplay/core/input_utils.h"
#include "netplay/core/mod_settings.h"
#include "netplay/core/text_utils.h"
#include "netplay/hooks/debug_overlay.h"
#include "netplay/hooks/internal/shared.h"
#include "netplay/render/draw_surface.h"
#include "netplay/render/software_font.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <d3d9.h>
#include <fstream>
#if defined(EFZ_XP_COMPAT)
using std::max;
using std::min;
#endif
#include <gdiplus.h>
#include <iterator>
#include <map>
#include <MinHook.h>
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
constexpr int kFilterRowH = 12;
constexpr int kDetailRowH = 11;
constexpr int kContentColumnX = kRowX;
constexpr int kContentColumnW = kRowW;
constexpr int kActionStackX = 120;
constexpr int kActionStackW = 80;
constexpr int kSummaryCardX = 12;
constexpr int kSummaryCardY = 80;
constexpr int kSummaryCardW = 296;
constexpr int kSummaryCardH = 108;
constexpr int kSummaryActionPanelX = 12;
constexpr int kSummaryActionPanelY = 190;
constexpr int kSummaryActionPanelW = 296;
constexpr int kSummaryActionPanelH = 28;
constexpr int kSummaryActionButtonW = 52;
constexpr int kSummaryActionButtonH = 12;
constexpr int kSummaryActionButtonGap = 4;
constexpr int kBrowserHeaderPanelX = 8;
constexpr int kBrowserHeaderPanelY = 20;
constexpr int kBrowserHeaderPanelW = 304;
constexpr int kBrowserHeaderPanelH = 68;
constexpr int kBrowserContentPanelY = 92;
constexpr int kBrowserActionPanelX = 12;
constexpr int kBrowserActionPanelY = 180;
constexpr int kBrowserActionPanelW = 296;
constexpr int kBrowserActionPanelH = 28;
constexpr int kBrowserActionButtonW = 64;
constexpr int kBrowserActionButtonH = 12;
constexpr int kBrowserActionButtonGap = 6;
constexpr int kDetailHeaderPanelX = 8;
constexpr int kDetailHeaderPanelY = 16;
constexpr int kDetailHeaderPanelW = 304;
constexpr int kDetailHeaderPanelH = 58;
constexpr int kDetailContentPanelY = 78;
constexpr int kDetailContentPanelH = 88;
constexpr int kDetailActionPanelX = 12;
constexpr int kDetailActionPanelY = 178;
constexpr int kDetailActionPanelW = 296;
constexpr int kDetailActionPanelH = 28;
constexpr int kDetailActionButtonW = 88;
constexpr int kDetailActionButtonH = 12;
constexpr int kDetailActionButtonGap = 8;
constexpr int kDetailHeaderIconSize = 18;
constexpr int kDetailRowIconSize = 10;
constexpr DWORD kStatusDisplayMs = 2200;
constexpr DWORD kCaretBlinkMs = 350;
constexpr size_t kMaxFilterTextBytes = 127;

constexpr std::array<int, 6> kBrowserSessionRowY = {94, 107, 120, 133, 146, 159};
constexpr std::array<int, 7> kFilterFieldRowY = {76, 89, 102, 115, 128, 141, 154};
constexpr std::array<int, 3> kFilterActionRowY = {168, 181, 194};
constexpr std::array<int, 7> kDetailGameRowY = {82, 94, 106, 118, 130, 142, 154};

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

enum class SummaryMode : uint8_t
{
    Profile = 0,
    FullLog,
    Search,
};

enum class FilterEditField : uint8_t
{
    None = 0,
    PlayerName,
    OpponentName,
};

enum class SetStatusMode : uint8_t
{
    All = 0,
    Played,
    Empty,
};

enum class GameCountFilterMode : uint8_t
{
    Any = 0,
    One,
    TwoToThree,
    FourPlus,
};

enum class SwitchFilterMode : uint8_t
{
    Any = 0,
    Stable,
    Swapped,
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
    BattleLogSummary nicknameSummary = {};
    BattleLogSummary fullSummary = {};
    BattleLogSummary searchSummary = {};
    BattleLogSummary browserSummary = {};
    BattleLogFilter activeFilter = {};
    BattleLogFilter draftFilter = {};
    View view = View::Summary;
    View filterReturnView = View::Summary;
    SummaryMode summaryMode = SummaryMode::Profile;
    int summarySelection = 0;
    int browserSelection = 0;
    int filtersSelection = 0;
    int detailSelection = 0;
    int browserListSelection = 0;
    int browserActionSelection = netplay::menu::kBattleLogVisibleSessionRows;
    int filtersFieldSelection = 0;
    int filtersActionSelection = 7;
    int detailListSelection = 0;
    int detailActionSelection = netplay::menu::kBattleLogVisibleGameRows;
    int browserPage = 0;
    int detailPage = 0;
    int detailSessionIndex = -1;
    std::vector<int> filteredSessionIndices;
    std::string currentNickname = "Player";
    std::string statusMessage;
    DWORD statusExpireTick = 0;
    FilterEditState edit = {};
    std::array<int8_t, 2> lastHorizontalDir = {};
    std::array<int8_t, 2> lastVerticalDir = {};
    std::array<uint8_t, 2> lastButtonC = {};
    std::array<NetplayMenuEntry, 10> entries = {};
    NetplayMenuSpec spec = {};
};

State g_state = {};

struct SpriteBitmap
{
    std::string path;
    Gdiplus::Bitmap* bitmap = nullptr;
    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> bgraPixels;
    IDirect3DTexture9* d3dTexture = nullptr;
};

struct RenderAssetState
{
    std::string resolvedSpriteDirectory;
    bool spriteDirectoryResolved = false;
    bool loadAttempted = false;
    bool iconsReady = false;
    bool firstSpriteBlitLogged = false;
    bool gdiplusStarted = false;
    ULONG_PTR gdiplusToken = 0;
    std::map<std::string, SpriteBitmap> sprites;
};

RenderAssetState g_renderAssets = {};

using EndSceneFn = HRESULT(WINAPI*)(LPDIRECT3DDEVICE9);

struct D3dOverlayState
{
    bool hookAttempted = false;
    bool hookInstalled = false;
    bool minhookInitialized = false;
    bool endSceneObserved = false;
    bool firstD3dSpriteBlitLogged = false;
    bool firstAcceptedRenderTargetLogged = false;
    bool firstRejectedRenderTargetLogged = false;
    bool firstNullDeviceLogged = false;
    bool firstInactiveMenuSkipLogged = false;
    bool firstWrongMenuSkipLogged = false;
    bool firstWrongViewSkipLogged = false;
    bool firstIconsNotReadySkipLogged = false;
    bool firstGetRenderTargetFailureLogged = false;
    bool firstGetRenderTargetDescFailureLogged = false;
    bool firstTextureReadyFailureLogged = false;
    bool firstStateBlockFailureLogged = false;
    bool firstNoDrawableIconsLogged = false;
    bool firstMissingSessionSpriteLogged = false;
    bool firstHookUnavailableLogged = false;
    bool firstGetViewportFailureLogged = false;
    bool firstGetSwapChainFailureLogged = false;
    bool firstGetPresentParametersFailureLogged = false;
    uint32_t targetTraceLogsRemaining = 24;
    void* endSceneTarget = nullptr;
    EndSceneFn originalEndScene = nullptr;
    IDirect3DDevice9* textureDevice = nullptr;
};

D3dOverlayState g_d3dOverlay = {};

enum class SpriteSlotAlignment : uint8_t
{
    Center = 0,
    Left,
    Right,
};

constexpr int kBrowserIconSlotSize = 10;
constexpr int kBrowserIconGap = 1;
constexpr int kBrowserIconSlotsPerPlayer = 3;
constexpr int kBrowserDateGap = 4;
constexpr int kBrowserIconNameGap = 1;
constexpr int kBrowserScoreGap = 3;
constexpr int kDetailLabelGap = 3;
constexpr int kDetailIconNameGap = 1;
constexpr int kDetailScoreGap = 2;
constexpr int kDetailDurationGap = 3;

struct BrowserRowLayout
{
    std::string dateTimeText;
    std::string leftNameText;
    std::string scoreText;
    std::string rightNameText;
    int dateLeft = 0;
    int dateRight = 0;
    int p1IconsX = 0;
    int p1IconCount = 0;
    int p1NameLeft = 0;
    int p1NameRight = 0;
    int scoreLeft = 0;
    int scoreRight = 0;
    int p2NameLeft = 0;
    int p2NameRight = 0;
    int p2IconsX = 0;
    int p2IconCount = 0;
};

struct DetailRowLayout
{
    std::string labelText;
    std::string leftNameText;
    std::string roundsText;
    std::string rightNameText;
    std::string durationText;
    int labelLeft = 0;
    int labelRight = 0;
    int p1IconX = 0;
    bool p1HasIcon = false;
    int p1NameLeft = 0;
    int p1NameRight = 0;
    int roundsLeft = 0;
    int roundsRight = 0;
    int p2NameLeft = 0;
    int p2NameRight = 0;
    int p2IconX = 0;
    bool p2HasIcon = false;
    int durationLeft = 0;
    int durationRight = 0;
};

struct CharacterSpriteFile
{
    const char* canonicalName;
    const char* fileName;
};

constexpr std::array<CharacterSpriteFile, 23> kCharacterSpriteFiles = {{
    {"Akane", "akane.png"},
    {"Akiko", "akiko.png"},
    {"Ayu", "ayu.png"},
    {"Doppel", "doppel.png"},
    {"Ikumi", "ikumi.png"},
    {"Kanna", "kanna.png"},
    {"Kano", "kano.png"},
    {"Kaori", "kaori.png"},
    {"Mai", "mai.png"},
    {"Makoto", "makoto.png"},
    {"Mayu", "mayu.png"},
    {"Minagi", "minagi.png"},
    {"Mio", "mio.png"},
    {"Misaki", "misaki.png"},
    {"Mishio", "mishio.png"},
    {"Misuzu", "misuzu.png"},
    {"Nagamori", "nagamori.png"},
    {"Nayuki (Awake)", "nayuki_awake.png"},
    {"Nayuki (Sleepy)", "nayuki_sleepy.png"},
    {"Rumi", "rumi.png"},
    {"Sayuri", "sayuri.png"},
    {"Shiori", "shiori.png"},
    {"Unknown", "unknown.png"},
}};

// Parsing and document helpers.
std::wstring TrimWide(std::wstring_view value);
std::string WideToUtf8(const std::wstring& wide);
std::wstring Utf8ToWide(const std::string& utf8);
bool DecodeUtf8(const std::vector<uint8_t>& bytes, size_t offset, std::wstring* outWide);
bool DecodeAnsi(const std::vector<uint8_t>& bytes, size_t offset, std::wstring* outWide);
bool ReadWideTextFile(const std::string& path, std::wstring* outText, FileEncoding* outEncoding);
std::vector<std::wstring> SplitLines(const std::wstring& text);
bool IsAbsolutePath(const std::string& path);
bool DirectoryExists(const std::string& path);
std::wstring AnsiPathToWide(const std::string& path);
std::string GetExecutableDirectory();
std::string ResolveRevivalIniPath();
std::string ResolveBattleLogPathFromIni(bool* outSaveEnabled);
bool ParseLeadingDateTime(std::string_view line, std::string* outDate, std::string* outTime, size_t* outTailOffset);
bool SplitVsPair(std::string_view text, std::string* outLeft, std::string* outRight);
bool ParseTwoInts(std::string_view text, int* outLeft, int* outRight);
bool ParseDurationField(std::string_view text, int* outTotalSeconds);
bool LooksLikeMatchRow(std::string_view line);
std::string ResolveBattleLogSpriteDirectory();
void PrimeRenderAssetDiagnostics();
bool EnsureGdiplusStarted();
bool EnsureRenderAssetsLoaded(uint32_t screenContext);
bool EnsureD3d9OverlayHookInstalled();
void ShutdownD3d9OverlayHook();
void ResetRenderAssetFrameState();
void ReleaseRenderAssets();
void ReleaseD3dTextures();
std::string CanonicalizeCharacterNameForSpriteLookup(std::string name);
const SpriteBitmap* FindCharacterSprite(const std::string& name);
std::vector<std::string> BuildSessionIconCharacters(const BattleLogSession& session, bool playerOne);
bool EnsureD3dTexturesReady(LPDIRECT3DDEVICE9 device);
bool RenderBrowserIconsD3d9(LPDIRECT3DDEVICE9 device);
HRESULT WINAPI HookedBattleLogEndScene(LPDIRECT3DDEVICE9 device);
void ComputeScaledSpriteRect(
    const SpriteBitmap& sprite,
    int slotLeft,
    int slotTop,
    int slotW,
    int slotH,
    SpriteSlotAlignment alignment,
    int* outDrawX,
    int* outDrawY,
    int* outDrawW,
    int* outDrawH);
void DrawScaledSpriteToSurface(
    const netplay::font::IndexedSurfaceView& surface,
    uint32_t screenContext,
    const SpriteBitmap& sprite,
    int slotX,
    int slotY,
    int slotW,
    int slotH,
    std::map<uint32_t, uint8_t>* paletteCache);
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
void LogParseDiagnostic(const std::string& path, int lineNumber, const char* message);
void UpdateDerivedSummaryStats(BattleLogSummary* summary, const BattleLogSession& session);
void FinalizeDerivedSummaryStats(BattleLogSummary* summary);
bool ParseHeaderLine(const std::string& line, int sessionIndex, int lineNumber, BattleLogSession* outSession);
bool ParseMatchLine(const std::string& line, int matchIndex, int lineNumber, BattleLogMatch* outMatch);
BattleLogDocument ParseBattleLogDocument(const std::string& path, bool saveEnabled);
bool IsAllCharacterValue(const std::string& value);
bool StringEqualsTrimmed(const std::string& left, const std::string& right);
bool MatchOrientationNames(const BattleLogSession& session, const BattleLogFilter& filter, int playerSide);
bool MatchOrientationCharacters(const BattleLogMatch& match, const BattleLogFilter& filter, int playerSide);
bool MatchSetStatus(const BattleLogSession& session, const BattleLogFilter& filter);
bool MatchGameCount(const BattleLogSession& session, const BattleLogFilter& filter);
bool MatchCharacterSwitches(const BattleLogSession& session, const BattleLogFilter& filter);
bool SessionMatchesFilter(const BattleLogSession& session, const BattleLogFilter& filter);
std::string TrimUtf8(const std::string& text);
BattleLogSummary BuildSummaryForNickname(const BattleLogDocument& document, const std::string& nickname);
BattleLogSummary BuildSummaryForDocument(const BattleLogDocument& document);
BattleLogSummary BuildSummaryForFilteredSessions(const BattleLogDocument& document, const std::vector<int>& sessionIndices, const BattleLogFilter& filter);
bool IsDefaultMineFilter(const BattleLogFilter& filter);
bool IsDefaultAllFilter(const BattleLogFilter& filter);
bool HasSearchSummaryFilter(const BattleLogFilter& filter);
void RefreshDisplayedSummary();

// State and navigation helpers.
void SetStatusMessage(const char* text);
bool HasStatusMessage();
void ClearStatusMessage();
int* GetSelectionStorage(View view);
const char* GetHeaderLabel();
int GetCurrentContentCount();
int GetBrowserSelectableSessionCount();
int GetDetailSelectableGameCount();
int ClampSelectionForCurrentView(int selection);
void RememberGroupedSelection(View view, int selection);
bool TogglePaneSelection(int currentSelection, int* outNextSelection);
void SetSelection(uint32_t screenContext, int selection);
int GetBrowserResultCount();
int GetBrowserPageCount();
int GetDetailPageCount();
const BattleLogSession* GetSessionByIndex(int sessionIndex);
void NormalizeFilter(BattleLogFilter* filter);
bool HasCharacterOption(const std::string& value);
void SanitizeFilterCharacters(BattleLogFilter* filter);
SetStatusMode ParseSetStatusMode(const std::string& value);
GameCountFilterMode ParseGameCountFilterMode(const std::string& value);
SwitchFilterMode ParseSwitchFilterMode(const std::string& value);
const std::array<const char*, 3>& GetSetStatusOptions();
const std::array<const char*, 4>& GetGameCountFilterOptions();
const std::array<const char*, 3>& GetSwitchFilterOptions();
template <size_t N>
void CycleOptionValue(std::string* value, const std::array<const char*, N>& options, int delta);
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
std::string BuildSummaryTitleText();
std::string BuildSummaryIdentityText();
std::string BuildSummaryLoadText();
std::string BuildSummarySetRateText();
std::string BuildSummaryGameRateText();
std::string BuildSummaryAverageGamesText();
std::string BuildSummaryAverageSetDurationText();
std::string BuildSummaryLongestSetText();
std::string BuildSummaryCompletionRatioText();
std::string BuildBrowserRecordLeftText();
std::string BuildBrowserRecordRightText();
std::string BuildBrowserUsageLeftText();
std::string BuildBrowserUsageRightText();
std::string BuildBrowserLongestSetText();
int FindCharacterOptionIndex(const std::string& value);
void CycleCharacterOption(std::string* value, int delta);
std::string GetDefaultBattleLogSetStatus();

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
void DrawRowBoxAt(const netplay::font::IndexedSurfaceView& surface, int x, int y, int w, int h, bool selected, uint8_t fillColor, uint8_t frameColor, uint8_t selectedFillColor);
int DrawChip(const netplay::font::IndexedSurfaceView& surface, const std::string& label, int x, int y, uint8_t fillColor, uint8_t frameColor, uint8_t textColor, bool switched);
void DrawSummaryPanel(const netplay::font::IndexedSurfaceView& surface, uint8_t panelFill, uint8_t panelFrame, uint8_t titleColor, uint8_t textColor, uint8_t dimColor, uint8_t alertColor);
void DrawBrowserPanel(const netplay::font::IndexedSurfaceView& surface, uint8_t panelFill, uint8_t panelFrame, uint8_t titleColor, uint8_t textColor, uint8_t dimColor, uint8_t alertColor);
void DrawFiltersPanel(const netplay::font::IndexedSurfaceView& surface, uint8_t panelFill, uint8_t panelFrame, uint8_t titleColor, uint8_t textColor, uint8_t dimColor);
void DrawDetailPanel(const netplay::font::IndexedSurfaceView& surface, uint8_t panelFill, uint8_t panelFrame, uint8_t titleColor, uint8_t textColor, uint8_t dimColor, uint8_t alertColor);
void DrawScreenTitleBar(const netplay::font::IndexedSurfaceView& surface, uint8_t fillColor, uint8_t frameColor, uint8_t titleColor, uint8_t dimColor);
void DrawSummaryRows(const netplay::font::IndexedSurfaceView& surface, int selection, uint8_t rowFill, uint8_t rowFrame, uint8_t selectedFill, uint8_t normalText, uint8_t selectedText);
void DrawBrowserRows(const netplay::font::IndexedSurfaceView& surface, uint32_t screenContext, int selection, uint8_t rowFill, uint8_t rowFrame, uint8_t selectedFill, uint8_t normalText, uint8_t selectedText, uint8_t chipFill, uint8_t chipFrame, uint8_t chipText, uint8_t dimText, uint8_t alertColor);
void DrawFilterRows(const netplay::font::IndexedSurfaceView& surface, int selection, uint8_t rowFill, uint8_t rowFrame, uint8_t selectedFill, uint8_t normalText, uint8_t selectedText, uint8_t dimText);
void DrawDetailRows(const netplay::font::IndexedSurfaceView& surface, uint32_t screenContext, int selection, uint8_t rowFill, uint8_t rowFrame, uint8_t selectedFill, uint8_t normalText, uint8_t selectedText, uint8_t chipFill, uint8_t chipFrame, uint8_t chipText, uint8_t dimText);

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

bool DirectoryExists(const std::string& path)
{
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring AnsiPathToWide(const std::string& path)
{
    if (path.empty())
    {
        return {};
    }

    const int required = MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, nullptr, 0);
    if (required <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, path.c_str(), -1, wide.data(), required) <= 0)
    {
        return {};
    }
    wide.resize(static_cast<size_t>(required - 1));
    return wide;
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

std::string ResolveBattleLogSpriteDirectory()
{
    mod::Log("BattleLog::ResolveSpriteDirectory: searching for assets\\sprites");

    const std::string moduleDir = hooks::g_moduleDirectory;
    if (!moduleDir.empty())
    {
        const std::string candidate = netplay::assets::JoinPath(moduleDir, "assets\\sprites");
        mod::Log("BattleLog::ResolveSpriteDirectory: [DLL dir] probing '%s'", candidate.c_str());
        if (DirectoryExists(candidate))
        {
            mod::Log("BattleLog::ResolveSpriteDirectory: using '%s'", candidate.c_str());
            return candidate;
        }
    }

    const std::string exeDir = GetExecutableDirectory();
    if (!exeDir.empty())
    {
        const std::string modsCandidate = exeDir + "\\mods\\efz_netplay_mod\\assets\\sprites";
        mod::Log("BattleLog::ResolveSpriteDirectory: [EXE mods dir] probing '%s'", modsCandidate.c_str());
        if (DirectoryExists(modsCandidate))
        {
            mod::Log("BattleLog::ResolveSpriteDirectory: using '%s'", modsCandidate.c_str());
            return modsCandidate;
        }

        const std::string assetsCandidate = exeDir + "\\assets\\sprites";
        mod::Log("BattleLog::ResolveSpriteDirectory: [EXE assets dir] probing '%s'", assetsCandidate.c_str());
        if (DirectoryExists(assetsCandidate))
        {
            mod::Log("BattleLog::ResolveSpriteDirectory: using '%s'", assetsCandidate.c_str());
            return assetsCandidate;
        }
    }

    mod::Log("BattleLog::ResolveSpriteDirectory: no sprite directory found");
    return {};
}

std::string CanonicalizeCharacterNameForSpriteLookup(std::string name)
{
    name = NormalizeCharacterToken(std::move(name));
    name = netplay::text::TrimAscii(std::move(name));
    if (_stricmp(name.c_str(), "BossUnknown") == 0 || _stricmp(name.c_str(), "UNKNOWN") == 0)
    {
        return "Unknown";
    }
    return name;
}

void PrimeRenderAssetDiagnostics()
{
    ReleaseRenderAssets();
    g_renderAssets.resolvedSpriteDirectory = ResolveBattleLogSpriteDirectory();
    g_renderAssets.spriteDirectoryResolved = !g_renderAssets.resolvedSpriteDirectory.empty();
    g_renderAssets.loadAttempted = false;
    g_renderAssets.iconsReady = false;
    g_renderAssets.firstSpriteBlitLogged = false;
    if (!g_renderAssets.spriteDirectoryResolved)
    {
        mod::Log("BattleLog::PrimeRenderAssetDiagnostics: sprite directory unavailable");
        return;
    }

    int foundFiles = 0;
    for (const auto& entry : kCharacterSpriteFiles)
    {
        const std::string candidate =
            netplay::assets::JoinPath(g_renderAssets.resolvedSpriteDirectory, entry.fileName);
        if (netplay::assets::FileExists(candidate))
        {
            ++foundFiles;
        }
    }

    mod::Log(
        "BattleLog::PrimeRenderAssetDiagnostics: spriteDir='%s' found=%d/%zu",
        g_renderAssets.resolvedSpriteDirectory.c_str(),
        foundFiles,
        kCharacterSpriteFiles.size());
}

bool EnsureGdiplusStarted()
{
    if (g_renderAssets.gdiplusStarted)
    {
        return true;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    const Gdiplus::Status status = Gdiplus::GdiplusStartup(
        &g_renderAssets.gdiplusToken,
        &startupInput,
        nullptr);
    if (status != Gdiplus::Ok)
    {
        mod::Log("BattleLog::EnsureGdiplusStarted: startup failed status=%d", static_cast<int>(status));
        g_renderAssets.gdiplusToken = 0;
        return false;
    }

    g_renderAssets.gdiplusStarted = true;
    mod::Log("BattleLog::EnsureGdiplusStarted: startup OK token=%p", reinterpret_cast<void*>(g_renderAssets.gdiplusToken));
    return true;
}

const SpriteBitmap* FindCharacterSprite(const std::string& name)
{
    const std::string canonical = CanonicalizeCharacterNameForSpriteLookup(name);
    const auto it = g_renderAssets.sprites.find(canonical);
    if (it == g_renderAssets.sprites.end())
    {
        return nullptr;
    }
    return &it->second;
}

std::vector<std::string> BuildSessionIconCharacters(const BattleLogSession& session, bool playerOne)
{
    struct CharacterCount
    {
        std::string character;
        int matchCount = 0;
        int lastSeenIndex = -1;
    };

    std::map<std::string, CharacterCount> counts;
    for (size_t index = 0; index < session.matches.size(); ++index)
    {
        const BattleLogMatch& match = session.matches[index];
        const std::string character = playerOne ? match.p1CharacterDisplay : match.p2CharacterDisplay;
        if (character.empty())
        {
            continue;
        }

        CharacterCount& entry = counts[character];
        entry.character = character;
        ++entry.matchCount;
        entry.lastSeenIndex = static_cast<int>(index);
    }

    std::vector<CharacterCount> ordered;
    ordered.reserve(counts.size());
    for (const auto& [_, value] : counts)
    {
        ordered.push_back(value);
    }

    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const CharacterCount& left, const CharacterCount& right)
        {
            if (left.matchCount != right.matchCount)
            {
                return left.matchCount > right.matchCount;
            }
            if (left.lastSeenIndex != right.lastSeenIndex)
            {
                return left.lastSeenIndex > right.lastSeenIndex;
            }
            return _stricmp(left.character.c_str(), right.character.c_str()) < 0;
        });

    std::vector<std::string> result;
    result.reserve((std::min)(ordered.size(), static_cast<size_t>(kBrowserIconSlotsPerPlayer)));
    for (size_t index = 0; index < ordered.size() && index < static_cast<size_t>(kBrowserIconSlotsPerPlayer); ++index)
    {
        result.push_back(ordered[index].character);
    }
    return result;
}

int MeasureText5x7(const std::string& text)
{
    return netplay::font::MeasureText5x7Width(text, 1);
}

std::string FitTextToPixelWidth(const std::string& text, int maxWidth)
{
    if (maxWidth <= 0 || text.empty())
    {
        return {};
    }

    std::string fitted;
    fitted.reserve(text.size());
    for (char c : text)
    {
        std::string trial = fitted;
        trial.push_back(c);
        if (MeasureText5x7(trial) > maxWidth)
        {
            break;
        }
        fitted.push_back(c);
    }
    return fitted;
}

void FitTwoTextsToPixelWidth(
    const std::string& leftText,
    const std::string& rightText,
    int totalWidth,
    std::string* outLeftText,
    std::string* outRightText)
{
    if (outLeftText == nullptr || outRightText == nullptr)
    {
        return;
    }

    if (totalWidth <= 0)
    {
        outLeftText->clear();
        outRightText->clear();
        return;
    }

    const int leftFullWidth = MeasureText5x7(leftText);
    const int rightFullWidth = MeasureText5x7(rightText);
    int leftBudget = totalWidth / 2;
    int rightBudget = totalWidth - leftBudget;

    std::string fittedLeft = FitTextToPixelWidth(leftText, leftBudget);
    std::string fittedRight = FitTextToPixelWidth(rightText, rightBudget);
    int leftUsed = MeasureText5x7(fittedLeft);
    int rightUsed = MeasureText5x7(fittedRight);
    int remaining = totalWidth - leftUsed - rightUsed;

    while (remaining > 0)
    {
        const int leftDeficit = leftFullWidth - leftUsed;
        const int rightDeficit = rightFullWidth - rightUsed;
        if (leftDeficit <= 0 && rightDeficit <= 0)
        {
            break;
        }

        if (leftDeficit >= rightDeficit && leftDeficit > 0)
        {
            fittedLeft = FitTextToPixelWidth(leftText, leftUsed + remaining);
            leftUsed = MeasureText5x7(fittedLeft);
        }
        else if (rightDeficit > 0)
        {
            fittedRight = FitTextToPixelWidth(rightText, rightUsed + remaining);
            rightUsed = MeasureText5x7(fittedRight);
        }

        const int updatedRemaining = totalWidth - leftUsed - rightUsed;
        if (updatedRemaining >= remaining)
        {
            break;
        }
        remaining = updatedRemaining;
    }

    *outLeftText = std::move(fittedLeft);
    *outRightText = std::move(fittedRight);
}

void CollectDrawableBrowserCharacters(
    const BattleLogSession& session,
    std::vector<std::string>* outP1Characters,
    std::vector<std::string>* outP2Characters)
{
    if (outP1Characters != nullptr)
    {
        outP1Characters->clear();
    }
    if (outP2Characters != nullptr)
    {
        outP2Characters->clear();
    }

    if (!g_renderAssets.iconsReady)
    {
        return;
    }

    for (const std::string& character : session.p1IconCharacters)
    {
        if (outP1Characters == nullptr || outP1Characters->size() >= static_cast<size_t>(kBrowserIconSlotsPerPlayer))
        {
            break;
        }
        if (FindCharacterSprite(character) != nullptr)
        {
            outP1Characters->push_back(character);
        }
    }

    for (const std::string& character : session.p2IconCharacters)
    {
        if (outP2Characters == nullptr || outP2Characters->size() >= static_cast<size_t>(kBrowserIconSlotsPerPlayer))
        {
            break;
        }
        if (FindCharacterSprite(character) != nullptr)
        {
            outP2Characters->push_back(character);
        }
    }
}

BrowserRowLayout ComputeBrowserRowLayout(
    const BattleLogSession& session,
    size_t p1IconCount,
    size_t p2IconCount)
{
    BrowserRowLayout layout = {};
    const std::string shortDate =
        session.date.size() >= 10
            ? session.date.substr(5)
            : session.date;
    const std::string shortTime =
        session.time.substr(0, (std::min)(session.time.size(), static_cast<size_t>(5)));
    layout.dateTimeText = shortDate.empty() ? shortTime : (shortDate + " " + shortTime);
    layout.scoreText =
        session.matches.empty()
            ? std::string("--")
            : FormatResultPair(GetSessionFinalScoreLeft(session), GetSessionFinalScoreRight(session));

    const int rowLeft = kContentColumnX + 4;
    const int rowRight = kContentColumnX + kContentColumnW - 4;
    const int labelWidth = MeasureText5x7(layout.dateTimeText);
    layout.dateLeft = rowLeft;
    layout.dateRight = rowLeft + labelWidth;

    const int p1IconsWidth =
        p1IconCount > 0
            ? static_cast<int>(p1IconCount) * kBrowserIconSlotSize
                + (static_cast<int>(p1IconCount) - 1) * kBrowserIconGap
            : 0;
    const int p2IconsWidth =
        p2IconCount > 0
            ? static_cast<int>(p2IconCount) * kBrowserIconSlotSize
                + (static_cast<int>(p2IconCount) - 1) * kBrowserIconGap
            : 0;
    const int scoreWidth = MeasureText5x7(layout.scoreText);
    const int availableLeft = layout.dateRight + kBrowserDateGap;
    const int availableWidth = (std::max)(0, rowRight - availableLeft + 1);
    const int fixedWidth =
        p1IconsWidth
        + (p1IconsWidth > 0 ? kBrowserIconNameGap : 0)
        + kBrowserScoreGap
        + scoreWidth
        + kBrowserScoreGap
        + (p2IconsWidth > 0 ? kBrowserIconNameGap : 0)
        + p2IconsWidth;
    const int maxNamesWidth = (std::max)(0, availableWidth - fixedWidth);

    FitTwoTextsToPixelWidth(session.p1Name, session.p2Name, maxNamesWidth, &layout.leftNameText, &layout.rightNameText);

    const int leftNameWidth = MeasureText5x7(layout.leftNameText);
    const int rightNameWidth = MeasureText5x7(layout.rightNameText);
    const int blockWidth =
        p1IconsWidth
        + (p1IconsWidth > 0 ? kBrowserIconNameGap : 0)
        + leftNameWidth
        + kBrowserScoreGap
        + scoreWidth
        + kBrowserScoreGap
        + rightNameWidth
        + (p2IconsWidth > 0 ? kBrowserIconNameGap : 0)
        + p2IconsWidth;
    int x = availableLeft + (std::max)(0, availableWidth - blockWidth) / 2;

    layout.p1IconsX = x;
    layout.p1IconCount = static_cast<int>(p1IconCount);
    x += p1IconsWidth;
    if (p1IconsWidth > 0)
    {
        x += kBrowserIconNameGap;
    }

    layout.p1NameLeft = x;
    layout.p1NameRight = x + leftNameWidth;
    x += leftNameWidth + kBrowserScoreGap;

    layout.scoreLeft = x;
    layout.scoreRight = x + scoreWidth;
    x += scoreWidth + kBrowserScoreGap;

    layout.p2NameLeft = x;
    layout.p2NameRight = x + rightNameWidth;
    x += rightNameWidth;
    if (p2IconsWidth > 0)
    {
        x += kBrowserIconNameGap;
    }
    layout.p2IconsX = x;
    layout.p2IconCount = static_cast<int>(p2IconCount);
    return layout;
}

DetailRowLayout ComputeDetailRowLayout(
    const BattleLogMatch& match,
    int absoluteMatchIndex,
    const BattleLogSession* session,
    bool p1HasIcon,
    bool p2HasIcon)
{
    DetailRowLayout layout = {};
    const std::string shortTime =
        match.time.size() >= 5
            ? match.time.substr(0, 5)
            : match.time;
    layout.labelText = "#" + std::to_string(absoluteMatchIndex) + " " + shortTime;
    layout.roundsText = FormatResultPair(match.p1Rounds, match.p2Rounds);
    layout.durationText = FormatDurationShort(match.durationSeconds);
    const std::string p1Name = session != nullptr ? session->p1Name : std::string("--");
    const std::string p2Name = session != nullptr ? session->p2Name : std::string("--");

    const int rowLeft = kContentColumnX + 4;
    const int rowRight = kContentColumnX + kContentColumnW - 6;
    const int labelWidth = MeasureText5x7(layout.labelText);
    const int durationWidth = MeasureText5x7(layout.durationText);
    const int roundsWidth = MeasureText5x7(layout.roundsText);
    layout.labelLeft = rowLeft;
    layout.labelRight = rowLeft + labelWidth;
    layout.durationRight = rowRight;
    layout.durationLeft = rowRight - durationWidth;
    layout.p1HasIcon = p1HasIcon;
    layout.p2HasIcon = p2HasIcon;

    const int p1IconWidth = p1HasIcon ? kDetailRowIconSize : 0;
    const int p2IconWidth = p2HasIcon ? kDetailRowIconSize : 0;
    const int availableLeft = layout.labelRight + kDetailLabelGap;
    const int availableRight = layout.durationLeft - kDetailDurationGap;
    const int availableWidth = (std::max)(0, availableRight - availableLeft);
    const int fixedWidth =
        p1IconWidth
        + (p1IconWidth > 0 ? kDetailIconNameGap : 0)
        + kDetailScoreGap
        + roundsWidth
        + kDetailScoreGap
        + (p2IconWidth > 0 ? kDetailIconNameGap : 0)
        + p2IconWidth;
    const int maxNamesWidth = (std::max)(0, availableWidth - fixedWidth);

    FitTwoTextsToPixelWidth(p1Name, p2Name, maxNamesWidth, &layout.leftNameText, &layout.rightNameText);

    const int leftNameWidth = MeasureText5x7(layout.leftNameText);
    const int rightNameWidth = MeasureText5x7(layout.rightNameText);
    const int blockWidth =
        p1IconWidth
        + (p1IconWidth > 0 ? kDetailIconNameGap : 0)
        + leftNameWidth
        + kDetailScoreGap
        + roundsWidth
        + kDetailScoreGap
        + rightNameWidth
        + (p2IconWidth > 0 ? kDetailIconNameGap : 0)
        + p2IconWidth;
    int x = availableLeft + (std::max)(0, availableWidth - blockWidth) / 2;

    layout.p1IconX = x;
    x += p1IconWidth;
    if (p1IconWidth > 0)
    {
        x += kDetailIconNameGap;
    }
    layout.p1NameLeft = x;
    layout.p1NameRight = x + leftNameWidth;
    x += leftNameWidth + kDetailScoreGap;

    layout.roundsLeft = x;
    layout.roundsRight = x + roundsWidth;
    x += roundsWidth + kDetailScoreGap;

    layout.p2NameLeft = x;
    layout.p2NameRight = x + rightNameWidth;
    x += rightNameWidth;
    if (p2IconWidth > 0)
    {
        x += kDetailIconNameGap;
    }
    layout.p2IconX = x;
    return layout;
}

bool EnsureRenderAssetsLoaded(uint32_t screenContext)
{
    (void)screenContext;

    if (!g_renderAssets.spriteDirectoryResolved)
    {
        if (!g_renderAssets.loadAttempted)
        {
            g_renderAssets.loadAttempted = true;
            mod::Log("BattleLog::EnsureRenderAssetsLoaded: skipped spriteDirectoryResolved=0");
        }
        return false;
    }

    if (g_renderAssets.iconsReady)
    {
        return true;
    }

    if (g_renderAssets.loadAttempted)
    {
        return false;
    }
    g_renderAssets.loadAttempted = true;

    if (!EnsureGdiplusStarted())
    {
        return false;
    }

    int loadedCount = 0;
    for (const auto& entry : kCharacterSpriteFiles)
    {
        const std::string path =
            netplay::assets::JoinPath(g_renderAssets.resolvedSpriteDirectory, entry.fileName);
        if (!netplay::assets::FileExists(path))
        {
            continue;
        }

        const std::wstring widePath = AnsiPathToWide(path);
        if (widePath.empty())
        {
            mod::Log(
                "BattleLog::EnsureRenderAssetsLoaded: failed to widen path '%s'",
                path.c_str());
            continue;
        }

        Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromFile(widePath.c_str(), FALSE);
        if (bitmap == nullptr)
        {
            mod::Log(
                "BattleLog::EnsureRenderAssetsLoaded: Bitmap::FromFile failed path='%s'",
                path.c_str());
            continue;
        }

        const Gdiplus::Status status = bitmap->GetLastStatus();
        if (status != Gdiplus::Ok || bitmap->GetWidth() == 0 || bitmap->GetHeight() == 0)
        {
            mod::Log(
                "BattleLog::EnsureRenderAssetsLoaded: invalid bitmap path='%s' status=%d size=%ux%u",
                path.c_str(),
                static_cast<int>(status),
                bitmap->GetWidth(),
                bitmap->GetHeight());
            delete bitmap;
            continue;
        }

        SpriteBitmap sprite = {};
        sprite.path = path;
        sprite.bitmap = bitmap;
        sprite.width = bitmap->GetWidth();
        sprite.height = bitmap->GetHeight();

        Gdiplus::Rect lockRect(0, 0, static_cast<INT>(sprite.width), static_cast<INT>(sprite.height));
        Gdiplus::BitmapData bitmapData = {};
        const Gdiplus::Status lockStatus = bitmap->LockBits(
            &lockRect,
            Gdiplus::ImageLockModeRead,
            PixelFormat32bppARGB,
            &bitmapData);
        if (lockStatus != Gdiplus::Ok || bitmapData.Scan0 == nullptr)
        {
            mod::Log(
                "BattleLog::EnsureRenderAssetsLoaded: LockBits failed path='%s' status=%d",
                path.c_str(),
                static_cast<int>(lockStatus));
            delete bitmap;
            continue;
        }

        sprite.bgraPixels.resize(static_cast<size_t>(sprite.width) * static_cast<size_t>(sprite.height) * 4u);
        const int srcStride = bitmapData.Stride;
        const uint8_t* srcBase = static_cast<const uint8_t*>(bitmapData.Scan0);
        for (UINT y = 0; y < sprite.height; ++y)
        {
            const uint8_t* srcRow = srcStride >= 0
                ? (srcBase + static_cast<size_t>(y) * static_cast<size_t>(srcStride))
                : (srcBase + static_cast<size_t>(sprite.height - 1u - y) * static_cast<size_t>(-srcStride));
            uint8_t* dstRow = sprite.bgraPixels.data() + static_cast<size_t>(y) * static_cast<size_t>(sprite.width) * 4u;
            std::memcpy(dstRow, srcRow, static_cast<size_t>(sprite.width) * 4u);
        }
        bitmap->UnlockBits(&bitmapData);

        g_renderAssets.sprites.emplace(entry.canonicalName, sprite);
        ++loadedCount;
    }

    g_renderAssets.iconsReady = loadedCount > 0;
    mod::Log(
        "BattleLog::EnsureRenderAssetsLoaded: using PNG sprite path loaded=%d/%zu dir='%s'",
        loadedCount,
        kCharacterSpriteFiles.size(),
        g_renderAssets.resolvedSpriteDirectory.c_str());
    if (g_renderAssets.iconsReady)
    {
        (void)EnsureD3d9OverlayHookInstalled();
    }
    return g_renderAssets.iconsReady;
}

void ResetRenderAssetFrameState()
{
    g_renderAssets.firstSpriteBlitLogged = false;
    g_d3dOverlay.firstD3dSpriteBlitLogged = false;
    g_d3dOverlay.firstAcceptedRenderTargetLogged = false;
    g_d3dOverlay.firstRejectedRenderTargetLogged = false;
    g_d3dOverlay.firstNullDeviceLogged = false;
    g_d3dOverlay.firstInactiveMenuSkipLogged = false;
    g_d3dOverlay.firstWrongMenuSkipLogged = false;
    g_d3dOverlay.firstWrongViewSkipLogged = false;
    g_d3dOverlay.firstIconsNotReadySkipLogged = false;
    g_d3dOverlay.firstGetRenderTargetFailureLogged = false;
    g_d3dOverlay.firstGetRenderTargetDescFailureLogged = false;
    g_d3dOverlay.firstTextureReadyFailureLogged = false;
    g_d3dOverlay.firstStateBlockFailureLogged = false;
    g_d3dOverlay.firstNoDrawableIconsLogged = false;
    g_d3dOverlay.firstMissingSessionSpriteLogged = false;
    g_d3dOverlay.firstHookUnavailableLogged = false;
    g_d3dOverlay.firstGetViewportFailureLogged = false;
    g_d3dOverlay.firstGetSwapChainFailureLogged = false;
    g_d3dOverlay.firstGetPresentParametersFailureLogged = false;
    g_d3dOverlay.targetTraceLogsRemaining = 24;
}

void ReleaseRenderAssets()
{
    for (auto& entry : g_renderAssets.sprites)
    {
        if (entry.second.d3dTexture != nullptr)
        {
            entry.second.d3dTexture->Release();
            entry.second.d3dTexture = nullptr;
        }
        delete entry.second.bitmap;
        entry.second.bitmap = nullptr;
    }
    g_renderAssets.sprites.clear();
    g_d3dOverlay.textureDevice = nullptr;

    if (g_renderAssets.gdiplusStarted)
    {
        Gdiplus::GdiplusShutdown(g_renderAssets.gdiplusToken);
    }

    g_renderAssets.gdiplusStarted = false;
    g_renderAssets.gdiplusToken = 0;
    g_renderAssets.iconsReady = false;
    g_renderAssets.loadAttempted = false;
}

void ReleaseD3dTextures()
{
    for (auto& entry : g_renderAssets.sprites)
    {
        if (entry.second.d3dTexture != nullptr)
        {
            entry.second.d3dTexture->Release();
            entry.second.d3dTexture = nullptr;
        }
    }
    g_d3dOverlay.textureDevice = nullptr;
    g_d3dOverlay.firstD3dSpriteBlitLogged = false;
}

bool EnsureD3dTexturesReady(LPDIRECT3DDEVICE9 device)
{
    if (device == nullptr || !g_renderAssets.iconsReady)
    {
        return false;
    }

    if (g_d3dOverlay.textureDevice != device)
    {
        ReleaseD3dTextures();
        g_d3dOverlay.textureDevice = device;
    }

    bool ready = false;
    for (auto& entry : g_renderAssets.sprites)
    {
        SpriteBitmap& sprite = entry.second;
        if (sprite.width == 0 || sprite.height == 0 || sprite.bgraPixels.empty())
        {
            continue;
        }

        if (sprite.d3dTexture == nullptr)
        {
            IDirect3DTexture9* texture = nullptr;
            const HRESULT createHr = device->CreateTexture(
                sprite.width,
                sprite.height,
                1,
                0,
                D3DFMT_A8R8G8B8,
                D3DPOOL_MANAGED,
                &texture,
                nullptr);
            if (FAILED(createHr) || texture == nullptr)
            {
                mod::Log(
                    "BattleLog::EnsureD3dTexturesReady: CreateTexture failed path='%s' hr=0x%08X",
                    sprite.path.c_str(),
                    static_cast<unsigned>(createHr));
                continue;
            }

            D3DLOCKED_RECT lockedRect = {};
            const HRESULT lockHr = texture->LockRect(0, &lockedRect, nullptr, 0);
            if (FAILED(lockHr) || lockedRect.pBits == nullptr)
            {
                mod::Log(
                    "BattleLog::EnsureD3dTexturesReady: LockRect failed path='%s' hr=0x%08X",
                    sprite.path.c_str(),
                    static_cast<unsigned>(lockHr));
                texture->Release();
                continue;
            }

            for (UINT y = 0; y < sprite.height; ++y)
            {
                const uint8_t* srcRow = sprite.bgraPixels.data()
                    + static_cast<size_t>(y) * static_cast<size_t>(sprite.width) * 4u;
                uint8_t* dstRow = static_cast<uint8_t*>(lockedRect.pBits)
                    + static_cast<size_t>(y) * static_cast<size_t>(lockedRect.Pitch);
                std::memcpy(dstRow, srcRow, static_cast<size_t>(sprite.width) * 4u);
            }

            texture->UnlockRect(0);
            sprite.d3dTexture = texture;
        }

        ready = ready || (sprite.d3dTexture != nullptr);
    }

    return ready;
}

bool RenderBrowserIconsD3d9(LPDIRECT3DDEVICE9 device)
{
    if (device == nullptr)
    {
        if (!g_d3dOverlay.firstNullDeviceLogged)
        {
            mod::Log("BattleLog::RenderBrowserIconsD3d9: skipped because device=null");
            g_d3dOverlay.firstNullDeviceLogged = true;
        }
        return false;
    }

    if (!hooks::g_netplayMenuState.active)
    {
        if (!g_d3dOverlay.firstInactiveMenuSkipLogged)
        {
            mod::Log("BattleLog::RenderBrowserIconsD3d9: skipped because netplay menu inactive");
            g_d3dOverlay.firstInactiveMenuSkipLogged = true;
        }
        return false;
    }

    if (hooks::g_netplayMenuState.menuId != NetplayMenuId::BattleLog)
    {
        if (!g_d3dOverlay.firstWrongMenuSkipLogged)
        {
            mod::Log(
                "BattleLog::RenderBrowserIconsD3d9: skipped because menuId=%d not BattleLog",
                static_cast<int>(hooks::g_netplayMenuState.menuId));
            g_d3dOverlay.firstWrongMenuSkipLogged = true;
        }
        return false;
    }

    const bool isBrowserView = g_state.view == View::Browser;
    const bool isDetailView = g_state.view == View::SetDetail;
    if (!isBrowserView && !isDetailView)
    {
        if (!g_d3dOverlay.firstWrongViewSkipLogged)
        {
            mod::Log(
                "BattleLog::RenderBrowserIconsD3d9: skipped because view=%d not Browser/SetDetail",
                static_cast<int>(g_state.view));
            g_d3dOverlay.firstWrongViewSkipLogged = true;
        }
        return false;
    }

    if (hooks::g_debugOverlay.open)
    {
        if (!g_d3dOverlay.firstWrongViewSkipLogged)
        {
            mod::Log("BattleLog::RenderBrowserIconsD3d9: skipped because debug overlay is open");
            g_d3dOverlay.firstWrongViewSkipLogged = true;
        }
        return false;
    }

    if (!g_renderAssets.iconsReady)
    {
        if (!g_d3dOverlay.firstIconsNotReadySkipLogged)
        {
            mod::Log("BattleLog::RenderBrowserIconsD3d9: skipped because iconsReady=0");
            g_d3dOverlay.firstIconsNotReadySkipLogged = true;
        }
        return false;
    }

    IDirect3DSurface9* renderTarget = nullptr;
    if (FAILED(device->GetRenderTarget(0, &renderTarget)) || renderTarget == nullptr)
    {
        if (!g_d3dOverlay.firstGetRenderTargetFailureLogged)
        {
            mod::Log(
                "BattleLog::RenderBrowserIconsD3d9: GetRenderTarget failed device=%p rt=%p",
                device,
                renderTarget);
            g_d3dOverlay.firstGetRenderTargetFailureLogged = true;
        }
        return false;
    }

    D3DSURFACE_DESC renderTargetDesc = {};
    const HRESULT renderTargetDescHr = renderTarget->GetDesc(&renderTargetDesc);
    if (FAILED(renderTargetDescHr))
    {
        if (!g_d3dOverlay.firstGetRenderTargetDescFailureLogged)
        {
            mod::Log(
                "BattleLog::RenderBrowserIconsD3d9: GetDesc failed rt=%p hr=0x%08X",
                renderTarget,
                static_cast<unsigned>(renderTargetDescHr));
            g_d3dOverlay.firstGetRenderTargetDescFailureLogged = true;
        }
        renderTarget->Release();
        return false;
    }

    IDirect3DSurface9* backBuffer = nullptr;
    const HRESULT backBufferHr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    D3DSURFACE_DESC backBufferDesc = {};
    bool backBufferDescReady = false;
    if (SUCCEEDED(backBufferHr) && backBuffer != nullptr)
    {
        backBufferDescReady = SUCCEEDED(backBuffer->GetDesc(&backBufferDesc));
    }

    bool matchesBackBuffer = false;
    IUnknown* renderTargetIdentity = nullptr;
    IUnknown* backBufferIdentity = nullptr;
    if (SUCCEEDED(backBufferHr)
        && backBuffer != nullptr
        && SUCCEEDED(renderTarget->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&renderTargetIdentity)))
        && SUCCEEDED(backBuffer->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&backBufferIdentity))))
    {
        matchesBackBuffer = (renderTargetIdentity == backBufferIdentity);
    }

    if (renderTargetIdentity != nullptr)
    {
        renderTargetIdentity->Release();
    }
    if (backBufferIdentity != nullptr)
    {
        backBufferIdentity->Release();
    }

    D3DVIEWPORT9 viewport = {};
    bool viewportReady = false;
    const HRESULT viewportHr = device->GetViewport(&viewport);
    if (SUCCEEDED(viewportHr))
    {
        viewportReady = true;
    }
    else if (!g_d3dOverlay.firstGetViewportFailureLogged)
    {
        mod::Log(
            "BattleLog::RenderBrowserIconsD3d9: GetViewport failed device=%p hr=0x%08X",
            device,
            static_cast<unsigned>(viewportHr));
        g_d3dOverlay.firstGetViewportFailureLogged = true;
    }

    IDirect3DSwapChain9* swapChain = nullptr;
    D3DPRESENT_PARAMETERS presentParameters = {};
    bool presentParametersReady = false;
    const HRESULT swapChainHr = device->GetSwapChain(0, &swapChain);
    if (SUCCEEDED(swapChainHr) && swapChain != nullptr)
    {
        const HRESULT presentParametersHr = swapChain->GetPresentParameters(&presentParameters);
        if (SUCCEEDED(presentParametersHr))
        {
            presentParametersReady = true;
        }
        else if (!g_d3dOverlay.firstGetPresentParametersFailureLogged)
        {
            mod::Log(
                "BattleLog::RenderBrowserIconsD3d9: GetPresentParameters failed swapChain=%p hr=0x%08X",
                swapChain,
                static_cast<unsigned>(presentParametersHr));
            g_d3dOverlay.firstGetPresentParametersFailureLogged = true;
        }
    }
    else if (!g_d3dOverlay.firstGetSwapChainFailureLogged)
    {
        mod::Log(
            "BattleLog::RenderBrowserIconsD3d9: GetSwapChain failed device=%p hr=0x%08X swapChain=%p",
            device,
            static_cast<unsigned>(swapChainHr),
            swapChain);
        g_d3dOverlay.firstGetSwapChainFailureLogged = true;
    }

    RECT clientRect = {};
    bool clientRectReady = false;
    if (presentParametersReady
        && presentParameters.hDeviceWindow != nullptr
        && GetClientRect(presentParameters.hDeviceWindow, &clientRect))
    {
        clientRectReady = true;
    }

    if (g_d3dOverlay.targetTraceLogsRemaining > 0)
    {
        mod::Log(
            "BattleLog::EndSceneTrace[%u]: device=%p rt=%p rtSize=%ux%u bb=%p bbSize=%ux%u matchesBB=%d viewport=%u,%u %ux%u swapChain=%p pp=%ux%u windowed=%d hwnd=%p client=%ldx%ld",
            25u - g_d3dOverlay.targetTraceLogsRemaining,
            device,
            renderTarget,
            static_cast<unsigned>(renderTargetDesc.Width),
            static_cast<unsigned>(renderTargetDesc.Height),
            backBuffer,
            backBufferDescReady ? static_cast<unsigned>(backBufferDesc.Width) : 0u,
            backBufferDescReady ? static_cast<unsigned>(backBufferDesc.Height) : 0u,
            matchesBackBuffer ? 1 : 0,
            viewportReady ? viewport.X : 0u,
            viewportReady ? viewport.Y : 0u,
            viewportReady ? viewport.Width : 0u,
            viewportReady ? viewport.Height : 0u,
            swapChain,
            presentParametersReady ? presentParameters.BackBufferWidth : 0u,
            presentParametersReady ? presentParameters.BackBufferHeight : 0u,
            presentParametersReady ? (presentParameters.Windowed ? 1 : 0) : -1,
            presentParametersReady ? presentParameters.hDeviceWindow : nullptr,
            clientRectReady ? static_cast<long>(clientRect.right - clientRect.left) : 0l,
            clientRectReady ? static_cast<long>(clientRect.bottom - clientRect.top) : 0l);
        --g_d3dOverlay.targetTraceLogsRemaining;
    }

    const bool correctGameTarget =
        renderTargetDesc.Width == 640
        && renderTargetDesc.Height == 480;

    if (!correctGameTarget)
    {
        if (!g_d3dOverlay.firstRejectedRenderTargetLogged)
        {
            mod::Log(
                "BattleLog::RenderBrowserIconsD3d9: skipping rt=%p bb=%p size=%ux%u bbSize=%ux%u matchesBackBuffer=%d viewport=%u,%u %ux%u",
                renderTarget,
                backBuffer,
                static_cast<unsigned>(renderTargetDesc.Width),
                static_cast<unsigned>(renderTargetDesc.Height),
                backBufferDescReady ? static_cast<unsigned>(backBufferDesc.Width) : 0u,
                backBufferDescReady ? static_cast<unsigned>(backBufferDesc.Height) : 0u,
                matchesBackBuffer ? 1 : 0,
                viewportReady ? viewport.X : 0u,
                viewportReady ? viewport.Y : 0u,
                viewportReady ? viewport.Width : 0u,
                viewportReady ? viewport.Height : 0u);
            g_d3dOverlay.firstRejectedRenderTargetLogged = true;
        }
        if (swapChain != nullptr)
        {
            swapChain->Release();
        }
        if (backBuffer != nullptr)
        {
            backBuffer->Release();
        }
        renderTarget->Release();
        return false;
    }

    if (!g_d3dOverlay.firstAcceptedRenderTargetLogged)
    {
        mod::Log(
            "BattleLog::RenderBrowserIconsD3d9: using render target rt=%p bb=%p size=%ux%u bbSize=%ux%u viewport=%u,%u %ux%u",
            renderTarget,
            backBuffer,
            static_cast<unsigned>(renderTargetDesc.Width),
            static_cast<unsigned>(renderTargetDesc.Height),
            backBufferDescReady ? static_cast<unsigned>(backBufferDesc.Width) : 0u,
            backBufferDescReady ? static_cast<unsigned>(backBufferDesc.Height) : 0u,
            viewportReady ? viewport.X : 0u,
            viewportReady ? viewport.Y : 0u,
            viewportReady ? viewport.Width : 0u,
            viewportReady ? viewport.Height : 0u);
        g_d3dOverlay.firstAcceptedRenderTargetLogged = true;
    }

    if (swapChain != nullptr)
    {
        swapChain->Release();
    }
    if (backBuffer != nullptr)
    {
        backBuffer->Release();
    }
    renderTarget->Release();

    if (!EnsureD3dTexturesReady(device))
    {
        if (!g_d3dOverlay.firstTextureReadyFailureLogged)
        {
            mod::Log("BattleLog::RenderBrowserIconsD3d9: skipped because EnsureD3dTexturesReady returned false");
            g_d3dOverlay.firstTextureReadyFailureLogged = true;
        }
        return false;
    }

    IDirect3DStateBlock9* stateBlock = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &stateBlock)) || stateBlock == nullptr)
    {
        if (!g_d3dOverlay.firstStateBlockFailureLogged)
        {
            mod::Log(
                "BattleLog::RenderBrowserIconsD3d9: CreateStateBlock failed device=%p",
                device);
            g_d3dOverlay.firstStateBlockFailureLogged = true;
        }
        return false;
    }
    stateBlock->Capture();

    const int targetW = static_cast<int>(renderTargetDesc.Width);
    const int targetH = static_cast<int>(renderTargetDesc.Height);
    int viewportW = targetW;
    int viewportH = (viewportW * 240) / 320;
    if (viewportH > targetH)
    {
        viewportH = targetH;
        viewportW = (viewportH * 320) / 240;
    }
    const int viewportX = (targetW - viewportW) / 2;
    const int viewportY = (targetH - viewportH) / 2;

    struct TexturedVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        DWORD color;
        float u;
        float v;
    };
    constexpr DWORD kTexturedFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetFVF(kTexturedFvf);

    struct IconDrawRequest
    {
        const SpriteBitmap* sprite = nullptr;
        const char* character = nullptr;
        int logicalX = 0;
        int logicalY = 0;
        int logicalSize = 0;
        bool flipHorizontal = false;
        SpriteSlotAlignment alignment = SpriteSlotAlignment::Center;
    };

    int drawnIconCount = 0;
    if (isBrowserView)
    {
        for (int slot = 0; slot < netplay::menu::kBattleLogVisibleSessionRows; ++slot)
        {
            const BattleLogSession* session = GetSessionByIndex(GetSessionIndexForVisibleSlot(slot));
            if (session == nullptr || session->matches.empty())
            {
                continue;
            }
            const int rowY = kBrowserSessionRowY[static_cast<size_t>(slot)];
            const int iconY = rowY + (kBrowserRowH - kBrowserIconSlotSize) / 2;
            std::vector<std::string> p1Characters;
            std::vector<std::string> p2Characters;
            CollectDrawableBrowserCharacters(*session, &p1Characters, &p2Characters);
            const BrowserRowLayout layout =
                ComputeBrowserRowLayout(*session, p1Characters.size(), p2Characters.size());

            std::vector<IconDrawRequest> icons;
            icons.reserve(static_cast<size_t>(kBrowserIconSlotsPerPlayer * 2));

            for (size_t index = 0; index < p1Characters.size(); ++index)
            {
                const SpriteBitmap* sprite = FindCharacterSprite(p1Characters[index]);
                if (sprite == nullptr || sprite->d3dTexture == nullptr)
                {
                    if (!g_d3dOverlay.firstMissingSessionSpriteLogged)
                    {
                        mod::Log(
                            "BattleLog::RenderBrowserIconsD3d9: missing P1 sprite/texture session=%d char='%s' sprite=%p texture=%p",
                            session->sessionIndex,
                            p1Characters[index].c_str(),
                            sprite,
                            sprite != nullptr ? sprite->d3dTexture : nullptr);
                        g_d3dOverlay.firstMissingSessionSpriteLogged = true;
                    }
                    continue;
                }
                icons.push_back({
                    sprite,
                    p1Characters[index].c_str(),
                    layout.p1IconsX + static_cast<int>(index) * (kBrowserIconSlotSize + kBrowserIconGap),
                    iconY,
                    kBrowserIconSlotSize,
                    false,
                    SpriteSlotAlignment::Center,
                });
            }
            for (size_t index = 0; index < p2Characters.size(); ++index)
            {
                const SpriteBitmap* sprite = FindCharacterSprite(p2Characters[index]);
                if (sprite == nullptr || sprite->d3dTexture == nullptr)
                {
                    if (!g_d3dOverlay.firstMissingSessionSpriteLogged)
                    {
                        mod::Log(
                            "BattleLog::RenderBrowserIconsD3d9: missing P2 sprite/texture session=%d char='%s' sprite=%p texture=%p",
                            session->sessionIndex,
                            p2Characters[index].c_str(),
                            sprite,
                            sprite != nullptr ? sprite->d3dTexture : nullptr);
                        g_d3dOverlay.firstMissingSessionSpriteLogged = true;
                    }
                    continue;
                }
                icons.push_back({
                    sprite,
                    p2Characters[index].c_str(),
                    layout.p2IconsX + static_cast<int>(index) * (kBrowserIconSlotSize + kBrowserIconGap),
                    iconY,
                    kBrowserIconSlotSize,
                    true,
                    SpriteSlotAlignment::Center,
                });
            }

            for (const IconDrawRequest& icon : icons)
            {
                const SpriteBitmap* sprite = icon.sprite;
                const int logicalX = icon.logicalX;
                const int logicalY = icon.logicalY;
                const int slotLeft = viewportX + (logicalX * viewportW) / 320;
                const int slotTop = viewportY + (logicalY * viewportH) / 240;
                const int slotRight = viewportX + ((logicalX + icon.logicalSize) * viewportW) / 320;
                const int slotBottom = viewportY + ((logicalY + icon.logicalSize) * viewportH) / 240;
                const int slotW = (std::max)(1, slotRight - slotLeft);
                const int slotH = (std::max)(1, slotBottom - slotTop);
                int drawX = 0;
                int drawY = 0;
                int drawW = 0;
                int drawH = 0;
                ComputeScaledSpriteRect(
                    *sprite,
                    slotLeft,
                    slotTop,
                    slotW,
                    slotH,
                    icon.alignment,
                    &drawX,
                    &drawY,
                    &drawW,
                    &drawH);

                const float left = static_cast<float>(drawX) - 0.5f;
                const float top = static_cast<float>(drawY) - 0.5f;
                const float right = static_cast<float>(drawX + drawW) - 0.5f;
                const float bottom = static_cast<float>(drawY + drawH) - 0.5f;

                const float uLeft = icon.flipHorizontal ? 1.0f : 0.0f;
                const float uRight = icon.flipHorizontal ? 0.0f : 1.0f;
                const TexturedVertex vertices[4] = {
                    {left,  top,    0.0f, 1.0f, 0xFFFFFFFFu, uLeft,  0.0f},
                    {right, top,    0.0f, 1.0f, 0xFFFFFFFFu, uRight, 0.0f},
                    {left,  bottom, 0.0f, 1.0f, 0xFFFFFFFFu, uLeft,  1.0f},
                    {right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, uRight, 1.0f},
                };

                device->SetTexture(0, sprite->d3dTexture);
                (void)device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(TexturedVertex));
                ++drawnIconCount;
            }

            if (!g_d3dOverlay.firstD3dSpriteBlitLogged && !icons.empty())
            {
                mod::Log(
                    "BattleLog::DrawBrowserIconsD3d9: viewport=(%d,%d %dx%d) slot=%d session=%d p1Count=%zu p2Count=%zu",
                    viewportX,
                    viewportY,
                    viewportW,
                    viewportH,
                    slot,
                    session->sessionIndex,
                    p1Characters.size(),
                    p2Characters.size());
                g_d3dOverlay.firstD3dSpriteBlitLogged = true;
            }
        }
    }
    else if (isDetailView)
    {
        for (int slot = 0; slot < netplay::menu::kBattleLogVisibleGameRows; ++slot)
        {
            const BattleLogMatch* match = GetMatchForVisibleDetailSlot(slot);
            if (match == nullptr)
            {
                continue;
            }

            const int rowY = kDetailGameRowY[static_cast<size_t>(slot)];
            const int iconY = rowY + (kDetailRowH - kDetailRowIconSize) / 2;
            const BattleLogSession* session = GetDetailSession();
            const SpriteBitmap* p1Sprite = FindCharacterSprite(match->p1CharacterDisplay);
            const SpriteBitmap* p2Sprite = FindCharacterSprite(match->p2CharacterDisplay);
            const DetailRowLayout layout = ComputeDetailRowLayout(
                *match,
                g_state.detailPage * netplay::menu::kBattleLogVisibleGameRows + slot + 1,
                session,
                p1Sprite != nullptr,
                p2Sprite != nullptr);
            std::vector<IconDrawRequest> icons;
            icons.reserve(2);

            if (p1Sprite != nullptr && p1Sprite->d3dTexture != nullptr)
            {
                icons.push_back({
                    p1Sprite,
                    match->p1CharacterDisplay.c_str(),
                    layout.p1IconX,
                    iconY,
                    kDetailRowIconSize,
                    false,
                    SpriteSlotAlignment::Right,
                });
            }
            else if ((p1Sprite != nullptr || !match->p1CharacterDisplay.empty()) && !g_d3dOverlay.firstMissingSessionSpriteLogged)
            {
                mod::Log(
                    "BattleLog::RenderBrowserIconsD3d9: missing detail P1 sprite/texture match=%d char='%s' sprite=%p texture=%p",
                    match->matchIndex,
                    match->p1CharacterDisplay.c_str(),
                    p1Sprite,
                    p1Sprite != nullptr ? p1Sprite->d3dTexture : nullptr);
                g_d3dOverlay.firstMissingSessionSpriteLogged = true;
            }

            if (p2Sprite != nullptr && p2Sprite->d3dTexture != nullptr)
            {
                icons.push_back({
                    p2Sprite,
                    match->p2CharacterDisplay.c_str(),
                    layout.p2IconX,
                    iconY,
                    kDetailRowIconSize,
                    true,
                    SpriteSlotAlignment::Left,
                });
            }
            else if ((p2Sprite != nullptr || !match->p2CharacterDisplay.empty()) && !g_d3dOverlay.firstMissingSessionSpriteLogged)
            {
                mod::Log(
                    "BattleLog::RenderBrowserIconsD3d9: missing detail P2 sprite/texture match=%d char='%s' sprite=%p texture=%p",
                    match->matchIndex,
                    match->p2CharacterDisplay.c_str(),
                    p2Sprite,
                    p2Sprite != nullptr ? p2Sprite->d3dTexture : nullptr);
                g_d3dOverlay.firstMissingSessionSpriteLogged = true;
            }

            for (const IconDrawRequest& icon : icons)
            {
                const SpriteBitmap* sprite = icon.sprite;
                const int slotLeft = viewportX + (icon.logicalX * viewportW) / 320;
                const int slotTop = viewportY + (icon.logicalY * viewportH) / 240;
                const int slotRight = viewportX + ((icon.logicalX + icon.logicalSize) * viewportW) / 320;
                const int slotBottom = viewportY + ((icon.logicalY + icon.logicalSize) * viewportH) / 240;
                const int slotW = (std::max)(1, slotRight - slotLeft);
                const int slotH = (std::max)(1, slotBottom - slotTop);
                int drawX = 0;
                int drawY = 0;
                int drawW = 0;
                int drawH = 0;
                ComputeScaledSpriteRect(
                    *sprite,
                    slotLeft,
                    slotTop,
                    slotW,
                    slotH,
                    icon.alignment,
                    &drawX,
                    &drawY,
                    &drawW,
                    &drawH);

                const float left = static_cast<float>(drawX) - 0.5f;
                const float top = static_cast<float>(drawY) - 0.5f;
                const float right = static_cast<float>(drawX + drawW) - 0.5f;
                const float bottom = static_cast<float>(drawY + drawH) - 0.5f;

                const float uLeft = icon.flipHorizontal ? 1.0f : 0.0f;
                const float uRight = icon.flipHorizontal ? 0.0f : 1.0f;
                const TexturedVertex vertices[4] = {
                    {left,  top,    0.0f, 1.0f, 0xFFFFFFFFu, uLeft,  0.0f},
                    {right, top,    0.0f, 1.0f, 0xFFFFFFFFu, uRight, 0.0f},
                    {left,  bottom, 0.0f, 1.0f, 0xFFFFFFFFu, uLeft,  1.0f},
                    {right, bottom, 0.0f, 1.0f, 0xFFFFFFFFu, uRight, 1.0f},
                };

                device->SetTexture(0, sprite->d3dTexture);
                (void)device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(TexturedVertex));
                ++drawnIconCount;
            }

            if (!g_d3dOverlay.firstD3dSpriteBlitLogged && !icons.empty())
            {
                mod::Log(
                    "BattleLog::DrawDetailIconsD3d9: viewport=(%d,%d %dx%d) slot=%d match=%d p1='%s' p2='%s'",
                    viewportX,
                    viewportY,
                    viewportW,
                    viewportH,
                    slot,
                    match->matchIndex,
                    match->p1CharacterDisplay.c_str(),
                    match->p2CharacterDisplay.c_str());
                g_d3dOverlay.firstD3dSpriteBlitLogged = true;
            }
        }
    }

    device->SetTexture(0, nullptr);
    stateBlock->Apply();
    stateBlock->Release();
    if (drawnIconCount == 0 && !g_d3dOverlay.firstNoDrawableIconsLogged)
    {
        if (isBrowserView)
        {
            mod::Log(
                "BattleLog::RenderBrowserIconsD3d9: no drawable icons for current page browserPage=%d results=%d visibleRows=%d",
                g_state.browserPage,
                GetBrowserResultCount(),
                netplay::menu::kBattleLogVisibleSessionRows);
        }
        else
        {
            mod::Log(
                "BattleLog::RenderBrowserIconsD3d9: no drawable icons for current detail page detailPage=%d visibleRows=%d",
                g_state.detailPage,
                netplay::menu::kBattleLogVisibleGameRows);
        }
        g_d3dOverlay.firstNoDrawableIconsLogged = true;
    }
    return true;
}

HRESULT WINAPI HookedBattleLogEndScene(LPDIRECT3DDEVICE9 device)
{
    if (!g_d3dOverlay.endSceneObserved)
    {
        g_d3dOverlay.endSceneObserved = true;
        mod::Log("BattleLog::HookedBattleLogEndScene: first EndScene observed device=%p", device);
    }

    if (device != nullptr)
    {
        (void)RenderBrowserIconsD3d9(device);
        // ImGui debug overlay + async-host in-gameplay indicator (renders on any
        // screen; no-op unless something is active).
        netplay::debug_overlay::Render(device);
    }

    if (g_d3dOverlay.originalEndScene == nullptr)
    {
        return D3D_OK;
    }
    return g_d3dOverlay.originalEndScene(device);
}

bool EnsureD3d9OverlayHookInstalled()
{
    if (g_d3dOverlay.hookInstalled)
    {
        return true;
    }
    if (g_d3dOverlay.hookAttempted)
    {
        return false;
    }
    g_d3dOverlay.hookAttempted = true;

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        mod::Log("BattleLog::EnsureD3d9OverlayHookInstalled: MH_Initialize failed status=%d", static_cast<int>(initStatus));
        return false;
    }
    g_d3dOverlay.minhookInitialized = true;

    HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
    if (d3d9Module == nullptr)
    {
        d3d9Module = LoadLibraryA("d3d9.dll");
    }
    if (d3d9Module == nullptr)
    {
        mod::Log("BattleLog::EnsureD3d9OverlayHookInstalled: d3d9.dll unavailable");
        return false;
    }

    auto direct3dCreate9 = reinterpret_cast<LPDIRECT3D9(WINAPI*)(UINT)>(
        GetProcAddress(d3d9Module, "Direct3DCreate9"));
    if (direct3dCreate9 == nullptr)
    {
        mod::Log("BattleLog::EnsureD3d9OverlayHookInstalled: Direct3DCreate9 unavailable");
        return false;
    }

    const LPDIRECT3D9 d3d9 = direct3dCreate9(D3D_SDK_VERSION);
    if (d3d9 == nullptr)
    {
        mod::Log("BattleLog::EnsureD3d9OverlayHookInstalled: Direct3DCreate9 failed");
        return false;
    }

    WNDCLASSA windowClass = {};
    windowClass.lpfnWndProc = DefWindowProcA;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "EFZ_NETPLAY_D3D9_DUMMY";
    (void)RegisterClassA(&windowClass);
    const HWND dummyWindow = CreateWindowExA(
        0,
        windowClass.lpszClassName,
        "efz_netplay_dummy",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        64,
        64,
        nullptr,
        nullptr,
        windowClass.hInstance,
        nullptr);
    if (dummyWindow == nullptr)
    {
        mod::Log("BattleLog::EnsureD3d9OverlayHookInstalled: dummy window creation failed");
        d3d9->Release();
        return false;
    }

    D3DPRESENT_PARAMETERS presentParameters = {};
    presentParameters.Windowed = TRUE;
    presentParameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    presentParameters.BackBufferFormat = D3DFMT_UNKNOWN;
    presentParameters.BackBufferWidth = 2;
    presentParameters.BackBufferHeight = 2;
    presentParameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    presentParameters.hDeviceWindow = dummyWindow;

    LPDIRECT3DDEVICE9 tempDevice = nullptr;
    const HRESULT createDeviceHr = d3d9->CreateDevice(
        D3DADAPTER_DEFAULT,
        D3DDEVTYPE_HAL,
        dummyWindow,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING,
        &presentParameters,
        &tempDevice);
    if (FAILED(createDeviceHr) || tempDevice == nullptr)
    {
        mod::Log(
            "BattleLog::EnsureD3d9OverlayHookInstalled: CreateDevice failed hr=0x%08X",
            static_cast<unsigned>(createDeviceHr));
        DestroyWindow(dummyWindow);
        UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);
        d3d9->Release();
        return false;
    }

    // Hook EndScene by MinHook'ing the FUNCTION BODY (vtable[42] resolves to the
    // shared d3d9 EndScene). A vtable-slot patch on this *dummy* device does NOT
    // work: the game's device uses a different vtable instance (EFZ Revival's
    // present path), so patching the dummy slot never intercepts the game's
    // EndScene (confirmed live — the hook installed but "first EndScene observed"
    // never logged). MinHook patches the function itself, so every caller hits it.
    // Coexistence with other EndScene MinHookers (efz-training-mode): MinHook
    // relocates the existing prologue into our trampoline, so independent hooks
    // chain through each other rather than corrupting.
    void** vtable = (tempDevice != nullptr) ? *reinterpret_cast<void***>(tempDevice) : nullptr;
    g_d3dOverlay.endSceneTarget = (vtable != nullptr) ? vtable[42] : nullptr;

    tempDevice->Release();
    d3d9->Release();
    DestroyWindow(dummyWindow);
    UnregisterClassA(windowClass.lpszClassName, windowClass.hInstance);

    if (g_d3dOverlay.endSceneTarget == nullptr)
    {
        mod::Log("BattleLog::EnsureD3d9OverlayHookInstalled: EndScene target unavailable");
        return false;
    }

    const MH_STATUS createHookStatus = MH_CreateHook(
        g_d3dOverlay.endSceneTarget,
        reinterpret_cast<LPVOID>(&HookedBattleLogEndScene),
        reinterpret_cast<LPVOID*>(&g_d3dOverlay.originalEndScene));
    if (createHookStatus != MH_OK)
    {
        mod::Log(
            "BattleLog::EnsureD3d9OverlayHookInstalled: MH_CreateHook failed status=%d target=%p",
            static_cast<int>(createHookStatus),
            g_d3dOverlay.endSceneTarget);
        g_d3dOverlay.endSceneTarget = nullptr;
        return false;
    }

    const MH_STATUS enableHookStatus = MH_EnableHook(g_d3dOverlay.endSceneTarget);
    if (enableHookStatus != MH_OK)
    {
        mod::Log(
            "BattleLog::EnsureD3d9OverlayHookInstalled: MH_EnableHook failed status=%d",
            static_cast<int>(enableHookStatus));
        (void)MH_RemoveHook(g_d3dOverlay.endSceneTarget);
        g_d3dOverlay.endSceneTarget = nullptr;
        g_d3dOverlay.originalEndScene = nullptr;
        return false;
    }

    g_d3dOverlay.hookInstalled = true;
    mod::Log(
        "BattleLog::EnsureD3d9OverlayHookInstalled: MinHook EndScene installed target=%p original=%p",
        g_d3dOverlay.endSceneTarget,
        reinterpret_cast<void*>(g_d3dOverlay.originalEndScene));
    return true;
}

void ShutdownD3d9OverlayHook()
{
    ReleaseD3dTextures();

    if (g_d3dOverlay.hookInstalled && g_d3dOverlay.endSceneTarget != nullptr)
    {
        (void)MH_DisableHook(g_d3dOverlay.endSceneTarget);
        (void)MH_RemoveHook(g_d3dOverlay.endSceneTarget);
    }

    g_d3dOverlay = {};
}

bool DrawBrowserIconsGdi(uint32_t screenContext, bool allowWindowDc)
{
    const bool isBrowserView = g_state.view == View::Browser;
    const bool isDetailView = g_state.view == View::SetDetail;
    if ((!isBrowserView && !isDetailView) || !g_renderAssets.iconsReady)
    {
        return false;
    }

    HDC dc = nullptr;
    void* surface = nullptr;
    HWND window = nullptr;
    const bool acquired =
        allowWindowDc
            ? netplay::draw::AcquirePresentedMenuDrawDc(screenContext, &dc, &surface, &window, true)
            : netplay::draw::AcquireMenuDrawDc(screenContext, &dc, &surface, &window, false);
    if (!acquired)
    {
        return false;
    }

    RECT bounds = {};
    (void)GetClipBox(dc, &bounds);
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top)
    {
        if (window != nullptr && IsWindow(window))
        {
            (void)GetClientRect(window, &bounds);
        }
    }

    const int targetW = bounds.right - bounds.left;
    const int targetH = bounds.bottom - bounds.top;
    if (targetW <= 0 || targetH <= 0)
    {
        netplay::draw::ReleaseMenuDrawDc(dc, surface, window);
        return false;
    }

    int viewportW = targetW;
    int viewportH = (viewportW * 240) / 320;
    if (viewportH > targetH)
    {
        viewportH = targetH;
        viewportW = (viewportH * 320) / 240;
    }
    const int viewportX = bounds.left + (targetW - viewportW) / 2;
    const int viewportY = bounds.top + (targetH - viewportH) / 2;

    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

    struct IconDrawRequest
    {
        const SpriteBitmap* sprite = nullptr;
        const char* character = nullptr;
        int logicalX = 0;
        int logicalY = 0;
        int logicalSize = 0;
        bool flipHorizontal = false;
        SpriteSlotAlignment alignment = SpriteSlotAlignment::Center;
    };

    const int visibleRows = isBrowserView
        ? netplay::menu::kBattleLogVisibleSessionRows
        : netplay::menu::kBattleLogVisibleGameRows;
    for (int slot = 0; slot < visibleRows; ++slot)
    {
        std::vector<IconDrawRequest> icons;
        if (isBrowserView)
        {
            const BattleLogSession* session = GetSessionByIndex(GetSessionIndexForVisibleSlot(slot));
            if (session == nullptr || session->matches.empty())
            {
                continue;
            }

            const int rowY = kBrowserSessionRowY[static_cast<size_t>(slot)];
            const int iconY = rowY + (kBrowserRowH - kBrowserIconSlotSize) / 2;
            std::vector<std::string> p1Characters;
            std::vector<std::string> p2Characters;
            CollectDrawableBrowserCharacters(*session, &p1Characters, &p2Characters);
            const BrowserRowLayout layout =
                ComputeBrowserRowLayout(*session, p1Characters.size(), p2Characters.size());
            icons.reserve(static_cast<size_t>(kBrowserIconSlotsPerPlayer * 2));

            for (size_t index = 0; index < p1Characters.size(); ++index)
            {
                const SpriteBitmap* sprite = FindCharacterSprite(p1Characters[index]);
                if (sprite == nullptr || sprite->bitmap == nullptr)
                {
                    continue;
                }
                icons.push_back({
                    sprite,
                    p1Characters[index].c_str(),
                    layout.p1IconsX + static_cast<int>(index) * (kBrowserIconSlotSize + kBrowserIconGap),
                    iconY,
                    kBrowserIconSlotSize,
                    false,
                    SpriteSlotAlignment::Center,
                });
            }
            for (size_t index = 0; index < p2Characters.size(); ++index)
            {
                const SpriteBitmap* sprite = FindCharacterSprite(p2Characters[index]);
                if (sprite == nullptr || sprite->bitmap == nullptr)
                {
                    continue;
                }
                icons.push_back({
                    sprite,
                    p2Characters[index].c_str(),
                    layout.p2IconsX + static_cast<int>(index) * (kBrowserIconSlotSize + kBrowserIconGap),
                    iconY,
                    kBrowserIconSlotSize,
                    true,
                    SpriteSlotAlignment::Center,
                });
            }
        }
        else
        {
            const BattleLogMatch* match = GetMatchForVisibleDetailSlot(slot);
            if (match == nullptr)
            {
                continue;
            }

            const int rowY = kDetailGameRowY[static_cast<size_t>(slot)];
            const int iconY = rowY + (kDetailRowH - kDetailRowIconSize) / 2;
            const BattleLogSession* session = GetDetailSession();
            const SpriteBitmap* p1Sprite = FindCharacterSprite(match->p1CharacterDisplay);
            const SpriteBitmap* p2Sprite = FindCharacterSprite(match->p2CharacterDisplay);
            const DetailRowLayout layout = ComputeDetailRowLayout(
                *match,
                g_state.detailPage * netplay::menu::kBattleLogVisibleGameRows + slot + 1,
                session,
                p1Sprite != nullptr,
                p2Sprite != nullptr);
            icons.reserve(2);

            if (p1Sprite != nullptr && p1Sprite->bitmap != nullptr)
            {
                icons.push_back({
                    p1Sprite,
                    match->p1CharacterDisplay.c_str(),
                    layout.p1IconX,
                    iconY,
                    kDetailRowIconSize,
                    false,
                    SpriteSlotAlignment::Right,
                });
            }
            if (p2Sprite != nullptr && p2Sprite->bitmap != nullptr)
            {
                icons.push_back({
                    p2Sprite,
                    match->p2CharacterDisplay.c_str(),
                    layout.p2IconX,
                    iconY,
                    kDetailRowIconSize,
                    true,
                    SpriteSlotAlignment::Left,
                });
            }
        }

        for (const IconDrawRequest& icon : icons)
        {
            const SpriteBitmap* sprite = icon.sprite;
            const int slotLeft = viewportX + (icon.logicalX * viewportW) / 320;
            const int slotTop = viewportY + (icon.logicalY * viewportH) / 240;
            const int slotRight = viewportX + ((icon.logicalX + icon.logicalSize) * viewportW) / 320;
            const int slotBottom = viewportY + ((icon.logicalY + icon.logicalSize) * viewportH) / 240;
            const int slotW = (std::max)(1, slotRight - slotLeft);
            const int slotH = (std::max)(1, slotBottom - slotTop);
            int drawX = 0;
            int drawY = 0;
            int drawW = 0;
            int drawH = 0;
            ComputeScaledSpriteRect(
                *sprite,
                slotLeft,
                slotTop,
                slotW,
                slotH,
                icon.alignment,
                &drawX,
                &drawY,
                &drawW,
                &drawH);

            if (icon.flipHorizontal)
            {
                const Gdiplus::Point destPoints[3] = {
                    Gdiplus::Point(drawX + drawW, drawY),
                    Gdiplus::Point(drawX, drawY),
                    Gdiplus::Point(drawX + drawW, drawY + drawH),
                };
                graphics.DrawImage(
                    sprite->bitmap,
                    destPoints,
                    3,
                    0,
                    0,
                    static_cast<INT>(sprite->width),
                    static_cast<INT>(sprite->height),
                    Gdiplus::UnitPixel);
            }
            else
            {
                graphics.DrawImage(
                    sprite->bitmap,
                    Gdiplus::Rect(drawX, drawY, drawW, drawH),
                    0,
                    0,
                    static_cast<INT>(sprite->width),
                    static_cast<INT>(sprite->height),
                    Gdiplus::UnitPixel);
            }
        }

        if (!g_renderAssets.firstSpriteBlitLogged)
        {
            mod::Log(
                isBrowserView
                    ? "BattleLog::DrawBrowserIcons%s: viewport=(%d,%d %dx%d) slot=%d"
                    : "BattleLog::DrawDetailIcons%s: viewport=(%d,%d %dx%d) slot=%d",
                allowWindowDc ? "Presented" : "Backbuffer",
                viewportX,
                viewportY,
                viewportW,
                viewportH,
                slot);
            g_renderAssets.firstSpriteBlitLogged = true;
        }
    }

    netplay::draw::ReleaseMenuDrawDc(dc, surface, window);
    return true;
}

void PutSurfacePixelClamped(const netplay::font::IndexedSurfaceView& surface, int x, int y, uint8_t color)
{
    if (surface.pixels == nullptr || x < 0 || y < 0 || x >= surface.width || y >= surface.height)
    {
        return;
    }
    surface.pixels[y * surface.pitch + x] = color;
}

uint8_t ResolveSpritePaletteColor(
    uint32_t screenContext,
    std::map<uint32_t, uint8_t>* paletteCache,
    int r,
    int g,
    int b)
{
    const uint32_t key =
        (static_cast<uint32_t>(r & 0xFF) << 16)
        | (static_cast<uint32_t>(g & 0xFF) << 8)
        | static_cast<uint32_t>(b & 0xFF);

    if (paletteCache != nullptr)
    {
        const auto it = paletteCache->find(key);
        if (it != paletteCache->end())
        {
            return it->second;
        }
    }

    const uint8_t color = netplay::draw::ResolveBestPaletteColor(screenContext, r, g, b);
    if (paletteCache != nullptr)
    {
        paletteCache->emplace(key, color);
    }
    return color;
}

void DrawScaledSpriteToSurface(
    const netplay::font::IndexedSurfaceView& surface,
    uint32_t screenContext,
    const SpriteBitmap& sprite,
    int slotX,
    int slotY,
    int slotW,
    int slotH,
    std::map<uint32_t, uint8_t>* paletteCache)
{
    if (sprite.width == 0 || sprite.height == 0 || sprite.bgraPixels.empty() || slotW <= 0 || slotH <= 0)
    {
        return;
    }

    const double scale = (std::min)(
        static_cast<double>(slotW) / static_cast<double>(sprite.width),
        static_cast<double>(slotH) / static_cast<double>(sprite.height));
    const int drawW = (std::max)(1, static_cast<int>(sprite.width * scale + 0.5));
    const int drawH = (std::max)(1, static_cast<int>(sprite.height * scale + 0.5));
    const int drawX = slotX + (slotW - drawW) / 2;
    const int drawY = slotY + (slotH - drawH) / 2;

    for (int y = 0; y < drawH; ++y)
    {
        const UINT srcY = static_cast<UINT>((static_cast<uint64_t>(y) * sprite.height) / static_cast<uint64_t>(drawH));
        const uint8_t* srcRow = sprite.bgraPixels.data() + static_cast<size_t>(srcY) * static_cast<size_t>(sprite.width) * 4u;
        for (int x = 0; x < drawW; ++x)
        {
            const UINT srcX = static_cast<UINT>((static_cast<uint64_t>(x) * sprite.width) / static_cast<uint64_t>(drawW));
            const uint8_t* src = srcRow + static_cast<size_t>(srcX) * 4u;
            const uint8_t alpha = src[3];
            if (alpha < 32)
            {
                continue;
            }

            const int b = src[0];
            const int g = src[1];
            const int r = src[2];
            const uint8_t color = ResolveSpritePaletteColor(screenContext, paletteCache, r, g, b);
            PutSurfacePixelClamped(surface, drawX + x, drawY + y, color);
        }
    }
}

void ComputeScaledSpriteRect(
    const SpriteBitmap& sprite,
    int slotLeft,
    int slotTop,
    int slotW,
    int slotH,
    SpriteSlotAlignment alignment,
    int* outDrawX,
    int* outDrawY,
    int* outDrawW,
    int* outDrawH)
{
    if (outDrawX == nullptr || outDrawY == nullptr || outDrawW == nullptr || outDrawH == nullptr)
    {
        return;
    }

    const double scale = (std::min)(
        static_cast<double>(slotW) / static_cast<double>(sprite.width),
        static_cast<double>(slotH) / static_cast<double>(sprite.height));
    const int drawW = (std::max)(1, static_cast<int>(sprite.width * scale + 0.5));
    const int drawH = (std::max)(1, static_cast<int>(sprite.height * scale + 0.5));
    int drawX = slotLeft + (slotW - drawW) / 2;
    if (alignment == SpriteSlotAlignment::Left)
    {
        drawX = slotLeft;
    }
    else if (alignment == SpriteSlotAlignment::Right)
    {
        drawX = slotLeft + slotW - drawW;
    }
    const int drawY = slotTop + (slotH - drawH) / 2;

    *outDrawX = drawX;
    *outDrawY = drawY;
    *outDrawW = drawW;
    *outDrawH = drawH;
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
        return "Nayuki (Awake)";
    }
    if (token == "NayukiS")
    {
        return "Nayuki (Sleepy)";
    }
    if (token == "BossUnknown")
    {
        return "Unknown";
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

void LogParseDiagnostic(const std::string& path, int lineNumber, const char* message)
{
    if (lineNumber > 0)
    {
        mod::Log(
            "BattleLog::Parse: %s line=%d file='%s'",
            message,
            lineNumber,
            path.c_str());
        return;
    }

    mod::Log("BattleLog::Parse: %s file='%s'", message, path.c_str());
}

void UpdateDerivedSummaryStats(BattleLogSummary* summary, const BattleLogSession& session)
{
    if (summary == nullptr || session.matches.empty())
    {
        return;
    }

    const int gameCount = static_cast<int>(session.matches.size());
    if (session.totalDurationSeconds > summary->longestSetByDurationSeconds)
    {
        summary->longestSetByDurationSeconds = session.totalDurationSeconds;
        summary->longestSetByDurationSessionIndex = session.sessionIndex;
    }

    if (gameCount > summary->longestSetByGamesCount)
    {
        summary->longestSetByGamesCount = gameCount;
        summary->longestSetByGamesSessionIndex = session.sessionIndex;
    }
}

void FinalizeDerivedSummaryStats(BattleLogSummary* summary)
{
    if (summary == nullptr)
    {
        return;
    }

    if (summary->completedSessions > 0)
    {
        summary->averageGamesPerCompletedSet =
            static_cast<double>(summary->totalGames) / static_cast<double>(summary->completedSessions);
        summary->averageSetDurationSeconds =
            (summary->totalDurationSeconds + (summary->completedSessions / 2)) / summary->completedSessions;
    }
    else
    {
        summary->averageGamesPerCompletedSet = 0.0;
        summary->averageSetDurationSeconds = 0;
    }
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
    int diagnosticCount = 0;

    const DWORD attrs = GetFileAttributesA(path.c_str());
    document.fileExists = (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0);
    if (!document.fileExists)
    {
        document.sourceEncoding = "missing";
        LogParseDiagnostic(path, 0, "battle log file was not found");
        return document;
    }

    std::wstring text;
    FileEncoding encoding = FileEncoding::Utf8;
    if (!ReadWideTextFile(path, &text, &encoding))
    {
        document.sourceEncoding = "unreadable";
        LogParseDiagnostic(path, 0, "battle log file could not be read");
        return document;
    }

    document.sourceEncoding =
        encoding == FileEncoding::Utf16Le ? "utf16le" :
        encoding == FileEncoding::Utf8Bom ? "utf8-bom" :
        "utf8";

    std::set<std::string> characters;
    characters.insert("Unknown");
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
                    ++diagnosticCount;
                    LogParseDiagnostic(path, lineNumber, "match row was found before any session header");
                    continue;
                }

                currentSession->totalDurationSeconds += parsedMatch.durationSeconds;
                currentSession->matches.push_back(parsedMatch);
                characters.insert(parsedMatch.p1CharacterDisplay);
                characters.insert(parsedMatch.p2CharacterDisplay);
                ++matchIndex;
                continue;
            }

            ++diagnosticCount;
            LogParseDiagnostic(path, lineNumber, "malformed match row was skipped");
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

        ++diagnosticCount;
        LogParseDiagnostic(path, lineNumber, "unrecognized battle log line was skipped");
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
        session.p1IconCharacters = BuildSessionIconCharacters(session, true);
        session.p2IconCharacters = BuildSessionIconCharacters(session, false);
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

    if (diagnosticCount > 0)
    {
        mod::Log(
            "BattleLog::Parse: completed with %d diagnostic(s) file='%s'",
            diagnosticCount,
            path.c_str());
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

bool MatchSetStatus(const BattleLogSession& session, const BattleLogFilter& filter)
{
    switch (ParseSetStatusMode(filter.setStatus))
    {
    case SetStatusMode::Played:
        return !session.matches.empty();
    case SetStatusMode::Empty:
        return session.matches.empty();
    case SetStatusMode::All:
    default:
        return true;
    }
}

bool MatchGameCount(const BattleLogSession& session, const BattleLogFilter& filter)
{
    const size_t gameCount = session.matches.size();
    switch (ParseGameCountFilterMode(filter.gameCount))
    {
    case GameCountFilterMode::One:
        return gameCount == 1;
    case GameCountFilterMode::TwoToThree:
        return gameCount >= 2 && gameCount <= 3;
    case GameCountFilterMode::FourPlus:
        return gameCount >= 4;
    case GameCountFilterMode::Any:
    default:
        return true;
    }
}

bool MatchCharacterSwitches(const BattleLogSession& session, const BattleLogFilter& filter)
{
    const bool hasSwitches = session.p1SwitchedCharacter || session.p2SwitchedCharacter;
    switch (ParseSwitchFilterMode(filter.characterSwitches))
    {
    case SwitchFilterMode::Stable:
        return !hasSwitches;
    case SwitchFilterMode::Swapped:
        return hasSwitches;
    case SwitchFilterMode::Any:
    default:
        return true;
    }
}

bool SessionMatchesFilter(const BattleLogSession& session, const BattleLogFilter& filter)
{
    if (!MatchSetStatus(session, filter)
        || !MatchGameCount(session, filter)
        || !MatchCharacterSwitches(session, filter))
    {
        return false;
    }

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

std::string BuildMatchupLabel(const std::string& playerCharacter, const std::string& opponentCharacter)
{
    if (playerCharacter.empty() && opponentCharacter.empty())
    {
        return {};
    }

    return (playerCharacter.empty() ? std::string("?") : playerCharacter)
        + " vs "
        + (opponentCharacter.empty() ? std::string("?") : opponentCharacter);
}

void FinalizeSummaryUsageCounts(
    BattleLogSummary* summary,
    const std::map<std::string, int>& characterCounts,
    const std::map<std::string, int>& matchupCounts)
{
    if (summary == nullptr)
    {
        return;
    }

    int bestCount = 0;
    for (const auto& [character, count] : characterCounts)
    {
        if (count > bestCount)
        {
            bestCount = count;
            summary->mostUsedCharacter = character;
        }
    }

    bestCount = 0;
    for (const auto& [matchup, count] : matchupCounts)
    {
        if (count > bestCount)
        {
            bestCount = count;
            summary->mostUsedMatchup = matchup;
        }
    }

    FinalizeDerivedSummaryStats(summary);
    summary->hasSessions = summary->totalSessions > 0;
}

void AccumulatePerspectiveSummary(
    BattleLogSummary* summary,
    std::map<std::string, int>* characterCounts,
    std::map<std::string, int>* matchupCounts,
    const BattleLogSession& session,
    bool focusIsP1)
{
    if (summary == nullptr || characterCounts == nullptr)
    {
        return;
    }

    summary->hasPerspective = true;
    ++summary->matchingSessions;
    ++summary->totalSessions;

    if (session.matches.empty())
    {
        ++summary->emptySessions;
        return;
    }

    ++summary->completedSessions;
    UpdateDerivedSummaryStats(summary, session);
    const int playerSetScore = focusIsP1 ? GetSessionFinalScoreLeft(session) : GetSessionFinalScoreRight(session);
    const int opponentSetScore = focusIsP1 ? GetSessionFinalScoreRight(session) : GetSessionFinalScoreLeft(session);
    if (playerSetScore > opponentSetScore)
    {
        ++summary->setWins;
    }
    else if (playerSetScore < opponentSetScore)
    {
        ++summary->setLosses;
    }

    for (const BattleLogMatch& match : session.matches)
    {
        ++summary->totalGames;
        summary->totalDurationSeconds += match.durationSeconds;
        const int playerRounds = focusIsP1 ? match.p1Rounds : match.p2Rounds;
        const int opponentRounds = focusIsP1 ? match.p2Rounds : match.p1Rounds;
        if (playerRounds > opponentRounds)
        {
            ++summary->gameWins;
        }
        else if (playerRounds < opponentRounds)
        {
            ++summary->gameLosses;
        }

        const std::string& character = focusIsP1 ? match.p1CharacterDisplay : match.p2CharacterDisplay;
        if (!character.empty())
        {
            ++(*characterCounts)[character];
        }

        if (matchupCounts != nullptr)
        {
            const std::string& playerCharacter = focusIsP1 ? match.p1CharacterDisplay : match.p2CharacterDisplay;
            const std::string& opponentCharacter = focusIsP1 ? match.p2CharacterDisplay : match.p1CharacterDisplay;
            const std::string matchup = BuildMatchupLabel(playerCharacter, opponentCharacter);
            if (!matchup.empty())
            {
                ++(*matchupCounts)[matchup];
            }
        }
    }
}

void AccumulateAggregateSummary(
    BattleLogSummary* summary,
    std::map<std::string, int>* characterCounts,
    const BattleLogSession& session)
{
    if (summary == nullptr || characterCounts == nullptr)
    {
        return;
    }

    ++summary->matchingSessions;
    ++summary->totalSessions;

    if (session.matches.empty())
    {
        ++summary->emptySessions;
        return;
    }

    ++summary->completedSessions;
    UpdateDerivedSummaryStats(summary, session);
    summary->totalGames += static_cast<int>(session.matches.size());
    for (const BattleLogMatch& match : session.matches)
    {
        summary->totalDurationSeconds += match.durationSeconds;
        if (!match.p1CharacterDisplay.empty())
        {
            ++(*characterCounts)[match.p1CharacterDisplay];
        }
        if (!match.p2CharacterDisplay.empty())
        {
            ++(*characterCounts)[match.p2CharacterDisplay];
        }
    }
}

BattleLogSummary BuildSummaryForNickname(const BattleLogDocument& document, const std::string& nickname)
{
    BattleLogSummary summary = {};
    summary.nickname = nickname;

    int64_t latestSortKey = 0;
    std::map<std::string, int> characterCounts;
    std::map<std::string, int> matchupCounts;
    for (const BattleLogSession& session : document.sessions)
    {
        const bool nicknameIsP1 = StringEqualsTrimmed(session.p1Name, nickname);
        const bool nicknameIsP2 = StringEqualsTrimmed(session.p2Name, nickname);
        if (!nicknameIsP1 && !nicknameIsP2)
        {
            continue;
        }

        if (session.sortKey >= latestSortKey)
        {
            latestSortKey = session.sortKey;
            summary.recentOpponent = nicknameIsP1 ? session.p2Name : session.p1Name;
            summary.recentTimestamp = FormatDateTime(session.date, session.time);
        }
        AccumulatePerspectiveSummary(&summary, &characterCounts, nullptr, session, nicknameIsP1);
    }

    FinalizeSummaryUsageCounts(&summary, characterCounts, matchupCounts);
    return summary;
}

BattleLogSummary BuildSummaryForDocument(const BattleLogDocument& document)
{
    BattleLogSummary summary = {};
    summary.nickname = "All logged sets";

    int64_t latestSortKey = 0;
    std::map<std::string, int> characterCounts;
    std::map<std::string, int> matchupCounts;
    for (const BattleLogSession& session : document.sessions)
    {
        if (session.sortKey >= latestSortKey)
        {
            latestSortKey = session.sortKey;
            summary.recentOpponent = session.p1Name + " vs " + session.p2Name;
            summary.recentTimestamp = FormatDateTime(session.date, session.time);
        }
        AccumulateAggregateSummary(&summary, &characterCounts, session);
    }

    FinalizeSummaryUsageCounts(&summary, characterCounts, matchupCounts);
    return summary;
}

BattleLogSummary BuildSummaryForFilteredSessions(
    const BattleLogDocument& document,
    const std::vector<int>& sessionIndices,
    const BattleLogFilter& filter)
{
    BattleLogSummary summary = {};
    const std::string playerName = TrimUtf8(filter.playerName);
    const std::string opponentName = TrimUtf8(filter.opponentName);
    const std::string focusName = !playerName.empty() ? playerName : opponentName;
    const std::string secondaryName =
        (!playerName.empty() && !opponentName.empty())
            ? opponentName
            : std::string();
    summary.nickname = focusName.empty() ? "Filtered results" : focusName;
    summary.secondaryNickname = secondaryName;
    summary.isHeadToHead = !focusName.empty() && !secondaryName.empty();

    int64_t latestSortKey = 0;
    std::map<std::string, int> characterCounts;
    std::map<std::string, int> matchupCounts;
    for (int sessionIndex : sessionIndices)
    {
        if (sessionIndex < 0 || sessionIndex >= static_cast<int>(document.sessions.size()))
        {
            continue;
        }

        const BattleLogSession& session = document.sessions[static_cast<size_t>(sessionIndex)];
        if (session.sortKey >= latestSortKey)
        {
            latestSortKey = session.sortKey;
            if (!focusName.empty())
            {
                const bool playerIsP1 = StringEqualsTrimmed(session.p1Name, focusName);
                const bool playerIsP2 = StringEqualsTrimmed(session.p2Name, focusName);
                if (playerIsP1 || playerIsP2)
                {
                    summary.recentOpponent = playerIsP1 ? session.p2Name : session.p1Name;
                    summary.recentTimestamp = FormatDateTime(session.date, session.time);
                }
                else
                {
                    summary.recentOpponent = session.p1Name + " vs " + session.p2Name;
                    summary.recentTimestamp = FormatDateTime(session.date, session.time);
                }
            }
            else
            {
                summary.recentOpponent = session.p1Name + " vs " + session.p2Name;
                summary.recentTimestamp = FormatDateTime(session.date, session.time);
            }
        }

        if (!focusName.empty())
        {
            const bool playerIsP1 = StringEqualsTrimmed(session.p1Name, focusName);
            const bool playerIsP2 = StringEqualsTrimmed(session.p2Name, focusName);
            if (playerIsP1 || playerIsP2)
            {
                AccumulatePerspectiveSummary(
                    &summary,
                    &characterCounts,
                    summary.isHeadToHead ? &matchupCounts : nullptr,
                    session,
                    playerIsP1);
                continue;
            }
        }

        AccumulateAggregateSummary(&summary, &characterCounts, session);
    }

    FinalizeSummaryUsageCounts(&summary, characterCounts, matchupCounts);
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
        return "SEARCH";
    case View::SetDetail:
        return "SET DETAILS";
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
        return 10;
    default:
        return 0;
    }
}

int GetBrowserSelectableSessionCount()
{
    const int remaining =
        GetBrowserResultCount() - g_state.browserPage * netplay::menu::kBattleLogVisibleSessionRows;
    return (std::max)(1, (std::min)(remaining, netplay::menu::kBattleLogVisibleSessionRows));
}

int GetDetailSelectableGameCount()
{
    const BattleLogSession* session = GetDetailSession();
    if (session == nullptr)
    {
        return 1;
    }

    const int remaining =
        static_cast<int>(session->matches.size())
        - g_state.detailPage * netplay::menu::kBattleLogVisibleGameRows;
    return (std::max)(1, (std::min)(remaining, netplay::menu::kBattleLogVisibleGameRows));
}

int ClampSelectionForCurrentView(int selection)
{
    switch (g_state.view)
    {
    case View::Browser:
        if (selection < netplay::menu::kBattleLogVisibleSessionRows)
        {
            return (std::max)(0, (std::min)(selection, GetBrowserSelectableSessionCount() - 1));
        }
        return (std::max)(
            netplay::menu::kBattleLogVisibleSessionRows,
            (std::min)(selection, netplay::menu::kBattleLogVisibleSessionRows + 3));
    case View::Filters:
        return (std::max)(0, (std::min)(selection, 9));
    case View::SetDetail:
        if (selection < netplay::menu::kBattleLogVisibleGameRows)
        {
            return (std::max)(0, (std::min)(selection, GetDetailSelectableGameCount() - 1));
        }
        return (std::max)(
            netplay::menu::kBattleLogVisibleGameRows,
            (std::min)(selection, netplay::menu::kBattleLogVisibleGameRows + 2));
    case View::Summary:
    default:
        return (std::max)(0, (std::min)(selection, 4));
    }
}

void RememberGroupedSelection(View view, int selection)
{
    switch (view)
    {
    case View::Browser:
        if (selection < netplay::menu::kBattleLogVisibleSessionRows)
        {
            g_state.browserListSelection = selection;
        }
        else
        {
            g_state.browserActionSelection = selection;
        }
        break;
    case View::Filters:
        if (selection < 7)
        {
            g_state.filtersFieldSelection = selection;
        }
        else
        {
            g_state.filtersActionSelection = selection;
        }
        break;
    case View::SetDetail:
        if (selection < netplay::menu::kBattleLogVisibleGameRows)
        {
            g_state.detailListSelection = selection;
        }
        else
        {
            g_state.detailActionSelection = selection;
        }
        break;
    default:
        break;
    }
}

bool TogglePaneSelection(int currentSelection, int* outNextSelection)
{
    if (outNextSelection == nullptr)
    {
        return false;
    }

    switch (g_state.view)
    {
    case View::Browser:
        *outNextSelection =
            currentSelection < netplay::menu::kBattleLogVisibleSessionRows
                ? ClampSelectionForCurrentView(g_state.browserActionSelection)
                : ClampSelectionForCurrentView(g_state.browserListSelection);
        return true;
    case View::Filters:
        *outNextSelection =
            currentSelection < 7
                ? ClampSelectionForCurrentView(g_state.filtersActionSelection)
                : ClampSelectionForCurrentView(g_state.filtersFieldSelection);
        return true;
    case View::SetDetail:
        *outNextSelection =
            currentSelection < netplay::menu::kBattleLogVisibleGameRows
                ? ClampSelectionForCurrentView(g_state.detailActionSelection)
                : ClampSelectionForCurrentView(g_state.detailListSelection);
        return true;
    default:
        return false;
    }
}

void SetSelection(uint32_t screenContext, int selection)
{
    const int clamped = ClampSelectionForCurrentView(selection);
    *GetSelectionStorage(g_state.view) = clamped;
    RememberGroupedSelection(g_state.view, clamped);

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

const std::array<const char*, 3>& GetSetStatusOptions()
{
    static constexpr std::array<const char*, 3> kOptions = {"Any", "Played Only", "Empty Only"};
    return kOptions;
}

const std::array<const char*, 4>& GetGameCountFilterOptions()
{
    static constexpr std::array<const char*, 4> kOptions = {"Any", "1 Game", "2-3 Games", "4+ Games"};
    return kOptions;
}

const std::array<const char*, 3>& GetSwitchFilterOptions()
{
    static constexpr std::array<const char*, 3> kOptions = {"Any", "No Changes", "Has Changes"};
    return kOptions;
}

SetStatusMode ParseSetStatusMode(const std::string& value)
{
    if (value == "Played" || value == "Played Only")
    {
        return SetStatusMode::Played;
    }
    if (value == "Empty" || value == "Empty Only")
    {
        return SetStatusMode::Empty;
    }
    return SetStatusMode::All;
}

GameCountFilterMode ParseGameCountFilterMode(const std::string& value)
{
    if (value == "1 Game")
    {
        return GameCountFilterMode::One;
    }
    if (value == "2-3 Games")
    {
        return GameCountFilterMode::TwoToThree;
    }
    if (value == "4+ Games")
    {
        return GameCountFilterMode::FourPlus;
    }
    return GameCountFilterMode::Any;
}

SwitchFilterMode ParseSwitchFilterMode(const std::string& value)
{
    if (value == "Stable" || value == "No Changes")
    {
        return SwitchFilterMode::Stable;
    }
    if (value == "Swapped" || value == "Has Changes")
    {
        return SwitchFilterMode::Swapped;
    }
    return SwitchFilterMode::Any;
}

template <size_t N>
void CycleOptionValue(std::string* value, const std::array<const char*, N>& options, int delta)
{
    if (value == nullptr || options.empty())
    {
        return;
    }

    int index = 0;
    for (size_t optionIndex = 0; optionIndex < options.size(); ++optionIndex)
    {
        if (*value == options[optionIndex])
        {
            index = static_cast<int>(optionIndex);
            break;
        }
    }
    const int count = static_cast<int>(options.size());
    index = (index + delta + count) % count;
    *value = options[static_cast<size_t>(index)];
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
    filter->setStatus = TrimUtf8(filter->setStatus);
    filter->gameCount = TrimUtf8(filter->gameCount);
    filter->characterSwitches = TrimUtf8(filter->characterSwitches);

    if (filter->playerCharacter.empty())
    {
        filter->playerCharacter = "All";
    }
    if (filter->opponentCharacter.empty())
    {
        filter->opponentCharacter = "All";
    }
    if (filter->setStatus.empty())
    {
        filter->setStatus = GetDefaultBattleLogSetStatus();
    }
    if (filter->gameCount.empty())
    {
        filter->gameCount = "Any";
    }
    if (filter->characterSwitches.empty())
    {
        filter->characterSwitches = "Any";
    }
}

std::string GetDefaultBattleLogSetStatus()
{
    return netplay::mod_settings::HideEmptySetsInBattleLogByDefault()
        ? "Played Only"
        : "Any";
}

bool IsDefaultMineFilter(const BattleLogFilter& filter)
{
    const SetStatusMode defaultSetStatus = ParseSetStatusMode(GetDefaultBattleLogSetStatus());
    return StringEqualsTrimmed(filter.playerName, g_state.currentNickname)
        && TrimUtf8(filter.opponentName).empty()
        && IsAllCharacterValue(filter.playerCharacter)
        && IsAllCharacterValue(filter.opponentCharacter)
        && ParseSetStatusMode(filter.setStatus) == defaultSetStatus
        && ParseGameCountFilterMode(filter.gameCount) == GameCountFilterMode::Any
        && ParseSwitchFilterMode(filter.characterSwitches) == SwitchFilterMode::Any;
}

bool IsDefaultAllFilter(const BattleLogFilter& filter)
{
    const SetStatusMode defaultSetStatus = ParseSetStatusMode(GetDefaultBattleLogSetStatus());
    return TrimUtf8(filter.playerName).empty()
        && TrimUtf8(filter.opponentName).empty()
        && IsAllCharacterValue(filter.playerCharacter)
        && IsAllCharacterValue(filter.opponentCharacter)
        && ParseSetStatusMode(filter.setStatus) == defaultSetStatus
        && ParseGameCountFilterMode(filter.gameCount) == GameCountFilterMode::Any
        && ParseSwitchFilterMode(filter.characterSwitches) == SwitchFilterMode::Any;
}

bool HasSearchSummaryFilter(const BattleLogFilter& filter)
{
    return !IsDefaultMineFilter(filter) && !IsDefaultAllFilter(filter);
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

    g_state.nicknameSummary = BuildSummaryForNickname(g_state.document, g_state.currentNickname);
    g_state.fullSummary = BuildSummaryForDocument(g_state.document);
    g_state.searchSummary = BuildSummaryForFilteredSessions(g_state.document, g_state.filteredSessionIndices, g_state.activeFilter);
    g_state.browserSummary = g_state.searchSummary;
    RefreshDisplayedSummary();
}

void RefreshDisplayedSummary()
{
    switch (g_state.summaryMode)
    {
    case SummaryMode::FullLog:
        g_state.summary = g_state.fullSummary;
        break;
    case SummaryMode::Search:
        g_state.summary = g_state.searchSummary;
        break;
    case SummaryMode::Profile:
    default:
        g_state.summary = g_state.nicknameSummary;
        break;
    }
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
    else if (g_state.document.sourceEncoding == "unreadable")
    {
        SetStatusMessage("Battle log file could not be read.");
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
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogSetStatus, blankRow, "BATTLELOG_SET_STATUS"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogGameCount, blankRow, "BATTLELOG_GAME_COUNT"};
        g_state.entries[entryIndex++] = {NetplayMenuAction::BattleLogCharacterSwitches, blankRow, "BATTLELOG_SWITCHES"};
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
        parts.push_back("Player " + filter.playerName);
    }
    if (!filter.opponentName.empty())
    {
        parts.push_back("vs " + filter.opponentName);
    }
    if (!IsAllCharacterValue(filter.playerCharacter))
    {
        parts.push_back("Player character " + filter.playerCharacter);
    }
    if (!IsAllCharacterValue(filter.opponentCharacter))
    {
        parts.push_back("Opponent character " + filter.opponentCharacter);
    }
    if (ParseSetStatusMode(filter.setStatus) == SetStatusMode::Played)
    {
        parts.push_back("played only");
    }
    else if (ParseSetStatusMode(filter.setStatus) == SetStatusMode::Empty)
    {
        parts.push_back("empty only");
    }
    switch (ParseGameCountFilterMode(filter.gameCount))
    {
    case GameCountFilterMode::One:
        parts.push_back("1 game");
        break;
    case GameCountFilterMode::TwoToThree:
        parts.push_back("2-3 games");
        break;
    case GameCountFilterMode::FourPlus:
        parts.push_back("4+ games");
        break;
    case GameCountFilterMode::Any:
    default:
        break;
    }
    if (ParseSwitchFilterMode(filter.characterSwitches) == SwitchFilterMode::Stable)
    {
        parts.push_back("No character changes");
    }
    else if (ParseSwitchFilterMode(filter.characterSwitches) == SwitchFilterMode::Swapped)
    {
        parts.push_back("Has character changes");
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

double ComputeWinRatePercent(int wins, int losses)
{
    const int total = wins + losses;
    if (total <= 0)
    {
        return -1.0;
    }
    return (static_cast<double>(wins) * 100.0) / static_cast<double>(total);
}

std::string FormatRecordWithRate(const char* label, int wins, int losses)
{
    char buffer[96] = {};
    const double rate = ComputeWinRatePercent(wins, losses);
    if (rate < 0.0)
    {
        std::snprintf(buffer, sizeof(buffer), "%s %d-%d", label, wins, losses);
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%s %d-%d (%.1f%%)", label, wins, losses, rate);
    }
    return buffer;
}

std::string FormatRateText(const char* label, int wins, int losses)
{
    char buffer[64] = {};
    const double rate = ComputeWinRatePercent(wins, losses);
    if (rate < 0.0)
    {
        std::snprintf(buffer, sizeof(buffer), "%s N/A", label);
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "%s %.1f%%", label, rate);
    }
    return buffer;
}

const BattleLogSummary& GetDisplayedSummary()
{
    return g_state.summary;
}

std::string BuildSummaryTitleText()
{
    if (g_state.summaryMode == SummaryMode::FullLog)
    {
        return "BATTLE LOG SUMMARY";
    }
    if (g_state.summaryMode == SummaryMode::Search && g_state.summary.isHeadToHead)
    {
        return "MATCHUP SUMMARY";
    }
    if (g_state.summaryMode == SummaryMode::Search && !g_state.summary.hasPerspective)
    {
        return "SEARCH SUMMARY";
    }
    return "PLAYER SUMMARY";
}

std::string BuildSummaryIdentityText()
{
    if (g_state.summaryMode == SummaryMode::FullLog)
    {
        return "All logged sets";
    }
    if (g_state.summaryMode == SummaryMode::Search)
    {
        if (g_state.summary.isHeadToHead)
        {
            return g_state.summary.nickname + " vs " + g_state.summary.secondaryNickname;
        }
        if (!g_state.summary.nickname.empty())
        {
            return g_state.summary.nickname;
        }
    }
    return g_state.currentNickname;
}

std::string BuildSummaryLoadText()
{
    const BattleLogSummary& summary = GetDisplayedSummary();
    char buffer[128] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%s %d set(s), %d game(s)",
        g_state.summaryMode == SummaryMode::Search ? "Found" : "Loaded",
        summary.totalSessions,
        summary.totalGames);
    return buffer;
}

std::string BuildSummarySetRateText()
{
    const BattleLogSummary& summary = GetDisplayedSummary();
    if (!summary.hasPerspective)
    {
        return "Completed " + std::to_string(summary.completedSessions);
    }
    return FormatRateText("Set WR", summary.setWins, summary.setLosses);
}

std::string BuildSummaryGameRateText()
{
    const BattleLogSummary& summary = GetDisplayedSummary();
    if (!summary.hasPerspective)
    {
        return BuildSummaryAverageSetDurationText();
    }
    return FormatRateText("Game WR", summary.gameWins, summary.gameLosses);
}

std::string BuildSummaryAverageGamesText()
{
    const BattleLogSummary& summary = GetDisplayedSummary();
    if (summary.completedSessions <= 0)
    {
        return "Avg Games/Set N/A";
    }

    char buffer[64] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "Avg Games/Set %.1f",
        summary.averageGamesPerCompletedSet);
    return buffer;
}

std::string BuildSummaryAverageSetDurationText()
{
    const BattleLogSummary& summary = GetDisplayedSummary();
    if (summary.completedSessions <= 0)
    {
        return "Avg Set Time N/A";
    }

    return "Avg Set Time " + FormatDurationShort(summary.averageSetDurationSeconds);
}

std::string BuildSummaryLongestSetText()
{
    const BattleLogSummary& summary = GetDisplayedSummary();
    if (summary.completedSessions <= 0)
    {
        return "Longest Set N/A";
    }

    char buffer[96] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "Longest: %s / %d games",
        FormatDurationShort(summary.longestSetByDurationSeconds).c_str(),
        summary.longestSetByGamesCount);
    return buffer;
}

std::string BuildSummaryCompletionRatioText()
{
    const BattleLogSummary& summary = GetDisplayedSummary();
    if (summary.hasPerspective)
    {
        return FormatRecordWithRate("Win-Lose", summary.setWins, summary.setLosses);
    }
    return FormatRecordWithRate("Played", summary.completedSessions, summary.emptySessions);
}

std::string BuildSummaryUsageText()
{
    const BattleLogSummary& summary = GetDisplayedSummary();
    const std::string usage =
        summary.isHeadToHead
            ? ("Matchup: "
                + AbbreviateForDisplay(
                    summary.mostUsedMatchup.empty() ? std::string("N/A") : summary.mostUsedMatchup,
                    16))
            : ("Most played: "
                + AbbreviateForDisplay(
                    summary.mostUsedCharacter.empty() ? std::string("N/A") : summary.mostUsedCharacter,
                    14));
    return "Play time " + FormatDurationShort(summary.totalDurationSeconds) + "   " + usage;
}

std::string BuildSummaryRecentText()
{
    const BattleLogSummary& summary = GetDisplayedSummary();
    if (!summary.hasSessions)
    {
        return g_state.summaryMode == SummaryMode::FullLog
            ? "No logged sessions found."
            : "No sessions found.";
    }

    const std::string recentDate =
        summary.recentTimestamp.empty()
            ? std::string("unknown date")
            : summary.recentTimestamp.substr(0, summary.recentTimestamp.find(' '));

    if (summary.isHeadToHead)
    {
        return "Most recent: " + recentDate;
    }

    const std::string prefix = summary.hasPerspective ? "Last vs " : "Most recent: ";
    return prefix
        + (summary.recentOpponent.empty() ? "?" : summary.recentOpponent)
        + " on "
        + recentDate;
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
    if (g_state.document.sourceEncoding == "unreadable")
    {
        return "Unreadable: " + Basename(g_state.document.sourcePath);
    }

    return
        "Loaded " + std::to_string(g_state.document.sessions.size()) + " set(s), "
        + std::to_string(matchCount) + " game(s)";
}

std::string BuildBrowserRecordLeftText()
{
    if (g_state.browserSummary.hasPerspective)
    {
        return FormatRecordWithRate("Sets", g_state.browserSummary.setWins, g_state.browserSummary.setLosses);
    }

    return "Played " + std::to_string(g_state.browserSummary.completedSessions)
        + "  Empty " + std::to_string(g_state.browserSummary.emptySessions);
}

std::string BuildBrowserRecordRightText()
{
    if (g_state.browserSummary.hasPerspective)
    {
        return FormatRecordWithRate("Games", g_state.browserSummary.gameWins, g_state.browserSummary.gameLosses);
    }

    return "Games " + std::to_string(g_state.browserSummary.totalGames);
}

std::string BuildBrowserUsageLeftText()
{
    return "Playtime: " + FormatDurationShort(g_state.browserSummary.totalDurationSeconds);
}

std::string BuildBrowserUsageRightText()
{
    if (g_state.browserSummary.completedSessions <= 0)
    {
        return "Avg Games/Set N/A";
    }

    char buffer[64] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "Avg Games/Set %.1f",
        g_state.browserSummary.averageGamesPerCompletedSet);
    return buffer;
}

std::string BuildBrowserLongestSetText()
{
    if (g_state.browserSummary.completedSessions <= 0)
    {
        return "Longest N/A";
    }

    char buffer[96] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "Longest %s / %dg",
        FormatDurationShort(g_state.browserSummary.longestSetByDurationSeconds).c_str(),
        g_state.browserSummary.longestSetByGamesCount);
    return buffer;
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

void DrawRowBoxAt(
    const netplay::font::IndexedSurfaceView& surface,
    int x,
    int y,
    int w,
    int h,
    bool selected,
    uint8_t fillColor,
    uint8_t frameColor,
    uint8_t selectedFillColor)
{
    DrawPanelBox(
        surface,
        x,
        y,
        w,
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
    uint8_t alertColor)
{
    DrawPanelBox(
        surface,
        kSummaryActionPanelX,
        kSummaryActionPanelY,
        kSummaryActionPanelW,
        kSummaryActionPanelH,
        panelFill,
        panelFrame);
    DrawPanelBox(surface, kSummaryCardX, kSummaryCardY, kSummaryCardW, kSummaryCardH, panelFill, panelFrame);
    netplay::font::DrawTextCentered5x7(surface, BuildSummaryTitleText(), kSummaryCardX + 2, kSummaryCardX + kSummaryCardW - 2, kSummaryCardY + 4, 1, 1, titleColor);
    netplay::font::DrawTextCentered5x7(
        surface,
        "ACTIONS",
        kSummaryActionPanelX + 2,
        kSummaryActionPanelX + kSummaryActionPanelW - 2,
        kSummaryActionPanelY + 3,
        1,
        1,
        titleColor);
    netplay::font::DrawTextCentered5x7(surface, BuildSummaryIdentityText(), kSummaryCardX + 2, kSummaryCardX + kSummaryCardW - 2, kSummaryCardY + 18, 1, 1, textColor);
    netplay::font::DrawTextCentered5x7(surface, BuildSummaryLoadText(), kSummaryCardX + 8, kSummaryCardX + kSummaryCardW - 8, kSummaryCardY + 30, 1, 1, dimColor);

    const BattleLogSummary& summary = GetDisplayedSummary();
    const int halfSplitLeft = kSummaryCardX + (kSummaryCardW / 2) - 4;
    const int halfSplitRight = kSummaryCardX + (kSummaryCardW / 2) + 4;
    const int wideRightLeft = kSummaryCardX + 122;
    const std::string setsText = summary.hasPerspective
        ? ("Sets " + std::to_string(summary.setWins) + "-" + std::to_string(summary.setLosses))
        : ("Sessions " + std::to_string(summary.totalSessions));
    const std::string gamesText = summary.hasPerspective
        ? ("Games " + std::to_string(summary.gameWins) + "-" + std::to_string(summary.gameLosses))
        : ("Games " + std::to_string(summary.totalGames));
    netplay::font::DrawTextLeft5x7(surface, setsText, kSummaryCardX + 8, halfSplitLeft, kSummaryCardY + 42, 1, 1, textColor);
    netplay::font::DrawTextRight5x7(surface, gamesText, halfSplitRight, kSummaryCardX + kSummaryCardW - 8, kSummaryCardY + 42, 1, 1, textColor);

    netplay::font::DrawTextLeft5x7(
        surface,
        BuildSummarySetRateText(),
        kSummaryCardX + 8,
        halfSplitLeft,
        kSummaryCardY + 53,
        1,
        1,
        dimColor);
    netplay::font::DrawTextRight5x7(
        surface,
        BuildSummaryGameRateText(),
        halfSplitRight,
        kSummaryCardX + kSummaryCardW - 8,
        kSummaryCardY + 53,
        1,
        1,
        dimColor);

    const std::string playTimeText = "Playtime: " + FormatDurationShort(summary.totalDurationSeconds);
    const std::string mostPlayedText =
        summary.isHeadToHead
            ? ("Most played matchup: "
                + AbbreviateForDisplay(
                    summary.mostUsedMatchup.empty() ? std::string("N/A") : summary.mostUsedMatchup,
                    18))
            : ("Most played character: "
                + AbbreviateForDisplay(
                    summary.mostUsedCharacter.empty() ? std::string("N/A") : summary.mostUsedCharacter,
                    18));
    netplay::font::DrawTextLeft5x7(surface, BuildSummaryAverageGamesText(), kSummaryCardX + 8, halfSplitLeft, kSummaryCardY + 64, 1, 1, dimColor);
    if (summary.hasPerspective)
    {
        netplay::font::DrawTextRight5x7(surface, BuildSummaryAverageSetDurationText(), halfSplitRight, kSummaryCardX + kSummaryCardW - 8, kSummaryCardY + 64, 1, 1, dimColor);
    }
    else
    {
        netplay::font::DrawTextRight5x7(surface, BuildSummaryLongestSetText(), wideRightLeft, kSummaryCardX + kSummaryCardW - 8, kSummaryCardY + 64, 1, 1, dimColor);
    }
    netplay::font::DrawTextLeft5x7(surface, playTimeText, kSummaryCardX + 8, halfSplitLeft, kSummaryCardY + 75, 1, 1, textColor);
    netplay::font::DrawTextRight5x7(
        surface,
        summary.hasPerspective ? BuildSummaryLongestSetText() : BuildSummaryCompletionRatioText(),
        wideRightLeft,
        kSummaryCardX + kSummaryCardW - 8,
        kSummaryCardY + 75,
        1,
        1,
        dimColor);

    const uint8_t recentColor = summary.hasSessions ? textColor : alertColor;
    netplay::font::DrawTextLeft5x7(surface, mostPlayedText, kSummaryCardX + 8, kSummaryCardX + kSummaryCardW - 8, kSummaryCardY + 86, 1, 1, textColor);
    netplay::font::DrawTextLeft5x7(surface, BuildSummaryRecentText(), kSummaryCardX + 8, kSummaryCardX + kSummaryCardW - 8, kSummaryCardY + 97, 1, 1, recentColor);
}

void DrawBrowserPanel(
    const netplay::font::IndexedSurfaceView& surface,
    uint8_t panelFill,
    uint8_t panelFrame,
    uint8_t titleColor,
    uint8_t textColor,
    uint8_t dimColor,
    uint8_t /*alertColor*/)
{
    DrawPanelBox(surface, kBrowserHeaderPanelX, kBrowserHeaderPanelY, kBrowserHeaderPanelW, kBrowserHeaderPanelH, panelFill, panelFrame);
    DrawPanelBox(surface, kContentColumnX - 2, kBrowserContentPanelY, kContentColumnW + 4, 84, panelFill, panelFrame);
    DrawPanelBox(surface, kBrowserActionPanelX, kBrowserActionPanelY, kBrowserActionPanelW, kBrowserActionPanelH, panelFill, panelFrame);
    netplay::font::DrawTextCentered5x7(surface, "SET BROWSER", kBrowserHeaderPanelX + 2, kBrowserHeaderPanelX + kBrowserHeaderPanelW - 2, kBrowserHeaderPanelY + 4, 1, 1, titleColor);
    netplay::font::DrawTextLeft5x7(surface, BuildFilterSummary(g_state.activeFilter), kBrowserHeaderPanelX + 8, kBrowserHeaderPanelX + kBrowserHeaderPanelW - 8, kBrowserHeaderPanelY + 18, 1, 1, textColor);
    netplay::font::DrawTextLeft5x7(surface, BuildBrowserPageText(), kBrowserHeaderPanelX + 8, kBrowserHeaderPanelX + kBrowserHeaderPanelW - 8, kBrowserHeaderPanelY + 31, 1, 1, dimColor);
    netplay::font::DrawTextRight5x7(surface, BuildBrowserLongestSetText(), kBrowserHeaderPanelX + 8, kBrowserHeaderPanelX + kBrowserHeaderPanelW - 8, kBrowserHeaderPanelY + 31, 1, 1, dimColor);
    netplay::font::DrawTextLeft5x7(surface, BuildBrowserRecordLeftText(), kBrowserHeaderPanelX + 8, kBrowserHeaderPanelX + (kBrowserHeaderPanelW / 2) - 4, kBrowserHeaderPanelY + 44, 1, 1, textColor);
    netplay::font::DrawTextRight5x7(surface, BuildBrowserRecordRightText(), kBrowserHeaderPanelX + (kBrowserHeaderPanelW / 2) + 4, kBrowserHeaderPanelX + kBrowserHeaderPanelW - 8, kBrowserHeaderPanelY + 44, 1, 1, textColor);
    netplay::font::DrawTextLeft5x7(surface, BuildBrowserUsageLeftText(), kBrowserHeaderPanelX + 8, kBrowserHeaderPanelX + (kBrowserHeaderPanelW / 2) - 4, kBrowserHeaderPanelY + 57, 1, 1, dimColor);
    netplay::font::DrawTextRight5x7(surface, BuildBrowserUsageRightText(), kBrowserHeaderPanelX + (kBrowserHeaderPanelW / 2) + 4, kBrowserHeaderPanelX + kBrowserHeaderPanelW - 8, kBrowserHeaderPanelY + 57, 1, 1, dimColor);
    netplay::font::DrawTextCentered5x7(
        surface,
        "ACTIONS",
        kBrowserActionPanelX + 2,
        kBrowserActionPanelX + kBrowserActionPanelW - 2,
        kBrowserActionPanelY + 3,
        1,
        1,
        titleColor);
}

void DrawFiltersPanel(
    const netplay::font::IndexedSurfaceView& surface,
    uint8_t panelFill,
    uint8_t panelFrame,
    uint8_t titleColor,
    uint8_t textColor,
    uint8_t dimColor)
{
    DrawPanelBox(surface, kPanelX, kPanelY, kPanelW, 46, panelFill, panelFrame);
    DrawPanelBox(surface, kContentColumnX - 2, 72, kContentColumnW + 4, 96, panelFill, panelFrame);
    DrawPanelBox(surface, kActionStackX, 164, kActionStackW, 44, panelFill, panelFrame);
    netplay::font::DrawTextCentered5x7(surface, "SEARCH", kPanelX + 2, kPanelX + kPanelW - 2, kPanelY + 4, 1, 1, titleColor);
    netplay::font::DrawTextLeft5x7(surface, BuildFilterSummary(g_state.draftFilter), kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 18, 1, 1, textColor);
    netplay::font::DrawTextLeft5x7(surface, "Exact names. Character, set type, and game count filters.", kPanelX + 8, kPanelX + kPanelW - 8, kPanelY + 31, 1, 1, dimColor);
    netplay::font::DrawTextCentered5x7(surface, "ACTIONS", kActionStackX + 2, kActionStackX + kActionStackW - 2, 158, 1, 1, titleColor);
}

void DrawDetailPanel(
    const netplay::font::IndexedSurfaceView& surface,
    uint8_t panelFill,
    uint8_t panelFrame,
    uint8_t titleColor,
    uint8_t textColor,
    uint8_t dimColor,
    uint8_t alertColor)
{
    DrawPanelBox(surface, kDetailHeaderPanelX, kDetailHeaderPanelY, kDetailHeaderPanelW, kDetailHeaderPanelH, panelFill, panelFrame);
    DrawPanelBox(surface, kContentColumnX - 2, kDetailContentPanelY, kContentColumnW + 4, kDetailContentPanelH, panelFill, panelFrame);
    DrawPanelBox(surface, kDetailActionPanelX, kDetailActionPanelY, kDetailActionPanelW, kDetailActionPanelH, panelFill, panelFrame);
    netplay::font::DrawTextCentered5x7(surface, "SET DETAILS", kDetailHeaderPanelX + 2, kDetailHeaderPanelX + kDetailHeaderPanelW - 2, kDetailHeaderPanelY + 4, 1, 1, titleColor);
    netplay::font::DrawTextCentered5x7(surface, "ACTIONS", kDetailActionPanelX + 2, kDetailActionPanelX + kDetailActionPanelW - 2, kDetailActionPanelY + 3, 1, 1, titleColor);

    const BattleLogSession* session = GetDetailSession();
    if (session == nullptr)
    {
        netplay::font::DrawTextCentered5x7(surface, "No set selected.", kDetailHeaderPanelX + 2, kDetailHeaderPanelX + kDetailHeaderPanelW - 2, kDetailHeaderPanelY + 24, 1, 1, alertColor);
        return;
    }

    const std::string leftName = AbbreviateForDisplay(session->p1Name, 15);
    const std::string rightName = AbbreviateForDisplay(session->p2Name, 15);
    const std::string matchup =
        leftName + " "
        + std::to_string(GetSessionFinalScoreLeft(*session))
        + "-" + std::to_string(GetSessionFinalScoreRight(*session))
        + " "
        + rightName;
    const std::string dateTimeText = FormatDateTime(session->date, session->time);
    const std::string durationText =
        "Games: "
        + std::to_string(static_cast<int>(session->matches.size()))
        + "  Playtime: "
        + FormatDurationShort(session->totalDurationSeconds);

    netplay::font::DrawTextCentered5x7(
        surface,
        matchup,
        kDetailHeaderPanelX + 8,
        kDetailHeaderPanelX + kDetailHeaderPanelW - 8,
        kDetailHeaderPanelY + 21,
        1,
        1,
        textColor);
    netplay::font::DrawTextLeft5x7(surface, dateTimeText, kDetailHeaderPanelX + 8, kDetailHeaderPanelX + kDetailHeaderPanelW - 8, kDetailHeaderPanelY + 37, 1, 1, dimColor);
    netplay::font::DrawTextRight5x7(surface, durationText, kDetailHeaderPanelX + 8, kDetailHeaderPanelX + kDetailHeaderPanelW - 8, kDetailHeaderPanelY + 37, 1, 1, dimColor);
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
    const std::array<std::string, 5> labels = {{
        g_state.summaryMode == SummaryMode::Search ? std::string("VIEW SETS") : std::string("MY SETS"),
        "SEARCH",
        "ALL SETS",
        "REFRESH",
        "BACK",
    }};

    constexpr int kButtonCount = 5;
    constexpr int kButtonsTotalWidth =
        kButtonCount * kSummaryActionButtonW + (kButtonCount - 1) * kSummaryActionButtonGap;
    const int buttonsX = kSummaryActionPanelX + (kSummaryActionPanelW - kButtonsTotalWidth) / 2;
    const int rowY = kSummaryActionPanelY + 11;

    for (size_t index = 0; index < labels.size(); ++index)
    {
        const bool isSelected = static_cast<int>(index) == selection;
        const int rowX = buttonsX + static_cast<int>(index) * (kSummaryActionButtonW + kSummaryActionButtonGap);
        DrawRowBoxAt(surface, rowX, rowY, kSummaryActionButtonW, kSummaryActionButtonH, isSelected, rowFill, rowFrame, selectedFill);
        netplay::font::DrawTextCentered5x7(
            surface,
            labels[index],
            rowX + 2,
            rowX + kSummaryActionButtonW - 2,
            rowY + 3,
            1,
            1,
            isSelected ? selectedText : normalText);
    }
}

void DrawBrowserRows(
    const netplay::font::IndexedSurfaceView& surface,
    uint32_t screenContext,
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
    uint8_t alertColor)
{
    (void)screenContext;
    (void)chipFill;
    (void)chipFrame;
    (void)chipText;

    for (int slot = 0; slot < netplay::menu::kBattleLogVisibleSessionRows; ++slot)
    {
        const bool isSelected = slot == selection;
        const int rowY = kBrowserSessionRowY[static_cast<size_t>(slot)];
        DrawRowBoxAt(surface, kContentColumnX, rowY, kContentColumnW, kBrowserRowH, isSelected, rowFill, rowFrame, selectedFill);

        const BattleLogSession* session = GetSessionByIndex(GetSessionIndexForVisibleSlot(slot));
        if (session == nullptr)
        {
            if (slot == 0 && GetBrowserResultCount() == 0)
            {
                netplay::font::DrawTextCentered5x7(
                    surface,
                    "<no matching sets>",
                    kContentColumnX + 4,
                    kContentColumnX + kContentColumnW - 4,
                    rowY + 2,
                    1,
                    1,
                    isSelected ? selectedText : alertColor);
            }
            continue;
        }

        std::vector<std::string> p1Characters;
        std::vector<std::string> p2Characters;
        CollectDrawableBrowserCharacters(*session, &p1Characters, &p2Characters);
        const BrowserRowLayout layout =
            ComputeBrowserRowLayout(*session, p1Characters.size(), p2Characters.size());

        netplay::font::DrawTextLeft5x7(
            surface,
            layout.dateTimeText,
            layout.dateLeft,
            layout.dateRight,
            rowY + 2,
            1,
            1,
            dimText);
        netplay::font::DrawTextLeft5x7(
            surface,
            layout.leftNameText,
            layout.p1NameLeft,
            layout.p1NameRight,
            rowY + 2,
            1,
            1,
            isSelected ? selectedText : normalText);
        netplay::font::DrawTextCentered5x7(
            surface,
            layout.scoreText,
            layout.scoreLeft,
            layout.scoreRight,
            rowY + 2,
            1,
            1,
            isSelected ? selectedText : normalText);
        netplay::font::DrawTextLeft5x7(
            surface,
            layout.rightNameText,
            layout.p2NameLeft,
            layout.p2NameRight,
            rowY + 2,
            1,
            1,
            isSelected ? selectedText : normalText);
    }

    static const std::array<const char*, 4> kControls = {
        "PREV",
        "NEXT",
        "FILTERS",
        "BACK",
    };

    constexpr int kButtonCount = 4;
    constexpr int kButtonsTotalWidth =
        kButtonCount * kBrowserActionButtonW + (kButtonCount - 1) * kBrowserActionButtonGap;
    const int buttonsX = kBrowserActionPanelX + (kBrowserActionPanelW - kButtonsTotalWidth) / 2;
    const int rowY = kBrowserActionPanelY + 11;

    for (int control = 0; control < 4; ++control)
    {
        const int selectionIndex = netplay::menu::kBattleLogVisibleSessionRows + control;
        const bool isSelected = selectionIndex == selection;
        const int rowX = buttonsX + control * (kBrowserActionButtonW + kBrowserActionButtonGap);
        DrawRowBoxAt(surface, rowX, rowY, kBrowserActionButtonW, kBrowserActionButtonH, isSelected, rowFill, rowFrame, selectedFill);
        netplay::font::DrawTextCentered5x7(
            surface,
            kControls[static_cast<size_t>(control)],
            rowX + 2,
            rowX + kBrowserActionButtonW - 2,
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

    const std::array<FilterRow, 10> rows = {{
        {"PLAYER", GetFilterFieldValue(FilterEditField::PlayerName, true)},
        {"OPPONENT", GetFilterFieldValue(FilterEditField::OpponentName, true)},
        {"PLAYER CHARACTER", g_state.draftFilter.playerCharacter},
        {"OPPONENT CHARACTER", g_state.draftFilter.opponentCharacter},
        {"SET TYPE", g_state.draftFilter.setStatus},
        {"GAMES IN SET", g_state.draftFilter.gameCount},
        {"CHAR CHANGES", g_state.draftFilter.characterSwitches},
        {"APPLY", ""},
        {"RESET", ""},
        {"BACK", ""},
    }};

    for (size_t index = 0; index < rows.size(); ++index)
    {
        const bool isSelected = static_cast<int>(index) == selection;
        const int rowY = index < 7
            ? kFilterFieldRowY[index]
            : kFilterActionRowY[index - 7];
        const bool isActionRow = index >= 7;
        const int rowX = isActionRow ? (kActionStackX + 4) : kContentColumnX;
        const int rowW = isActionRow ? (kActionStackW - 8) : kContentColumnW;
        DrawRowBoxAt(surface, rowX, rowY, rowW, kFilterRowH, isSelected, rowFill, rowFrame, selectedFill);
        if (isActionRow)
        {
            netplay::font::DrawTextCentered5x7(
                surface,
                rows[index].label,
                rowX + 4,
                rowX + rowW - 4,
                rowY + 2,
                1,
                1,
                isSelected ? selectedText : normalText);
        }
        else
        {
            netplay::font::DrawTextLeft5x7(
                surface,
                rows[index].label,
                rowX + 6,
                rowX + 118,
                rowY + 2,
                1,
                1,
                isSelected ? selectedText : normalText);
        }
        if (!rows[index].value.empty() && !isActionRow)
        {
            netplay::font::DrawTextRight5x7(
                surface,
                rows[index].value,
                rowX + 122,
                rowX + rowW - 6,
                rowY + 2,
                1,
                1,
                isSelected ? selectedText : dimText);
        }
    }
}

void DrawDetailRows(
    const netplay::font::IndexedSurfaceView& surface,
    uint32_t screenContext,
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
    (void)screenContext;
    (void)chipFill;
    (void)chipFrame;
    (void)chipText;

    const BattleLogSession* session = GetDetailSession();

    for (int slot = 0; slot < netplay::menu::kBattleLogVisibleGameRows; ++slot)
    {
        const bool isSelected = slot == selection;
        const int rowY = kDetailGameRowY[static_cast<size_t>(slot)];
        DrawRowBoxAt(surface, kContentColumnX, rowY, kContentColumnW, kDetailRowH, isSelected, rowFill, rowFrame, selectedFill);

        const BattleLogMatch* match = GetMatchForVisibleDetailSlot(slot);
        if (match == nullptr)
        {
            if (slot == 0)
            {
                netplay::font::DrawTextCentered5x7(
                    surface,
                    "<no games>",
                    kContentColumnX + 4,
                    kContentColumnX + kContentColumnW - 4,
                    rowY + 2,
                    1,
                    1,
                    isSelected ? selectedText : dimText);
            }
            continue;
        }

        const int absoluteMatchIndex =
            g_state.detailPage * netplay::menu::kBattleLogVisibleGameRows + slot + 1;
        const DetailRowLayout layout = ComputeDetailRowLayout(
            *match,
            absoluteMatchIndex,
            session,
            FindCharacterSprite(match->p1CharacterDisplay) != nullptr,
            FindCharacterSprite(match->p2CharacterDisplay) != nullptr);

        netplay::font::DrawTextLeft5x7(surface, layout.labelText, layout.labelLeft, layout.labelRight, rowY + 2, 1, 1, dimText);
        netplay::font::DrawTextLeft5x7(
            surface,
            layout.leftNameText,
            layout.p1NameLeft,
            layout.p1NameRight,
            rowY + 2,
            1,
            1,
            isSelected ? selectedText : normalText);
        netplay::font::DrawTextCentered5x7(
            surface,
            layout.roundsText,
            layout.roundsLeft,
            layout.roundsRight,
            rowY + 2,
            1,
            1,
            isSelected ? selectedText : normalText);
        netplay::font::DrawTextLeft5x7(
            surface,
            layout.rightNameText,
            layout.p2NameLeft,
            layout.p2NameRight,
            rowY + 2,
            1,
            1,
            isSelected ? selectedText : normalText);
        netplay::font::DrawTextRight5x7(
            surface,
            layout.durationText,
            layout.durationLeft,
            layout.durationRight,
            rowY + 2,
            1,
            1,
            dimText);
    }

    static const std::array<const char*, 3> kControls = {
        "PREV",
        "NEXT",
        "BACK",
    };
    constexpr int kButtonCount = 3;
    constexpr int kButtonsTotalWidth =
        kButtonCount * kDetailActionButtonW + (kButtonCount - 1) * kDetailActionButtonGap;
    const int buttonsX = kDetailActionPanelX + (kDetailActionPanelW - kButtonsTotalWidth) / 2;
    const int rowY = kDetailActionPanelY + 11;

    for (int control = 0; control < 3; ++control)
    {
        const int selectionIndex = netplay::menu::kBattleLogVisibleGameRows + control;
        const bool isSelected = selectionIndex == selection;
        const int rowX = buttonsX + control * (kDetailActionButtonW + kDetailActionButtonGap);
        DrawRowBoxAt(surface, rowX, rowY, kDetailActionButtonW, kDetailActionButtonH, isSelected, rowFill, rowFrame, selectedFill);
        netplay::font::DrawTextCentered5x7(
            surface,
            kControls[static_cast<size_t>(control)],
            rowX + 2,
            rowX + kDetailActionButtonW - 2,
            rowY + 3,
            1,
            1,
            isSelected ? selectedText : normalText);
    }
}
} // namespace

void ShutdownRenderOverlay()
{
    ShutdownD3d9OverlayHook();
}

bool EnsureGameplayOverlayHook()
{
    return EnsureD3d9OverlayHookInstalled();
}

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
    g_state.activeFilter.setStatus = GetDefaultBattleLogSetStatus();
    g_state.activeFilter.gameCount = "Any";
    g_state.activeFilter.characterSwitches = "Any";
    g_state.draftFilter = g_state.activeFilter;
    g_state.browserListSelection = 0;
    g_state.browserActionSelection = netplay::menu::kBattleLogVisibleSessionRows;
    g_state.filtersFieldSelection = 0;
    g_state.filtersActionSelection = 7;
    g_state.detailListSelection = 0;
    g_state.detailActionSelection = netplay::menu::kBattleLogVisibleGameRows;
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
    PrimeRenderAssetDiagnostics();
    ResetRenderAssetFrameState();
    g_state.view = View::Summary;
    RebuildMenuEntries();
    return true;
}

void LeaveMenu()
{
    ReleaseRenderAssets();
    ResetRenderAssetFrameState();
    ResetState();
}

std::string BuildRowLabel(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::BattleLogBrowseMine:
        return g_state.summaryMode == SummaryMode::Search ? "VIEW SETS" : "BROWSE MY SETS";
    case NetplayMenuAction::BattleLogSearchFilters:
        return "SEARCH";
    case NetplayMenuAction::BattleLogBrowseAll:
        return "BROWSE ALL SETS";
    case NetplayMenuAction::BattleLogRefresh:
        return "REFRESH LOG";
    case NetplayMenuAction::BattleLogEditPlayerName:
        return "PLAYER";
    case NetplayMenuAction::BattleLogEditOpponentName:
        return "OPPONENT";
    case NetplayMenuAction::BattleLogPlayerCharacter:
        return "PLAYER CHARACTER";
    case NetplayMenuAction::BattleLogOpponentCharacter:
        return "OPPONENT CHARACTER";
    case NetplayMenuAction::BattleLogSetStatus:
        return "SET TYPE";
    case NetplayMenuAction::BattleLogGameCount:
        return "GAMES IN SET";
    case NetplayMenuAction::BattleLogCharacterSwitches:
        return "CHAR CHANGES";
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
        && action <= NetplayMenuAction::BattleLogGame6)
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
    case NetplayMenuAction::BattleLogSetStatus:
        return g_state.draftFilter.setStatus;
    case NetplayMenuAction::BattleLogGameCount:
        return g_state.draftFilter.gameCount;
    case NetplayMenuAction::BattleLogCharacterSwitches:
        return g_state.draftFilter.characterSwitches;
    default:
        return {};
    }
}

std::string BuildFooterText(NetplayMenuAction selectedAction)
{
    std::string footer;
    std::string hint;
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
            footer =
                selectedAction == NetplayMenuAction::BattleLogSearchFilters
                    ? "Search by player, matchup, characters, set type, games, and character changes."
                    : "Refine this set list by player, matchup, characters, set type, games, and character changes.";
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
        case NetplayMenuAction::BattleLogSetStatus:
            footer = "Choose any set, only played sets, or only empty 0-0 headers.";
            break;
        case NetplayMenuAction::BattleLogGameCount:
            footer = "Filter by how many games were logged inside the set.";
            break;
        case NetplayMenuAction::BattleLogCharacterSwitches:
            footer = "Filter sets where nobody switched, or where someone changed character.";
            break;
        case NetplayMenuAction::BattleLogApplyFilters:
            footer =
                g_state.filterReturnView == View::Summary
                    ? "Run this search and open a summary before browsing sets."
                    : "Run this query in the set browser.";
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
                && selectedAction <= NetplayMenuAction::BattleLogGame6)
            {
                footer = "Per-game log entry inside this set.";
            }
            break;
        }
    }

    if (!g_state.edit.active)
    {
        if (g_state.view == View::Summary)
        {
            hint =
                g_state.summaryMode == SummaryMode::Search
                    ? "Press C to Switch to Profile"
                    : (g_state.summaryMode == SummaryMode::FullLog
                        ? "Press C to Switch to Profile"
                        : "Press C to Switch to Full Summary");
        }
        else if (g_state.view == View::Filters)
        {
            hint =
                selectedAction == NetplayMenuAction::BattleLogApplyFilters
                    || selectedAction == NetplayMenuAction::BattleLogResetFilters
                    || selectedAction == NetplayMenuAction::BattleLogBack
                ? "Press C to Switch to Fields"
                : "Press C to Switch to Actions";
        }
        else if (g_state.view == View::Browser)
        {
            hint =
                selectedAction >= NetplayMenuAction::BattleLogBrowserPrevPage
                    && selectedAction <= NetplayMenuAction::BattleLogBack
                ? "Press C to Switch to Set List"
                : "Press C to Switch to Actions";
        }
        else if (g_state.view == View::SetDetail)
        {
            hint =
                selectedAction >= NetplayMenuAction::BattleLogDetailPrevPage
                    && selectedAction <= NetplayMenuAction::BattleLogBack
                ? "Press C to Switch to Games"
                : "Press C to Switch to Actions";
        }
    }

    if (HasStatusMessage())
    {
        if (hint.empty())
        {
            return g_state.statusMessage;
        }
        return g_state.statusMessage + "\n" + hint;
    }

    if (footer.empty())
    {
        return hint;
    }
    if (hint.empty())
    {
        return footer;
    }

    return footer + "\n" + hint;
}

bool HandleVerticalNavigation(int currentSelection, int delta, int* outNextSelection)
{
    if (outNextSelection == nullptr || delta == 0)
    {
        return false;
    }

    switch (g_state.view)
    {
    case View::Browser:
        if (currentSelection < netplay::menu::kBattleLogVisibleSessionRows)
        {
            const int count = GetBrowserSelectableSessionCount();
            const int local = (currentSelection + delta + count) % count;
            *outNextSelection = local;
            return true;
        }
        else
        {
            constexpr int kActionCount = 4;
            const int firstAction = netplay::menu::kBattleLogVisibleSessionRows;
            const int local = ((currentSelection - firstAction) + delta + kActionCount) % kActionCount;
            *outNextSelection = firstAction + local;
            return true;
        }
    case View::Filters:
        if (currentSelection < 7)
        {
            constexpr int kFieldCount = 7;
            *outNextSelection = (currentSelection + delta + kFieldCount) % kFieldCount;
            return true;
        }
        else
        {
            constexpr int kActionCount = 3;
            const int firstAction = 7;
            const int local = ((currentSelection - firstAction) + delta + kActionCount) % kActionCount;
            *outNextSelection = firstAction + local;
            return true;
        }
    case View::SetDetail:
        if (currentSelection < netplay::menu::kBattleLogVisibleGameRows)
        {
            const int count = GetDetailSelectableGameCount();
            const int local = (currentSelection + delta + count) % count;
            *outNextSelection = local;
            return true;
        }
        else
        {
            constexpr int kActionCount = 3;
            const int firstAction = netplay::menu::kBattleLogVisibleGameRows;
            const int local = ((currentSelection - firstAction) + delta + kActionCount) % kActionCount;
            *outNextSelection = firstAction + local;
            return true;
        }
    case View::Summary:
    default:
        return false;
    }
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

    bool consumed = false;
    bool suppressDefaultMenuInput = false;
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        const int selectedIndex = ClampSelectionForCurrentView(
            static_cast<int>(
                *reinterpret_cast<int8_t*>(screenContext + netplay::constants::kOffsetMenuSelection)));
        const NetplayMenuAction selectedAction =
            selectedIndex < g_state.spec.entryCount
            ? g_state.entries[static_cast<size_t>(selectedIndex)].action
            : NetplayMenuAction::BattleLogBack;

        const int8_t horizontal = static_cast<int8_t>(inputBytes[playerIndex + 12]);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);
        const uint8_t buttonC = inputBytes[playerIndex + 20];
        const int8_t horizontalDir = horizontal > 0 ? 1 : (horizontal < 0 ? -1 : 0);
        const int8_t verticalDir = vertical > 0 ? 1 : (vertical < 0 ? -1 : 0);
        const bool buttonCEdge = (buttonC == 1) && (g_state.lastButtonC[static_cast<size_t>(playerIndex)] == 0);
        const bool horizontalEdge =
            (horizontalDir != 0) && (horizontalDir != g_state.lastHorizontalDir[static_cast<size_t>(playerIndex)]);
        const bool verticalEdge =
            (verticalDir != 0) && (verticalDir != g_state.lastVerticalDir[static_cast<size_t>(playerIndex)]);

        suppressDefaultMenuInput = suppressDefaultMenuInput
            || horizontalDir != 0
            || verticalDir != 0
            || buttonC != 0;

        if (buttonCEdge)
        {
            int nextSelection = selectedIndex;
            if (TogglePaneSelection(selectedIndex, &nextSelection))
            {
                *inactivityCounter = 0;
                SetSelection(screenContext, nextSelection);
                hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
                consumed = true;
                goto next_player;
            }
        }

        if (g_state.view == View::Summary)
        {
            if (buttonCEdge)
            {
                *inactivityCounter = 0;
                if (g_state.summaryMode == SummaryMode::Search)
                {
                    g_state.summaryMode = SummaryMode::Profile;
                }
                else if (g_state.summaryMode == SummaryMode::FullLog)
                {
                    g_state.summaryMode = SummaryMode::Profile;
                }
                else
                {
                    g_state.summaryMode = SummaryMode::FullLog;
                }
                RefreshDisplayedSummary();
                hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
                consumed = true;
                goto next_player;
            }

            int step = 0;
            if (horizontalEdge)
            {
                step = horizontalDir;
            }
            else if (verticalEdge)
            {
                step = verticalDir;
            }

            if (step != 0)
            {
                *inactivityCounter = 0;
                const int nextSelection = (selectedIndex + step + 5) % 5;
                SetSelection(screenContext, nextSelection);
                hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
                consumed = true;
            }
            goto next_player;
        }

        if ((selectedAction == NetplayMenuAction::BattleLogPlayerCharacter
                || selectedAction == NetplayMenuAction::BattleLogOpponentCharacter
                || selectedAction == NetplayMenuAction::BattleLogSetStatus
                || selectedAction == NetplayMenuAction::BattleLogGameCount
                || selectedAction == NetplayMenuAction::BattleLogCharacterSwitches)
            && horizontalEdge)
        {
            *inactivityCounter = 0;
            const int delta = horizontalDir;
            if (selectedAction == NetplayMenuAction::BattleLogPlayerCharacter)
            {
                CycleCharacterOption(&g_state.draftFilter.playerCharacter, delta);
            }
            else if (selectedAction == NetplayMenuAction::BattleLogOpponentCharacter)
            {
                CycleCharacterOption(&g_state.draftFilter.opponentCharacter, delta);
            }
            else if (selectedAction == NetplayMenuAction::BattleLogSetStatus)
            {
                CycleOptionValue(&g_state.draftFilter.setStatus, GetSetStatusOptions(), delta);
            }
            else if (selectedAction == NetplayMenuAction::BattleLogGameCount)
            {
                CycleOptionValue(&g_state.draftFilter.gameCount, GetGameCountFilterOptions(), delta);
            }
            else
            {
                CycleOptionValue(&g_state.draftFilter.characterSwitches, GetSwitchFilterOptions(), delta);
            }
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
            consumed = true;
        }
        else if (g_state.view == View::Browser
            && selectedIndex < netplay::menu::kBattleLogVisibleSessionRows
            && horizontalEdge)
        {
            bool pageChanged = false;
            if (horizontalDir < 0 && g_state.browserPage > 0)
            {
                --g_state.browserPage;
                pageChanged = true;
                SetStatusMessage("Previous page.");
            }
            else if (horizontalDir > 0 && g_state.browserPage + 1 < GetBrowserPageCount())
            {
                ++g_state.browserPage;
                pageChanged = true;
                SetStatusMessage("Next page.");
            }

            if (pageChanged)
            {
                *inactivityCounter = 0;
                RebuildMenuEntries();
                SetSelection(screenContext, g_state.browserListSelection);
                hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
                consumed = true;
            }
        }
        else if (g_state.view == View::Browser
            && selectedIndex >= netplay::menu::kBattleLogVisibleSessionRows
            && horizontalEdge)
        {
            *inactivityCounter = 0;
            constexpr int kActionCount = 4;
            const int firstAction = netplay::menu::kBattleLogVisibleSessionRows;
            const int local = ((selectedIndex - firstAction) + horizontalDir + kActionCount) % kActionCount;
            SetSelection(screenContext, firstAction + local);
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
            consumed = true;
        }
        else if (g_state.view == View::SetDetail
            && selectedIndex < netplay::menu::kBattleLogVisibleGameRows
            && horizontalEdge)
        {
            bool pageChanged = false;
            if (horizontalDir < 0 && g_state.detailPage > 0)
            {
                --g_state.detailPage;
                pageChanged = true;
                SetStatusMessage("Previous page.");
            }
            else if (horizontalDir > 0 && g_state.detailPage + 1 < GetDetailPageCount())
            {
                ++g_state.detailPage;
                pageChanged = true;
                SetStatusMessage("Next page.");
            }

            if (pageChanged)
            {
                *inactivityCounter = 0;
                RebuildMenuEntries();
                SetSelection(screenContext, g_state.detailListSelection);
                hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
                consumed = true;
            }
        }
        else if (g_state.view == View::SetDetail
            && selectedIndex >= netplay::menu::kBattleLogVisibleGameRows
            && horizontalEdge)
        {
            *inactivityCounter = 0;
            constexpr int kActionCount = 3;
            const int firstAction = netplay::menu::kBattleLogVisibleGameRows;
            const int local = ((selectedIndex - firstAction) + horizontalDir + kActionCount) % kActionCount;
            SetSelection(screenContext, firstAction + local);
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
            consumed = true;
        }
        else if (verticalEdge)
        {
            *inactivityCounter = 0;
            int nextSelection = selectedIndex;
            if (HandleVerticalNavigation(selectedIndex, verticalDir, &nextSelection))
            {
                SetSelection(screenContext, nextSelection);
                hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
                consumed = true;
            }
        }

next_player:
        g_state.lastHorizontalDir[static_cast<size_t>(playerIndex)] = horizontalDir;
        g_state.lastVerticalDir[static_cast<size_t>(playerIndex)] = verticalDir;
        g_state.lastButtonC[static_cast<size_t>(playerIndex)] = buttonC;
    }

    return consumed || suppressDefaultMenuInput;
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
        mod::Log("BattleLog::HandleCancel: leaving summary, restoring netplay assets");
        (void)hooks::LoadNetplayAssets(screenContext);
        hooks::StartMenuSlideTransition(
            screenContext,
            NetplayMenuId::Main,
            hooks::g_netplayMenuState.mainSelection,
            -1);
        return true;
    case View::Browser:
        if (*GetSelectionStorage(View::Browser) < netplay::menu::kBattleLogVisibleSessionRows)
        {
            SetSelection(screenContext, g_state.browserActionSelection);
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
            return true;
        }
        SwitchView(screenContext, View::Summary, -1);
        return true;
    case View::Filters:
        if (*GetSelectionStorage(View::Filters) < 7)
        {
            SetSelection(screenContext, g_state.filtersActionSelection);
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
            return true;
        }
        SwitchView(screenContext, g_state.filterReturnView, -1);
        return true;
    case View::SetDetail:
        if (*GetSelectionStorage(View::SetDetail) < netplay::menu::kBattleLogVisibleGameRows)
        {
            SetSelection(screenContext, g_state.detailActionSelection);
            hooks::PlayUiSound(screenContext, netplay::constants::kSfxMove);
            return true;
        }
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
        if (g_state.summaryMode == SummaryMode::Search)
        {
            g_state.browserPage = 0;
            RebuildFilteredSessionIndices();
            if (GetBrowserResultCount() == 0)
            {
                SetStatusMessage("No sets matched the current search.");
            }
            (void)EnsureRenderAssetsLoaded(screenContext);
            SwitchView(screenContext, View::Browser, 0);
            return true;
        }

        g_state.activeFilter = {};
        g_state.activeFilter.playerName = g_state.currentNickname;
        g_state.activeFilter.playerCharacter = "All";
        g_state.activeFilter.opponentCharacter = "All";
        g_state.activeFilter.setStatus = GetDefaultBattleLogSetStatus();
        g_state.activeFilter.gameCount = "Any";
        g_state.activeFilter.characterSwitches = "Any";
        g_state.draftFilter = g_state.activeFilter;
        g_state.summaryMode = SummaryMode::Profile;
        g_state.browserPage = 0;
        RebuildFilteredSessionIndices();
        if (GetBrowserResultCount() == 0)
        {
            SetStatusMessage("No sets matched your nickname.");
        }
        (void)EnsureRenderAssetsLoaded(screenContext);
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
        g_state.activeFilter.setStatus = GetDefaultBattleLogSetStatus();
        g_state.activeFilter.gameCount = "Any";
        g_state.activeFilter.characterSwitches = "Any";
        g_state.draftFilter = g_state.activeFilter;
        g_state.summaryMode = SummaryMode::FullLog;
        g_state.browserPage = 0;
        RebuildFilteredSessionIndices();
        (void)EnsureRenderAssetsLoaded(screenContext);
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

    case NetplayMenuAction::BattleLogSetStatus:
        CycleOptionValue(&g_state.draftFilter.setStatus, GetSetStatusOptions(), +1);
        return true;

    case NetplayMenuAction::BattleLogGameCount:
        CycleOptionValue(&g_state.draftFilter.gameCount, GetGameCountFilterOptions(), +1);
        return true;

    case NetplayMenuAction::BattleLogCharacterSwitches:
        CycleOptionValue(&g_state.draftFilter.characterSwitches, GetSwitchFilterOptions(), +1);
        return true;

    case NetplayMenuAction::BattleLogApplyFilters:
        NormalizeFilter(&g_state.draftFilter);
        SanitizeFilterCharacters(&g_state.draftFilter);
        g_state.activeFilter = g_state.draftFilter;
        g_state.browserPage = 0;
        RebuildFilteredSessionIndices();
        g_state.summaryMode =
            HasSearchSummaryFilter(g_state.activeFilter)
                ? SummaryMode::Search
                : (IsDefaultAllFilter(g_state.activeFilter) ? SummaryMode::FullLog : SummaryMode::Profile);
        RefreshDisplayedSummary();
        if (GetBrowserResultCount() == 0)
        {
            SetStatusMessage("No sets matched the current filter.");
        }
        if (g_state.filterReturnView == View::Summary)
        {
            SwitchView(screenContext, View::Summary, 0);
        }
        else
        {
            (void)EnsureRenderAssetsLoaded(screenContext);
            SwitchView(screenContext, View::Browser, 0);
        }
        return true;

    case NetplayMenuAction::BattleLogResetFilters:
        g_state.draftFilter = {};
        g_state.draftFilter.playerCharacter = "All";
        g_state.draftFilter.opponentCharacter = "All";
        g_state.draftFilter.setStatus = GetDefaultBattleLogSetStatus();
        g_state.draftFilter.gameCount = "Any";
        g_state.draftFilter.characterSwitches = "Any";
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
        && action <= NetplayMenuAction::BattleLogGame6)
    {
        return true;
    }

    return false;
}

bool DrawOverlayGdi(uint32_t screenContext, bool /*allowWindowDc*/)
{
    (void)((g_state.view == View::Browser) && EnsureRenderAssetsLoaded(screenContext));

    netplay::draw::LockedMenuSurface lockedSurface;
    if (!netplay::draw::AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        mod::Log("BattleLog::DrawOverlayGdi: failed to lock draw surface");
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
    const uint8_t alertColor = netplay::draw::ResolveBestPaletteColor(screenContext, 255, 176, 110);
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
        DrawSummaryPanel(surface, panelFill, panelFrame, titleColor, textColor, dimColor, alertColor);
        DrawSummaryRows(surface, selection, rowFill, rowFrame, selectedFill, textColor, selectedText);
        break;
    case View::Browser:
        DrawBrowserPanel(surface, panelFill, panelFrame, titleColor, textColor, dimColor, alertColor);
        DrawBrowserRows(surface, screenContext, selection, rowFill, rowFrame, selectedFill, textColor, selectedText, chipFill, chipFrame, chipText, dimColor, alertColor);
        break;
    case View::Filters:
        DrawFiltersPanel(surface, panelFill, panelFrame, titleColor, textColor, dimColor);
        DrawFilterRows(surface, selection, rowFill, rowFrame, selectedFill, textColor, selectedText, dimColor);
        break;
    case View::SetDetail:
        DrawDetailPanel(surface, panelFill, panelFrame, titleColor, textColor, dimColor, alertColor);
        DrawDetailRows(surface, screenContext, selection, rowFill, rowFrame, selectedFill, textColor, selectedText, chipFill, chipFrame, chipText, dimColor);
        break;
    }

    netplay::draw::ReleaseMenuDrawSurfaceLock(lockedSurface);
    return true;
}

bool DrawImageOverlayGdi(uint32_t screenContext, bool allowWindowDc)
{
    if (g_state.view != View::Browser && g_state.view != View::SetDetail)
    {
        return false;
    }

    if (hooks::g_debugOverlay.open)
    {
        return true;
    }

    if (!EnsureRenderAssetsLoaded(screenContext))
    {
        return false;
    }

    if (EnsureD3d9OverlayHookInstalled())
    {
        return true;
    }

    if (!g_d3dOverlay.firstHookUnavailableLogged)
    {
        mod::Log(
            "BattleLog::DrawImageOverlayGdi: D3D9 hook unavailable, falling back allowWindowDc=%d hookAttempted=%d hookInstalled=%d endSceneObserved=%d",
            allowWindowDc ? 1 : 0,
            g_d3dOverlay.hookAttempted ? 1 : 0,
            g_d3dOverlay.hookInstalled ? 1 : 0,
            g_d3dOverlay.endSceneObserved ? 1 : 0);
        g_d3dOverlay.firstHookUnavailableLogged = true;
    }

    return DrawBrowserIconsGdi(screenContext, allowWindowDc);
}

uint8_t GetMenuDetailForStateExport()
{
    switch (g_state.view)
    {
    case View::Browser:
        return static_cast<uint8_t>(EFZ_MENU_DETAIL_BATTLE_LOG_BROWSER);
    case View::Filters:
        return static_cast<uint8_t>(EFZ_MENU_DETAIL_BATTLE_LOG_FILTERS);
    case View::SetDetail:
        return static_cast<uint8_t>(EFZ_MENU_DETAIL_BATTLE_LOG_SET_DETAILS);
    case View::Summary:
    default:
        switch (g_state.summaryMode)
        {
        case SummaryMode::FullLog:
            return static_cast<uint8_t>(EFZ_MENU_DETAIL_BATTLE_LOG_SUMMARY_FULL);
        case SummaryMode::Search:
            return static_cast<uint8_t>(EFZ_MENU_DETAIL_BATTLE_LOG_SUMMARY_SEARCH);
        case SummaryMode::Profile:
        default:
            return static_cast<uint8_t>(EFZ_MENU_DETAIL_BATTLE_LOG_SUMMARY_PROFILE);
        }
    }
}
} // namespace netplay::battle_log
