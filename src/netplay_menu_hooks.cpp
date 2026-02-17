#include "netplay_menu_hooks.h"

#include "logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr uintptr_t kEfzImageBase = 0x00400000;

constexpr uintptr_t kVaProcessPlayerInput = 0x00406590;
constexpr uintptr_t kVaPlaySoundEffect = 0x00406860;
constexpr uintptr_t kVaPlayBackgroundMusic = 0x004068B0;
constexpr uintptr_t kVaStopBackgroundMusic = 0x00406A10;
constexpr uintptr_t kVaLoadCompressedImageFile = 0x00406DB0;
constexpr uintptr_t kVaFadeWithSoundAdjustment = 0x00759C90;
constexpr uintptr_t kVaFadeScreenEffect = 0x00759E20;
constexpr uintptr_t kVaPerformSlideAnimation = 0x0075DA30;
constexpr uintptr_t kVaLoadBgrColorsFromRawFile = 0x0040B760;
constexpr uintptr_t kVaReadPixelValue = 0x0040BCC0;
constexpr uintptr_t kVaSetPalette = 0x0040BD30;
constexpr uintptr_t kVaBlitSurfaceWithTransparency = 0x00409A90;
constexpr uintptr_t kVaPresentFrameToScreen = 0x0040B5E0;

constexpr uintptr_t kVaTitleRender = 0x007764B0;
constexpr uintptr_t kVaUpdateTitleScreenLogic = 0x00775FB0;
constexpr uintptr_t kVaTitleExitInputCheck = 0x00776760;
constexpr uintptr_t kVaTitleVtableRender = 0x00789980;
constexpr uintptr_t kVaTitleVtableUpdate = 0x00789984;
constexpr uintptr_t kVaTitleCaseEpilogue = 0x00776483;

constexpr uintptr_t kVaWrapCmpMax = 0x0077612E;       // cmp eax, 6
constexpr uintptr_t kVaWrapClampNegative = 0x0077614B; // mov ... , 6
constexpr uintptr_t kVaSwitchCmpMax = 0x007761D5;      // cmp [ebp-0Ch], 6
constexpr uintptr_t kVaSwitchTableDisp = 0x007761E5;   // dword disp in jmp [ecx*4 + disp32]

constexpr uintptr_t kVaRenderPanelDestH = 0x00776578;     // push 62h
constexpr uintptr_t kVaRenderPanelSourceH = 0x007765A3;   // push 62h
constexpr uintptr_t kVaRenderPanelDestTop = 0x007765A7;   // push 82h (130)
constexpr uintptr_t kVaRenderHighlightDestBase = 0x0077664A; // add eax, 62h
constexpr uintptr_t kVaRenderHighlightScreenBase = 0x00776696; // add edx, 82h (130)

constexpr uint32_t kMenuDestTopPatched = 123; // Move list up by 7px for 8-row layout.

constexpr uintptr_t kVaCaseArcade = 0x007761E9;
constexpr uintptr_t kVaCaseVsCpu = 0x00776268;
constexpr uintptr_t kVaCaseVsHuman = 0x007762E3;
constexpr uintptr_t kVaCasePractice = 0x00776352;
constexpr uintptr_t kVaCaseReplay = 0x007763D1;
constexpr uintptr_t kVaCaseOptions = 0x00776432;
constexpr uintptr_t kVaCaseExit = 0x0077645F;

constexpr uint32_t kOffsetWindowHandle = 0x08;
constexpr uint32_t kOffsetGameSystem = 0x1C;
constexpr uint32_t kOffsetGraphicsContext = 0x20;
constexpr uint32_t kOffsetPalette = 46;
constexpr uint32_t kOffsetTransparentColor = 0x42E;
constexpr uint32_t kOffsetBackgroundSurface = 0x434;
constexpr uint32_t kOffsetObjectsSurface = 0x438;
constexpr uint32_t kOffsetMenuSelection = 0x43C;
constexpr uint32_t kOffsetMenuAnimCounter = 0x43E;
constexpr uint32_t kOffsetInputLatchP1 = 0x440;
constexpr uint32_t kOffsetInputLatchP2 = 0x441;
constexpr uint32_t kOffsetTitleMenuState = 0x442;
constexpr uint32_t kOffsetInactivityCounter = 0x444;
constexpr uint32_t kOffsetSlideAnimationY = 1876;
constexpr uint32_t kOffsetScreenInitState = 44;
constexpr uint32_t kOffsetScreenExitState = 45;
constexpr uint32_t kOffsetGraphicsPrimarySurface = 33283u * sizeof(uint32_t);
constexpr uint32_t kOffsetGraphicsBackBufferSurface = 33284u * sizeof(uint32_t);

constexpr uint32_t kVtableOffsetSurfaceGetDc = 68;       // IDirectDrawSurface7::GetDC
constexpr uint32_t kVtableOffsetSurfaceReleaseDc = 104;  // IDirectDrawSurface7::ReleaseDC
constexpr uint32_t kVtableOffsetSurfaceLock = 100;       // IDirectDrawSurface7::Lock
constexpr uint32_t kVtableOffsetSurfaceUnlock = 128;     // IDirectDrawSurface7::Unlock

constexpr unsigned short kSfxConfirm = 6;
constexpr unsigned short kSfxMove = 8;
constexpr unsigned short kNetplayBgmTrack = 8;
constexpr DWORD kNetplayFrameLogIntervalMs = 500;
constexpr uint16_t kDefaultNetplayPort = 7500;
constexpr int kDialogInputLimit = 63;

constexpr UINT kDialogControlPrompt = 2001;
constexpr UINT kDialogControlInput = 2002;

constexpr int kNetplayDefaultOptionCount = 4;
constexpr int kNetplayDefaultBackIndex = 3;
constexpr int kNetplayConfigOptionCount = 8;
constexpr int kNetplayConfigBackIndex = 7;
constexpr int kNetplayDefaultHighlightHeight = 14;
constexpr int kNetplayCompactMenuTopY = 95;
constexpr int kNetplayCompactMenuRowStep = 18;
constexpr int kNetplayNativeSlideDivisor = 3;
constexpr DWORD kInlineEditCaretBlinkMs = 350;
constexpr DWORD kInlineEditErrorDisplayMs = 1800;
constexpr size_t kInlineEditMaxPortLength = 5;
constexpr size_t kInlineEditMaxJoinAddressLength = 63;
constexpr size_t kInlineEditMaxNicknameLength = 20;

constexpr std::array<int, kNetplayConfigOptionCount> kDefaultHighlightSourceY = {261, 279, 297, 315, 333, 351, 369, 387};
constexpr std::array<int, kNetplayConfigOptionCount> kDefaultHighlightDestY = {95, 113, 131, 149, 167, 185, 203, 221};
constexpr std::array<int, kNetplayConfigOptionCount> kDefaultHighlightWidth = {320, 320, 320, 320, 320, 320, 320, 320};

struct NetplayRenderLayout
{
    std::array<int, kNetplayConfigOptionCount> highlightSourceY = kDefaultHighlightSourceY;
    std::array<int, kNetplayConfigOptionCount> highlightDestY = kDefaultHighlightDestY;
    std::array<int, kNetplayConfigOptionCount> highlightWidth = kDefaultHighlightWidth;
    int highlightSourceX = 0;
    int highlightDestX = 0;
    int highlightHeight = kNetplayDefaultHighlightHeight;
};

struct SpriteGlyph
{
    int srcX = 0;
    int srcY = 0;
    int width = 0;
    int height = 0;
    int advance = 0;
};

struct SpriteFont
{
    bool loaded = false;
    bool uppercaseInput = true;
    int lineHeight = 14;
    int letterSpacing = 1;
    std::unordered_map<char, SpriteGlyph> glyphs;
};

// Canonical row slots in netplay_ob (8-row config-style sheet lanes).
enum class NetplayObRow : uint8_t
{
    Host = 0,
    Join = 1,
    Nickname = 2,
    Address = 3,
    Port = 4,
    Reserved5 = 5,
    Reserved6 = 6,
    ReturnToTitle = 7,
};

constexpr int RowToIndex(NetplayObRow row)
{
    return static_cast<int>(row);
}

enum class NetplayMenuId : uint8_t
{
    Main = 0,
    Host = 1,
    Join = 2,
    Nickname = 3,
};

enum class NetplayMenuAction : uint8_t
{
    OpenHost = 0,
    OpenJoin = 1,
    OpenNickname = 2,
    LeaveNetplay = 3,
    HostStart = 4,
    HostEditPort = 5,
    BackToMain = 6,
    JoinConnect = 7,
    JoinEditAddress = 8,
    JoinEditPort = 9,
    NicknameEdit = 10,
};

struct NetplayMenuEntry
{
    NetplayMenuAction action = NetplayMenuAction::LeaveNetplay;
    int renderRow = 0;
    const char* debugLabel = "";
};

struct NetplayMenuSpec
{
    NetplayMenuId menuId = NetplayMenuId::Main;
    const char* headerLabel = "NETPLAY";
    const NetplayMenuEntry* entries = nullptr;
    int entryCount = 0;
    int defaultSelection = 0;
};

constexpr std::array<NetplayMenuEntry, 4> kMainMenuEntries = {{
    {NetplayMenuAction::OpenHost, RowToIndex(NetplayObRow::Host), "HOST"},
    {NetplayMenuAction::OpenJoin, RowToIndex(NetplayObRow::Join), "JOIN"},
    {NetplayMenuAction::OpenNickname, RowToIndex(NetplayObRow::Nickname), "CHANGE_NICKNAME"},
    {NetplayMenuAction::LeaveNetplay, RowToIndex(NetplayObRow::ReturnToTitle), "RETURN_TO_TITLE"},
}};

constexpr std::array<NetplayMenuEntry, 3> kHostMenuEntries = {{
    {NetplayMenuAction::HostStart, RowToIndex(NetplayObRow::Host), "HOST_START"},
    {NetplayMenuAction::HostEditPort, RowToIndex(NetplayObRow::Port), "HOST_PORT"},
    {NetplayMenuAction::BackToMain, RowToIndex(NetplayObRow::ReturnToTitle), "BACK"},
}};

constexpr std::array<NetplayMenuEntry, 4> kJoinMenuEntries = {{
    {NetplayMenuAction::JoinConnect, RowToIndex(NetplayObRow::Join), "JOIN_CONNECT"},
    {NetplayMenuAction::JoinEditAddress, RowToIndex(NetplayObRow::Address), "JOIN_ADDRESS"},
    {NetplayMenuAction::JoinEditPort, RowToIndex(NetplayObRow::Port), "JOIN_PORT"},
    {NetplayMenuAction::BackToMain, RowToIndex(NetplayObRow::ReturnToTitle), "BACK"},
}};

constexpr std::array<NetplayMenuEntry, 2> kNicknameMenuEntries = {{
    {NetplayMenuAction::NicknameEdit, RowToIndex(NetplayObRow::Nickname), "NICKNAME_EDIT"},
    {NetplayMenuAction::BackToMain, RowToIndex(NetplayObRow::ReturnToTitle), "BACK"},
}};

struct PatchRecord
{
    uintptr_t address;
    std::vector<uint8_t> originalBytes;
};

std::mutex g_patchMutex;
std::atomic<bool> g_hooksInstalled{false};
uintptr_t g_exeBase = 0;

std::vector<PatchRecord> g_appliedPatches;

uint32_t g_customDispatchTable[8] = {};
extern "C" uint32_t g_titleCaseReturnAddress = 0;
std::string g_moduleDirectory;

struct NetplayMenuState
{
    bool active = false;
    bool bgmActive = false;
    bool useConfigStyleRender = false;
    NetplayMenuId menuId = NetplayMenuId::Main;
    int mainSelection = 0;
    int optionCount = kNetplayDefaultOptionCount;
    int backIndex = kNetplayDefaultBackIndex;
    uint16_t hostPort = kDefaultNetplayPort;
    std::string joinAddress = "127.0.0.1";
    uint16_t joinPort = kDefaultNetplayPort;
    std::string nickname = "Player";
    uint8_t paletteStart = 193;
    uint8_t paletteCount = 48;
    NetplayRenderLayout renderLayout = {};
};

struct MenuSlideTransition
{
    bool active = false;
    NetplayMenuId fromMenu = NetplayMenuId::Main;
    NetplayMenuId toMenu = NetplayMenuId::Main;
    int fromSelection = 0;
    int toSelection = 0;
    int direction = 1; // +1 forward (submenu), -1 backward (back to main)
    int frame = 0;
};

struct InlineEditState
{
    bool active = false;
    NetplayMenuAction action = NetplayMenuAction::LeaveNetplay;
    std::string buffer;
    std::array<uint8_t, 256> keyDown = {};
    bool caretVisible = true;
    DWORD lastCaretTick = 0;
    std::string errorMessage;
    DWORD errorExpireTick = 0;
};

NetplayMenuState g_netplayMenuState;
MenuSlideTransition g_menuSlideTransition;
InlineEditState g_inlineEditState;
DWORD g_lastNetplayFrameLogTick = 0;
uint64_t g_titleUpdateCallCount = 0;
uint64_t g_netplayUpdateCallCount = 0;
int8_t g_lastLoggedSelection = -1;
bool g_hasLoggedInputSnapshot = false;
SpriteFont g_spriteFont = {};
bool g_useRuntimeTextOverlay = true;
bool g_enableGdiFallbackOverlay = false;
bool g_loggedSurfaceDcOk = false;
bool g_loggedSurfaceDcUnavailable = false;
bool g_loggedWindowDcFallback = false;
bool g_loggedSurfaceLockOk = false;
bool g_loggedSurfaceLockUnavailable = false;
bool g_loggedBackbufferSurfaceDesc = false;
bool g_loggedPrimarySurfaceDesc = false;
bool g_loggedOverlayPaletteChoice = false;
HFONT g_menuOverlayFont = nullptr;
HWND g_hookedWindow = nullptr;
WNDPROC g_originalWindowProc = nullptr;
bool g_netplayEscapeDown = false;

struct InputSnapshot
{
    int8_t p1Horizontal = 0;
    int8_t p1Vertical = 0;
    uint8_t p1Confirm = 0;
    uint8_t p1Cancel = 0;
    int8_t p2Horizontal = 0;
    int8_t p2Vertical = 0;
    uint8_t p2Confirm = 0;
    uint8_t p2Cancel = 0;
};

InputSnapshot g_lastInputSnapshot = {};

HMODULE ResolveCurrentModule();
uintptr_t RuntimeAddress(uintptr_t va);
void SwitchToMenu(uint32_t screenContext, NetplayMenuId menuId, int selection);
bool LoadNetplaySpriteFont();
bool IsInlineEditableAction(NetplayMenuAction action);
bool GetInlineEditDisplayValue(NetplayMenuAction action, std::string* outValue, bool includeCaret);
void ResetInlineEditState();
void CancelInlineEdit();
bool HandleInlineEditInput(uint32_t screenContext, const uint8_t* inputBytes);
void PlayUiSound(uint32_t screenContext, unsigned short soundIndex);
std::string TrimAscii(std::string value);
void InstallNetplayWindowHook(uint32_t screenContext);
void RemoveNetplayWindowHook();

const char* MenuIdToString(NetplayMenuId menuId)
{
    switch (menuId)
    {
    case NetplayMenuId::Main:
        return "Main";
    case NetplayMenuId::Host:
        return "Host";
    case NetplayMenuId::Join:
        return "Join";
    case NetplayMenuId::Nickname:
        return "Nickname";
    default:
        return "Unknown";
    }
}

const char* RowIndexToString(int rowIndex)
{
    switch (rowIndex)
    {
    case RowToIndex(NetplayObRow::Host):
        return "ROW_HOST";
    case RowToIndex(NetplayObRow::Join):
        return "ROW_JOIN";
    case RowToIndex(NetplayObRow::Nickname):
        return "ROW_NICKNAME";
    case RowToIndex(NetplayObRow::Address):
        return "ROW_ADDRESS";
    case RowToIndex(NetplayObRow::Port):
        return "ROW_PORT";
    case RowToIndex(NetplayObRow::Reserved5):
        return "ROW_RESERVED5";
    case RowToIndex(NetplayObRow::Reserved6):
        return "ROW_RESERVED6";
    case RowToIndex(NetplayObRow::ReturnToTitle):
        return "ROW_RETURN";
    default:
        return "ROW_UNKNOWN";
    }
}

const NetplayMenuSpec* GetMenuSpec(NetplayMenuId menuId)
{
    static const std::array<NetplayMenuSpec, 4> specs = {{
        {NetplayMenuId::Main, "NETPLAY SETTINGS", kMainMenuEntries.data(), static_cast<int>(kMainMenuEntries.size()), 0},
        {NetplayMenuId::Host, "HOST SETTINGS", kHostMenuEntries.data(), static_cast<int>(kHostMenuEntries.size()), 0},
        {NetplayMenuId::Join, "JOIN SETTINGS", kJoinMenuEntries.data(), static_cast<int>(kJoinMenuEntries.size()), 0},
        {NetplayMenuId::Nickname, "NICKNAME", kNicknameMenuEntries.data(), static_cast<int>(kNicknameMenuEntries.size()), 0},
    }};

    for (const NetplayMenuSpec& spec : specs)
    {
        if (spec.menuId == menuId)
        {
            return &spec;
        }
    }
    return nullptr;
}

const char* MenuActionToString(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::OpenHost:
        return "OpenHost";
    case NetplayMenuAction::OpenJoin:
        return "OpenJoin";
    case NetplayMenuAction::OpenNickname:
        return "OpenNickname";
    case NetplayMenuAction::LeaveNetplay:
        return "LeaveNetplay";
    case NetplayMenuAction::HostStart:
        return "HostStart";
    case NetplayMenuAction::HostEditPort:
        return "HostEditPort";
    case NetplayMenuAction::BackToMain:
        return "BackToMain";
    case NetplayMenuAction::JoinConnect:
        return "JoinConnect";
    case NetplayMenuAction::JoinEditAddress:
        return "JoinEditAddress";
    case NetplayMenuAction::JoinEditPort:
        return "JoinEditPort";
    case NetplayMenuAction::NicknameEdit:
        return "NicknameEdit";
    default:
        return "Unknown";
    }
}

const NetplayMenuEntry* GetMenuEntries(NetplayMenuId menuId, int* outCount)
{
    const NetplayMenuSpec* spec = GetMenuSpec(menuId);
    if (spec == nullptr)
    {
        if (outCount != nullptr)
        {
            *outCount = 0;
        }
        return nullptr;
    }

    if (outCount != nullptr)
    {
        *outCount = spec->entryCount;
    }
    return spec->entries;
}

int GetCurrentMenuEntryCount()
{
    int count = 0;
    (void)GetMenuEntries(g_netplayMenuState.menuId, &count);
    return count;
}

int GetDefaultSelectionForMenu(NetplayMenuId menuId)
{
    const NetplayMenuSpec* spec = GetMenuSpec(menuId);
    if (spec == nullptr)
    {
        return 0;
    }
    return spec->defaultSelection;
}

bool ValidateMenuSpecs()
{
    constexpr std::array<NetplayMenuId, 4> kMenus = {
        NetplayMenuId::Main,
        NetplayMenuId::Host,
        NetplayMenuId::Join,
        NetplayMenuId::Nickname,
    };

    for (NetplayMenuId menuId : kMenus)
    {
        const NetplayMenuSpec* spec = GetMenuSpec(menuId);
        if (spec == nullptr || spec->entries == nullptr || spec->entryCount <= 0)
        {
            mod::Log("ValidateMenuSpecs: invalid spec for menu=%s", MenuIdToString(menuId));
            return false;
        }

        if (spec->defaultSelection < 0 || spec->defaultSelection >= spec->entryCount)
        {
            mod::Log(
                "ValidateMenuSpecs: invalid default selection menu=%s default=%d count=%d",
                MenuIdToString(menuId),
                spec->defaultSelection,
                spec->entryCount);
            return false;
        }

        std::array<uint8_t, kNetplayConfigOptionCount> usedRows = {};
        for (int i = 0; i < spec->entryCount; ++i)
        {
            const NetplayMenuEntry& entry = spec->entries[i];
            if (entry.renderRow < 0 || entry.renderRow >= kNetplayConfigOptionCount)
            {
                mod::Log(
                    "ValidateMenuSpecs: row out of range menu=%s entry=%d row=%d label=%s",
                    MenuIdToString(menuId),
                    i,
                    entry.renderRow,
                    entry.debugLabel);
                return false;
            }

            if (usedRows[static_cast<size_t>(entry.renderRow)] != 0)
            {
                mod::Log(
                    "ValidateMenuSpecs: duplicate row menu=%s row=%d(%s) entry=%s",
                    MenuIdToString(menuId),
                    entry.renderRow,
                    RowIndexToString(entry.renderRow),
                    entry.debugLabel);
                return false;
            }
            usedRows[static_cast<size_t>(entry.renderRow)] = 1u;
        }
    }

    mod::Log("ValidateMenuSpecs: row-slot map valid");
    return true;
}

int ClampSelectionToCurrentMenu(int selection)
{
    const int count = GetCurrentMenuEntryCount();
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

const NetplayMenuEntry* GetCurrentMenuEntry(int selection)
{
    int count = 0;
    const NetplayMenuEntry* entries = GetMenuEntries(g_netplayMenuState.menuId, &count);
    if (entries == nullptr || count <= 0)
    {
        return nullptr;
    }
    const int clamped = ClampSelectionToCurrentMenu(selection);
    return &entries[clamped];
}

int GetRenderRowForSelection(int selection)
{
    if (!g_netplayMenuState.useConfigStyleRender)
    {
        return ClampSelectionToCurrentMenu(selection);
    }

    const NetplayMenuEntry* entry = GetCurrentMenuEntry(selection);
    if (entry == nullptr)
    {
        return 0;
    }
    return entry->renderRow;
}

bool IsRenderRowUsedByMenu(NetplayMenuId menuId, int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= kNetplayConfigOptionCount)
    {
        return false;
    }

    int count = 0;
    const NetplayMenuEntry* entries = GetMenuEntries(menuId, &count);
    if (entries == nullptr || count <= 0)
    {
        return false;
    }

    for (int i = 0; i < count; ++i)
    {
        if (entries[i].renderRow == rowIndex)
        {
            return true;
        }
    }
    return false;
}

int GetScaledNativeSlideY(uint32_t screenContext)
{
    int slideY = *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY);
    slideY /= kNetplayNativeSlideDivisor;
    if (slideY < -400)
    {
        slideY = -400;
    }
    if (slideY > 400)
    {
        slideY = 400;
    }
    return slideY;
}

void ResetMenuSlideTransition()
{
    g_menuSlideTransition = {};
}

bool IsMenuSlideTransitionActive()
{
    return g_menuSlideTransition.active;
}

void StartMenuSlideTransition(uint32_t screenContext, NetplayMenuId targetMenu, int targetSelection, int direction)
{
    if (g_menuSlideTransition.active)
    {
        return;
    }
    if (g_inlineEditState.active)
    {
        CancelInlineEdit();
    }

    const int currentSelection = ClampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
    const int resolvedTargetSelection = (targetSelection < 0) ? GetDefaultSelectionForMenu(targetMenu) : targetSelection;
    auto const performSlide = reinterpret_cast<int(__thiscall*)(void*, unsigned char)>(RuntimeAddress(kVaPerformSlideAnimation));

    g_menuSlideTransition.active = true;
    g_menuSlideTransition.fromMenu = g_netplayMenuState.menuId;
    g_menuSlideTransition.toMenu = targetMenu;
    g_menuSlideTransition.fromSelection = currentSelection;
    g_menuSlideTransition.toSelection = resolvedTargetSelection;
    g_menuSlideTransition.direction = (direction >= 0) ? 1 : -1;
    g_menuSlideTransition.frame = 0;

    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
    *reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter) = 0;

    mod::Log(
        "NetplaySlide(native): start from=%s(%d) to=%s(%d) dir=%d",
        MenuIdToString(g_menuSlideTransition.fromMenu),
        g_menuSlideTransition.fromSelection,
        MenuIdToString(g_menuSlideTransition.toMenu),
        g_menuSlideTransition.toSelection,
        g_menuSlideTransition.direction);

    if (performSlide != nullptr)
    {
        *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY) = 0;
        (void)performSlide(reinterpret_cast<void*>(screenContext), 0);
    }
    SwitchToMenu(screenContext, targetMenu, resolvedTargetSelection);
    if (performSlide != nullptr)
    {
        (void)performSlide(reinterpret_cast<void*>(screenContext), 1u);
    }
    *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY) = 0;
    g_menuSlideTransition.active = false;
    mod::Log("NetplaySlide(native): complete menu=%s selection=%d", MenuIdToString(targetMenu), resolvedTargetSelection);
}

void AdvanceMenuSlideTransition(uint32_t screenContext)
{
    (void)screenContext;
}

struct TextInputDialogContext
{
    std::string title;
    std::string prompt;
    std::string text;
    bool numericOnly = false;
    bool accepted = false;
    bool done = false;
    HWND editHandle = nullptr;
    HFONT uiFont = nullptr;
};

constexpr const char* kInputDialogClassName = "EFZNetplayInputDialog";

LRESULT CALLBACK InputDialogWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* const context = reinterpret_cast<TextInputDialogContext*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

    switch (message)
    {
    case WM_NCCREATE:
    {
        auto* const create = reinterpret_cast<CREATESTRUCTA*>(lParam);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE:
    {
        if (context == nullptr)
        {
            return -1;
        }

        context->uiFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HWND promptLabel = CreateWindowExA(
            0,
            "STATIC",
            context->prompt.c_str(),
            WS_CHILD | WS_VISIBLE,
            12,
            12,
            332,
            20,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDialogControlPrompt)),
            nullptr,
            nullptr);

        const DWORD editStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (context->numericOnly ? ES_NUMBER : 0);
        context->editHandle = CreateWindowExA(
            WS_EX_CLIENTEDGE,
            "EDIT",
            context->text.c_str(),
            editStyle,
            12,
            36,
            332,
            24,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDialogControlInput)),
            nullptr,
            nullptr);

        HWND okButton = CreateWindowExA(0, "BUTTON", "OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 180, 74, 80, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), nullptr, nullptr);
        HWND cancelButton = CreateWindowExA(0, "BUTTON", "Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 264, 74, 80, 26, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), nullptr, nullptr);

        if (context->uiFont != nullptr)
        {
            SendMessageA(promptLabel, WM_SETFONT, reinterpret_cast<WPARAM>(context->uiFont), TRUE);
            SendMessageA(context->editHandle, WM_SETFONT, reinterpret_cast<WPARAM>(context->uiFont), TRUE);
            SendMessageA(okButton, WM_SETFONT, reinterpret_cast<WPARAM>(context->uiFont), TRUE);
            SendMessageA(cancelButton, WM_SETFONT, reinterpret_cast<WPARAM>(context->uiFont), TRUE);
        }

        SendMessageA(context->editHandle, EM_SETLIMITTEXT, static_cast<WPARAM>(kDialogInputLimit), 0);
        SetFocus(context->editHandle);
        SendMessageA(context->editHandle, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND:
    {
        if (context == nullptr)
        {
            return 0;
        }

        const UINT commandId = LOWORD(wParam);
        if (commandId == IDOK)
        {
            char buffer[kDialogInputLimit + 1] = {};
            if (context->editHandle != nullptr)
            {
                GetWindowTextA(context->editHandle, buffer, static_cast<int>(sizeof(buffer)));
            }
            context->text = buffer;
            context->accepted = true;
            context->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (commandId == IDCANCEL)
        {
            context->accepted = false;
            context->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (context != nullptr)
        {
            context->accepted = false;
            context->done = true;
        }
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

bool EnsureInputDialogClass()
{
    HMODULE module = ResolveCurrentModule();
    if (module == nullptr)
    {
        module = GetModuleHandleA(nullptr);
    }

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    if (GetClassInfoExA(module, kInputDialogClassName, &wc) != FALSE
        || GetClassInfoExA(nullptr, kInputDialogClassName, &wc) != FALSE)
    {
        return true;
    }

    WNDCLASSEXA cls = {};
    cls.cbSize = sizeof(cls);
    cls.style = CS_HREDRAW | CS_VREDRAW;
    cls.lpfnWndProc = InputDialogWndProc;
    cls.hInstance = module;
    cls.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    cls.lpszClassName = kInputDialogClassName;

    if (RegisterClassExA(&cls) != 0)
    {
        return true;
    }

    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void CenterWindowOnOwner(HWND hwnd, HWND owner)
{
    RECT dialogRect = {};
    GetWindowRect(hwnd, &dialogRect);

    int targetX = 100;
    int targetY = 100;
    const int dialogWidth = dialogRect.right - dialogRect.left;
    const int dialogHeight = dialogRect.bottom - dialogRect.top;

    if (owner != nullptr && IsWindow(owner))
    {
        RECT ownerRect = {};
        GetWindowRect(owner, &ownerRect);
        targetX = ownerRect.left + ((ownerRect.right - ownerRect.left) - dialogWidth) / 2;
        targetY = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - dialogHeight) / 2;
    }
    else
    {
        targetX = (GetSystemMetrics(SM_CXSCREEN) - dialogWidth) / 2;
        targetY = (GetSystemMetrics(SM_CYSCREEN) - dialogHeight) / 2;
    }

    SetWindowPos(hwnd, nullptr, targetX, targetY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool ShowTextInputDialog(HWND owner, const char* title, const char* prompt, std::string* inOutValue, bool numericOnly)
{
    if (inOutValue == nullptr)
    {
        return false;
    }

    if (!EnsureInputDialogClass())
    {
        mod::Log("ShowTextInputDialog: failed to register dialog class");
        return false;
    }

    TextInputDialogContext context = {};
    context.title = title;
    context.prompt = prompt;
    context.text = *inOutValue;
    context.numericOnly = numericOnly;

    HMODULE module = ResolveCurrentModule();
    if (module == nullptr)
    {
        module = GetModuleHandleA(nullptr);
    }

    HWND dialog = CreateWindowExA(
        WS_EX_DLGMODALFRAME,
        kInputDialogClassName,
        context.title.c_str(),
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        360,
        140,
        owner,
        nullptr,
        module,
        &context);
    if (dialog == nullptr)
    {
        mod::Log("ShowTextInputDialog: CreateWindowExA failed (err=%lu)", GetLastError());
        return false;
    }

    const bool disableOwner = owner != nullptr && IsWindow(owner) && IsWindowEnabled(owner);
    if (disableOwner)
    {
        EnableWindow(owner, FALSE);
    }

    CenterWindowOnOwner(dialog, owner);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);

    MSG message = {};
    while (!context.done)
    {
        const BOOL messageResult = GetMessageA(&message, nullptr, 0, 0);
        if (messageResult <= 0)
        {
            context.accepted = false;
            context.done = true;
            break;
        }

        if (!IsDialogMessageA(dialog, &message))
        {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }
    }

    if (disableOwner)
    {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    if (context.accepted)
    {
        *inOutValue = context.text;
        return true;
    }
    return false;
}

bool ParsePort(const std::string& text, uint16_t* outPort)
{
    if (outPort == nullptr || text.empty() || text.size() > 5)
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

    const int parsed = std::atoi(text.c_str());
    if (parsed <= 0 || parsed > 65535)
    {
        return false;
    }

    *outPort = static_cast<uint16_t>(parsed);
    return true;
}

bool IsValidJoinAddress(const std::string& address)
{
    if (address.empty() || address.size() > 63)
    {
        return false;
    }

    for (char c : address)
    {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == ':' || c == '-' || c == '_';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

bool IsValidNickname(const std::string& nickname)
{
    if (nickname.empty() || nickname.size() > 20)
    {
        return false;
    }

    for (char c : nickname)
    {
        if (c < 32 || c > 126)
        {
            return false;
        }
    }
    return true;
}

LRESULT CALLBACK NetplayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_netplayMenuState.active)
    {
        const bool isEsc = (wParam == static_cast<WPARAM>(VK_ESCAPE));
        const bool isEscMessage =
            message == WM_KEYDOWN
            || message == WM_KEYUP
            || message == WM_SYSKEYDOWN
            || message == WM_SYSKEYUP
            || message == WM_CHAR
            || message == WM_SYSCHAR;

        // Only swallow ESC while inline edit is active; outside edit mode
        // ESC should continue to drive menu back navigation.
        if (g_inlineEditState.active && isEsc && isEscMessage)
        {
            return 0;
        }
    }

    if (g_originalWindowProc != nullptr)
    {
        return CallWindowProcA(g_originalWindowProc, hwnd, message, wParam, lParam);
    }
    return DefWindowProcA(hwnd, message, wParam, lParam);
}

void InstallNetplayWindowHook(uint32_t screenContext)
{
    const HWND hwnd = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
    if (hwnd == nullptr || !IsWindow(hwnd))
    {
        return;
    }

    if (g_hookedWindow == hwnd && g_originalWindowProc != nullptr)
    {
        return;
    }

    RemoveNetplayWindowHook();
    LONG_PTR previousProc = SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&NetplayWindowProc));
    if (previousProc == 0)
    {
        const DWORD err = GetLastError();
        if (err != 0)
        {
            mod::Log("InstallNetplayWindowHook: SetWindowLongPtrA failed (err=%lu)", err);
            return;
        }
    }

    g_originalWindowProc = reinterpret_cast<WNDPROC>(previousProc);
    g_hookedWindow = hwnd;
    mod::Log("InstallNetplayWindowHook: hwnd=0x%p originalProc=0x%p", hwnd, reinterpret_cast<void*>(previousProc));
}

void RemoveNetplayWindowHook()
{
    if (g_hookedWindow == nullptr || g_originalWindowProc == nullptr)
    {
        g_hookedWindow = nullptr;
        g_originalWindowProc = nullptr;
        return;
    }

    if (IsWindow(g_hookedWindow))
    {
        LONG_PTR result = SetWindowLongPtrA(g_hookedWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_originalWindowProc));
        if (result == 0)
        {
            const DWORD err = GetLastError();
            if (err != 0)
            {
                mod::Log("RemoveNetplayWindowHook: restore failed (err=%lu)", err);
            }
        }
    }

    mod::Log("RemoveNetplayWindowHook: restored");
    g_hookedWindow = nullptr;
    g_originalWindowProc = nullptr;
}

bool IsInlineEditableAction(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::HostEditPort:
    case NetplayMenuAction::JoinEditAddress:
    case NetplayMenuAction::JoinEditPort:
    case NetplayMenuAction::NicknameEdit:
        return true;
    default:
        return false;
    }
}

size_t GetInlineEditMaxLength(NetplayMenuAction action)
{
    switch (action)
    {
    case NetplayMenuAction::HostEditPort:
    case NetplayMenuAction::JoinEditPort:
        return kInlineEditMaxPortLength;
    case NetplayMenuAction::JoinEditAddress:
        return kInlineEditMaxJoinAddressLength;
    case NetplayMenuAction::NicknameEdit:
        return kInlineEditMaxNicknameLength;
    default:
        return 0;
    }
}

bool IsInlineEditCharacterAllowed(NetplayMenuAction action, char c)
{
    const unsigned char uc = static_cast<unsigned char>(c);
    switch (action)
    {
    case NetplayMenuAction::HostEditPort:
    case NetplayMenuAction::JoinEditPort:
        return c >= '0' && c <= '9';
    case NetplayMenuAction::JoinEditAddress:
        return std::isalnum(uc) != 0 || c == '.' || c == ':' || c == '-' || c == '_';
    case NetplayMenuAction::NicknameEdit:
        return c >= 32 && c <= 126;
    default:
        return false;
    }
}

bool GetCommittedActionValue(NetplayMenuAction action, std::string* outValue)
{
    if (outValue == nullptr)
    {
        return false;
    }

    switch (action)
    {
    case NetplayMenuAction::HostEditPort:
        *outValue = std::to_string(static_cast<unsigned>(g_netplayMenuState.hostPort));
        return true;
    case NetplayMenuAction::JoinEditAddress:
        *outValue = g_netplayMenuState.joinAddress;
        return true;
    case NetplayMenuAction::JoinEditPort:
        *outValue = std::to_string(static_cast<unsigned>(g_netplayMenuState.joinPort));
        return true;
    case NetplayMenuAction::NicknameEdit:
        *outValue = g_netplayMenuState.nickname;
        return true;
    default:
        return false;
    }
}

bool GetInlineEditDisplayValue(NetplayMenuAction action, std::string* outValue, bool includeCaret)
{
    if (outValue == nullptr || !GetCommittedActionValue(action, outValue))
    {
        return false;
    }

    if (g_inlineEditState.active && g_inlineEditState.action == action)
    {
        *outValue = g_inlineEditState.buffer;
        if (includeCaret && g_inlineEditState.caretVisible)
        {
            outValue->push_back('_');
        }
    }
    return true;
}

void PrimeInlineEditKeyState()
{
    g_inlineEditState.keyDown.fill(0);
    for (int vk = 0; vk < 256; ++vk)
    {
        g_inlineEditState.keyDown[static_cast<size_t>(vk)] =
            (GetAsyncKeyState(vk) & 0x8000) != 0 ? 1u : 0u;
    }
}

void ClearInlineEditError()
{
    g_inlineEditState.errorMessage.clear();
    g_inlineEditState.errorExpireTick = 0;
}

void SetInlineEditError(const char* error)
{
    g_inlineEditState.errorMessage = (error != nullptr) ? error : "";
    g_inlineEditState.errorExpireTick = GetTickCount() + kInlineEditErrorDisplayMs;
}

void UpdateInlineEditCaretBlink()
{
    if (!g_inlineEditState.active)
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (g_inlineEditState.lastCaretTick == 0)
    {
        g_inlineEditState.lastCaretTick = now;
        g_inlineEditState.caretVisible = true;
        return;
    }

    if (now - g_inlineEditState.lastCaretTick >= kInlineEditCaretBlinkMs)
    {
        g_inlineEditState.lastCaretTick = now;
        g_inlineEditState.caretVisible = !g_inlineEditState.caretVisible;
    }

    if (!g_inlineEditState.errorMessage.empty() && now >= g_inlineEditState.errorExpireTick)
    {
        ClearInlineEditError();
    }
}

bool ConsumeInlineEditKeyEdge(int virtualKey)
{
    if (virtualKey < 0 || virtualKey >= 256)
    {
        return false;
    }

    const bool down = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    const size_t keyIndex = static_cast<size_t>(virtualKey);
    const bool pressed = down && g_inlineEditState.keyDown[keyIndex] == 0;
    g_inlineEditState.keyDown[keyIndex] = down ? 1u : 0u;
    return pressed;
}

bool IsCtrlPressed()
{
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
}

bool TryTranslateVirtualKeyToAscii(int virtualKey, char* outChar)
{
    if (outChar == nullptr)
    {
        return false;
    }

    if (virtualKey >= VK_NUMPAD0 && virtualKey <= VK_NUMPAD9)
    {
        *outChar = static_cast<char>('0' + (virtualKey - VK_NUMPAD0));
        return true;
    }
    if (virtualKey == VK_DECIMAL)
    {
        *outChar = '.';
        return true;
    }

    BYTE keyboardState[256] = {};
    if (GetKeyboardState(keyboardState) == FALSE)
    {
        return false;
    }

    WORD translated = 0;
    const UINT scanCode = MapVirtualKeyA(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
    const int result = ToAscii(
        static_cast<UINT>(virtualKey),
        scanCode,
        keyboardState,
        &translated,
        0);
    if (result != 1)
    {
        return false;
    }

    const char c = static_cast<char>(translated & 0xFF);
    if (c < 32 || c > 126)
    {
        return false;
    }
    *outChar = c;
    return true;
}

bool TryReadClipboardAsciiText(HWND owner, std::string* outText)
{
    if (outText == nullptr || OpenClipboard(owner) == FALSE)
    {
        return false;
    }

    std::string clipboardText;

    HANDLE textHandle = GetClipboardData(CF_TEXT);
    if (textHandle != nullptr)
    {
        const char* text = reinterpret_cast<const char*>(GlobalLock(textHandle));
        if (text != nullptr)
        {
            clipboardText = text;
            GlobalUnlock(textHandle);
        }
    }

    if (clipboardText.empty())
    {
        HANDLE unicodeHandle = GetClipboardData(CF_UNICODETEXT);
        if (unicodeHandle != nullptr)
        {
            const wchar_t* text = reinterpret_cast<const wchar_t*>(GlobalLock(unicodeHandle));
            if (text != nullptr)
            {
                const int needed = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
                if (needed > 1)
                {
                    std::vector<char> converted(static_cast<size_t>(needed));
                    if (WideCharToMultiByte(CP_ACP, 0, text, -1, converted.data(), needed, nullptr, nullptr) > 0)
                    {
                        clipboardText.assign(converted.data());
                    }
                }
                GlobalUnlock(unicodeHandle);
            }
        }
    }

    CloseClipboard();
    *outText = clipboardText;
    return !clipboardText.empty();
}

void BeginInlineEdit(NetplayMenuAction action)
{
    if (!IsInlineEditableAction(action))
    {
        return;
    }

    std::string initialValue;
    (void)GetCommittedActionValue(action, &initialValue);

    g_inlineEditState.active = true;
    g_inlineEditState.action = action;
    g_inlineEditState.buffer = initialValue;
    g_inlineEditState.caretVisible = true;
    g_inlineEditState.lastCaretTick = GetTickCount();
    ClearInlineEditError();
    PrimeInlineEditKeyState();

    mod::Log("InlineEdit: begin action=%s value='%s'", MenuActionToString(action), initialValue.c_str());
}

void ResetInlineEditState()
{
    g_inlineEditState = {};
}

void CancelInlineEdit()
{
    if (!g_inlineEditState.active)
    {
        return;
    }

    mod::Log("InlineEdit: cancel action=%s", MenuActionToString(g_inlineEditState.action));
    ResetInlineEditState();
}

bool CommitInlineEdit()
{
    if (!g_inlineEditState.active)
    {
        return false;
    }

    std::string value = TrimAscii(g_inlineEditState.buffer);

    switch (g_inlineEditState.action)
    {
    case NetplayMenuAction::HostEditPort:
    case NetplayMenuAction::JoinEditPort:
    {
        uint16_t parsedPort = 0;
        if (!ParsePort(value, &parsedPort))
        {
            SetInlineEditError("INVALID PORT");
            mod::Log("InlineEdit: invalid port '%s'", value.c_str());
            return false;
        }

        if (g_inlineEditState.action == NetplayMenuAction::HostEditPort)
        {
            g_netplayMenuState.hostPort = parsedPort;
        }
        else
        {
            g_netplayMenuState.joinPort = parsedPort;
        }
        break;
    }
    case NetplayMenuAction::JoinEditAddress:
        if (!IsValidJoinAddress(value))
        {
            SetInlineEditError("INVALID ADDRESS");
            mod::Log("InlineEdit: invalid join address '%s'", value.c_str());
            return false;
        }
        g_netplayMenuState.joinAddress = value;
        break;
    case NetplayMenuAction::NicknameEdit:
        if (!IsValidNickname(value))
        {
            SetInlineEditError("INVALID NAME");
            mod::Log("InlineEdit: invalid nickname '%s'", value.c_str());
            return false;
        }
        g_netplayMenuState.nickname = value;
        break;
    default:
        return false;
    }

    mod::Log("InlineEdit: commit action=%s value='%s'", MenuActionToString(g_inlineEditState.action), value.c_str());
    ResetInlineEditState();
    return true;
}

void TryAppendInlineEditCharacter(char c)
{
    if (!g_inlineEditState.active)
    {
        return;
    }

    if (!IsInlineEditCharacterAllowed(g_inlineEditState.action, c))
    {
        return;
    }

    const size_t maxLength = GetInlineEditMaxLength(g_inlineEditState.action);
    if (maxLength == 0 || g_inlineEditState.buffer.size() >= maxLength)
    {
        return;
    }

    g_inlineEditState.buffer.push_back(c);
    g_inlineEditState.caretVisible = true;
    g_inlineEditState.lastCaretTick = GetTickCount();
    ClearInlineEditError();
}

void TryPasteInlineEditText(HWND owner)
{
    if (!g_inlineEditState.active)
    {
        return;
    }

    std::string clipboardText;
    if (!TryReadClipboardAsciiText(owner, &clipboardText))
    {
        return;
    }

    for (char c : clipboardText)
    {
        TryAppendInlineEditCharacter(c);
    }
}

bool HandleInlineEditInput(uint32_t screenContext, const uint8_t* inputBytes)
{
    if (!g_inlineEditState.active || inputBytes == nullptr)
    {
        return false;
    }

    // Keep global ESC edge tracking in sync while inline edit owns ESC.
    g_netplayEscapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;

    UpdateInlineEditCaretBlink();

    const HWND owner = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
    // Inline edit is keyboard-driven on purpose: avoid conflicts with the
    // player's in-game menu bindings (confirm/cancel/attack buttons).
    const bool confirmPressed = ConsumeInlineEditKeyEdge(VK_RETURN);
    const bool cancelPressed = ConsumeInlineEditKeyEdge(VK_ESCAPE);

    if (cancelPressed)
    {
        PlayUiSound(screenContext, kSfxConfirm);
        CancelInlineEdit();
        return true;
    }

    if (confirmPressed)
    {
        const bool committed = CommitInlineEdit();
        PlayUiSound(screenContext, committed ? kSfxConfirm : kSfxMove);
        return true;
    }

    bool changed = false;
    if (ConsumeInlineEditKeyEdge(VK_BACK) || ConsumeInlineEditKeyEdge(VK_DELETE))
    {
        if (!g_inlineEditState.buffer.empty())
        {
            g_inlineEditState.buffer.pop_back();
            g_inlineEditState.caretVisible = true;
            g_inlineEditState.lastCaretTick = GetTickCount();
            ClearInlineEditError();
            changed = true;
        }
    }

    if (ConsumeInlineEditKeyEdge('V'))
    {
        if (IsCtrlPressed())
        {
            const size_t before = g_inlineEditState.buffer.size();
            TryPasteInlineEditText(owner);
            changed = changed || g_inlineEditState.buffer.size() != before;
        }
        else
        {
            char c = 0;
            if (TryTranslateVirtualKeyToAscii('V', &c))
            {
                const size_t before = g_inlineEditState.buffer.size();
                TryAppendInlineEditCharacter(c);
                changed = changed || g_inlineEditState.buffer.size() != before;
            }
        }
    }

    auto handlePrintableKey = [&](int virtualKey)
    {
        if (!ConsumeInlineEditKeyEdge(virtualKey))
        {
            return;
        }
        char c = 0;
        if (!TryTranslateVirtualKeyToAscii(virtualKey, &c))
        {
            return;
        }
        const size_t before = g_inlineEditState.buffer.size();
        TryAppendInlineEditCharacter(c);
        changed = changed || g_inlineEditState.buffer.size() != before;
    };

    for (int vk = 'A'; vk <= 'Z'; ++vk)
    {
        if (vk == 'V')
        {
            continue;
        }
        handlePrintableKey(vk);
    }
    for (int vk = '0'; vk <= '9'; ++vk)
    {
        handlePrintableKey(vk);
    }
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk)
    {
        handlePrintableKey(vk);
    }

    constexpr std::array<int, 11> kExtraKeys = {
        VK_SPACE,
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
        handlePrintableKey(vk);
    }
    handlePrintableKey(VK_DECIMAL);

    if (changed)
    {
        mod::Log("InlineEdit: action=%s buffer='%s'", MenuActionToString(g_inlineEditState.action), g_inlineEditState.buffer.c_str());
    }
    return true;
}

bool ConsumeNetplayEscapeEdge()
{
    const bool down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool pressed = down && !g_netplayEscapeDown;
    g_netplayEscapeDown = down;
    return pressed;
}

uintptr_t RuntimeAddress(uintptr_t va)
{
    return g_exeBase + (va - kEfzImageBase);
}

bool WriteProcessMemoryLocal(uintptr_t address, const uint8_t* data, size_t size)
{
    DWORD oldProtect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect) == FALSE)
    {
        mod::Log("VirtualProtect failed at 0x%08X", static_cast<unsigned>(address));
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(address), data, size);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);

    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &tmp);
    return true;
}

bool ApplyPatch(
    uintptr_t address,
    const std::vector<uint8_t>& expectedBytes,
    const std::vector<uint8_t>& patchedBytes,
    const char* label)
{
    if (patchedBytes.empty())
    {
        return true;
    }

    std::vector<uint8_t> currentBytes(patchedBytes.size());
    std::memcpy(currentBytes.data(), reinterpret_cast<void*>(address), currentBytes.size());

    if (!expectedBytes.empty() && expectedBytes != currentBytes)
    {
        mod::Log("Patch '%s' mismatch at 0x%08X", label, static_cast<unsigned>(address));
        return false;
    }

    if (!WriteProcessMemoryLocal(address, patchedBytes.data(), patchedBytes.size()))
    {
        mod::Log("Patch '%s' write failed at 0x%08X", label, static_cast<unsigned>(address));
        return false;
    }

    g_appliedPatches.push_back(PatchRecord{address, std::move(currentBytes)});
    mod::Log("Patched '%s' at 0x%08X", label, static_cast<unsigned>(address));
    return true;
}

void RestorePatches()
{
    for (auto it = g_appliedPatches.rbegin(); it != g_appliedPatches.rend(); ++it)
    {
        (void)WriteProcessMemoryLocal(it->address, it->originalBytes.data(), it->originalBytes.size());
    }
    g_appliedPatches.clear();
}

using PlaySoundEffectFn = int(__thiscall*)(void* gameSystem, unsigned short soundIndex);
using PlayBackgroundMusicFn = void(__thiscall*)(int gameSystem, unsigned short trackNumber);
using StopBackgroundMusicFn = int(__thiscall*)(int gameSystem);
using ProcessPlayerInputFn = void(__thiscall*)(int* inputManager);
using LoadCompressedImageFileFn = void(__thiscall*)(void*** graphicsManager, uint32_t* destSurface, const char* fileName, unsigned char colorOffset1, unsigned char colorOffset2);
using LoadBgrColorsFromRawFileFn = int(__stdcall*)(int destBuffer, const char* rgbFileName, int srcStartIndex, int destStartIndex, int colorCount);
using ReadPixelValueFn = char(__stdcall*)(int surfacePtr);
using SetPaletteFn = int(__thiscall*)(void** graphicsContext, int paletteData);
using FadeWithSoundAdjustmentFn =
    int(__thiscall*)(void* screenContext, int paletteId, unsigned char fadeDirection, int baseVolume, int volumeAdjustment);
using FadeScreenEffectFn = int(__thiscall*)(void* screenContext, int paletteId, unsigned char fadeDirection);
using PerformSlideAnimationFn = int(__thiscall*)(void* screenContext, unsigned char slideDirection);
using BlitSurfaceWithTransparencyFn = BOOL(__thiscall*)(
    void** graphicsContext,
    int destX,
    int destY,
    int destRight,
    int destBottom,
    int sourceSurface,
    int sourceX,
    int sourceY,
    int sourceRight,
    int sourceBottom,
    char transparentColor,
    int flipHorizontal);
using PresentFrameToScreenFn = BOOL(__thiscall*)(int graphicsContext);

using TitleUpdateFn = char(__thiscall*)(uint32_t screenContext);
using TitleRenderFn = BOOL(__thiscall*)(uint32_t screenContext);
using TitleAuxFn = int(__thiscall*)(uint32_t screenContext);

bool FileExists(const std::string& path)
{
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string JoinPath(const std::string& left, const char* right)
{
    if (left.empty())
    {
        return right;
    }
    if (left.back() == '\\' || left.back() == '/')
    {
        return left + right;
    }
    return left + "\\" + right;
}

HMODULE ResolveCurrentModule()
{
    HMODULE module = nullptr;
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&ResolveCurrentModule),
            &module)
        == FALSE)
    {
        return nullptr;
    }
    return module;
}

std::string BuildModuleDirectory(HMODULE moduleHandle)
{
    if (moduleHandle == nullptr)
    {
        return ".";
    }

    char modulePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return ".";
    }

    std::string path(modulePath);
    const std::size_t slashPos = path.find_last_of("\\/");
    if (slashPos == std::string::npos)
    {
        return ".";
    }

    path.resize(slashPos);
    return path;
}

std::string ResolveNetplayBackgroundPath()
{
    mod::Log("ResolveNetplayBackgroundPath: searching DLL-relative assets");
    const std::array<const char*, 2> candidates = {
        "assets\\netplay_bg.dat",
        "netplay_bg.dat",
    };

    for (const char* candidate : candidates)
    {
        const std::string path = JoinPath(g_moduleDirectory, candidate);
        mod::Log("ResolveNetplayBackgroundPath: probing '%s'", path.c_str());
        if (FileExists(path))
        {
            mod::Log("ResolveNetplayBackgroundPath: using '%s'", path.c_str());
            return path;
        }
    }

    if (FileExists("netplay_bg.dat"))
    {
        mod::Log("ResolveNetplayBackgroundPath: fallback working-directory file 'netplay_bg.dat'");
        return "netplay_bg.dat";
    }

    mod::Log("ResolveNetplayBackgroundPath: no candidate found");
    return {};
}

std::string ResolveNetplayObjectsPath()
{
    mod::Log("ResolveNetplayObjectsPath: searching DLL-relative assets");
    const std::array<const char*, 6> candidates = {
        "assets\\netplay_ob.dat",
        "assets\\netplay_ui_ob.dat",
        "assets\\config_ob.dat",
        "netplay_ob.dat",
        "netplay_ui_ob.dat",
        "config_ob.dat",
    };

    for (const char* candidate : candidates)
    {
        const std::string path = JoinPath(g_moduleDirectory, candidate);
        mod::Log("ResolveNetplayObjectsPath: probing '%s'", path.c_str());
        if (FileExists(path))
        {
            mod::Log("ResolveNetplayObjectsPath: using '%s'", path.c_str());
            return path;
        }
    }

    mod::Log("ResolveNetplayObjectsPath: fallback to vanilla title objects (no DLL-local netplay object found)");
    return "system\\title_ob.dat";
}

bool ContainsNoCase(std::string_view text, std::string_view needle)
{
    if (needle.empty() || needle.size() > text.size())
    {
        return false;
    }

    for (std::size_t i = 0; i + needle.size() <= text.size(); ++i)
    {
        bool matched = true;
        for (std::size_t j = 0; j < needle.size(); ++j)
        {
            char a = text[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z')
            {
                a = static_cast<char>(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z')
            {
                b = static_cast<char>(b - 'A' + 'a');
            }
            if (a != b)
            {
                matched = false;
                break;
            }
        }
        if (matched)
        {
            return true;
        }
    }

    return false;
}

struct NetplayObjectProfile
{
    unsigned char colorOffset = 193;
    int paletteDestStart = 193;
    int paletteCount = 48;
    bool useConfigStyleRender = false;
    bool deriveLayoutFromDat = false;
    NetplayRenderLayout renderLayout = {};
};

struct ParsedDatImage
{
    int width = 0;
    int height = 0;
    uint8_t transparentIndex = 0;
    bool hasTransparentIndex = false;
    std::vector<uint8_t> pixelsTopDown;
};

bool ParseEfzDatImage(const std::string& path, ParsedDatImage* outImage)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        mod::Log("ParseEfzDatImage: failed to open '%s'", path.c_str());
        return false;
    }

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 1 + 3 + 4)
    {
        mod::Log("ParseEfzDatImage: file too small '%s' (%zu bytes)", path.c_str(), bytes.size());
        return false;
    }

    auto tryHeader = [&bytes](size_t headerOffset, int* outW, int* outH, size_t* outPixelOffset, size_t* outPixelCount)
    {
        if (headerOffset + 4u > bytes.size())
        {
            return false;
        }

        const int width = static_cast<int>(bytes[headerOffset] | (bytes[headerOffset + 1] << 8));
        const int height = static_cast<int>(bytes[headerOffset + 2] | (bytes[headerOffset + 3] << 8));
        if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        {
            return false;
        }

        const size_t pixelOffset = headerOffset + 4u;
        const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (pixelCount == 0 || pixelOffset + pixelCount > bytes.size())
        {
            return false;
        }

        *outW = width;
        *outH = height;
        *outPixelOffset = pixelOffset;
        *outPixelCount = pixelCount;
        return true;
    };

    std::vector<size_t> candidateOffsets;
    candidateOffsets.reserve(20);
    candidateOffsets.push_back(1u + (static_cast<size_t>(bytes[0]) + 1u) * 3u); // 256-color interpretation
    candidateOffsets.push_back(1u + static_cast<size_t>(bytes[0]) * 3u);        // legacy interpretation
    candidateOffsets.push_back(769u);                                            // common EFZ DAT layout
    candidateOffsets.push_back(766u);                                            // converter variant
    for (size_t o = 760; o <= 772; ++o)
    {
        candidateOffsets.push_back(o);
    }

    // De-duplicate while preserving order.
    std::vector<size_t> offsets;
    offsets.reserve(candidateOffsets.size());
    for (size_t off : candidateOffsets)
    {
        bool seen = false;
        for (size_t existing : offsets)
        {
            if (existing == off)
            {
                seen = true;
                break;
            }
        }
        if (!seen)
        {
            offsets.push_back(off);
        }
    }

    bool found = false;
    size_t headerOffset = 0;
    size_t pixelOffset = 0;
    size_t pixelCount = 0;
    int width = 0;
    int height = 0;
    int bestScore = -1;
    for (size_t off : offsets)
    {
        int w = 0;
        int h = 0;
        size_t po = 0;
        size_t pc = 0;
        if (!tryHeader(off, &w, &h, &po, &pc))
        {
            continue;
        }

        int score = 0;
        if (po + pc == bytes.size())
        {
            score += 10000; // exact payload fit is strongest signal
        }
        if (w == 320)
        {
            score += 100;
        }
        if (h == 240 || h == 480)
        {
            score += 100;
        }
        if (off == 769 || off == 766)
        {
            score += 50;
        }

        if (!found || score > bestScore)
        {
            found = true;
            bestScore = score;
            headerOffset = off;
            pixelOffset = po;
            pixelCount = pc;
            width = w;
            height = h;
        }
    }

    if (!found)
    {
        mod::Log(
            "ParseEfzDatImage: could not resolve header layout for '%s' (size=%zu firstByte=%u)",
            path.c_str(),
            bytes.size(),
            static_cast<unsigned>(bytes[0]));
        return false;
    }

    outImage->width = width;
    outImage->height = height;
    outImage->pixelsTopDown.resize(pixelCount);

    const uint8_t* srcPixels = bytes.data() + pixelOffset;
    for (int y = 0; y < height; ++y)
    {
        const int srcY = (height - 1) - y;
        std::memcpy(
            outImage->pixelsTopDown.data() + static_cast<size_t>(y) * static_cast<size_t>(width),
            srcPixels + static_cast<size_t>(srcY) * static_cast<size_t>(width),
            static_cast<size_t>(width));
    }

    // Resolve transparent key robustly:
    // 1) exact magenta palette entries (255,0,255)
    // 2) near-magenta entries (high R/B, low G) for converter variance
    // 3) most-used index fallback when palette metadata is ambiguous
    const size_t paletteEntries = (headerOffset > 1u) ? ((headerOffset - 1u) / 3u) : 0u;
    std::array<uint32_t, 256> usage = {};
    for (uint8_t px : outImage->pixelsTopDown)
    {
        ++usage[px];
    }

    auto chooseMostUsed = [&usage](const std::vector<uint8_t>& indices, uint8_t* outIndex) -> bool
    {
        if (indices.empty() || outIndex == nullptr)
        {
            return false;
        }
        uint8_t best = indices[0];
        uint32_t bestUsage = usage[best];
        for (uint8_t candidate : indices)
        {
            if (usage[candidate] > bestUsage)
            {
                best = candidate;
                bestUsage = usage[candidate];
            }
        }
        *outIndex = best;
        return true;
    };

    std::vector<uint8_t> exactMagenta;
    std::vector<uint8_t> nearMagenta;
    exactMagenta.reserve(paletteEntries < 256u ? paletteEntries : 256u);
    nearMagenta.reserve(paletteEntries < 256u ? paletteEntries : 256u);

    for (size_t i = 0; i < paletteEntries && i < 256u; ++i)
    {
        const size_t base = 1u + i * 3u;
        if (base + 2u >= headerOffset)
        {
            break;
        }

        const uint8_t b = bytes[base];
        const uint8_t g = bytes[base + 1u];
        const uint8_t r = bytes[base + 2u];
        if (r == 255u && g == 0u && b == 255u)
        {
            exactMagenta.push_back(static_cast<uint8_t>(i));
        }
        else if (r >= 240u && b >= 240u && g <= 24u)
        {
            nearMagenta.push_back(static_cast<uint8_t>(i));
        }
    }

    uint8_t resolvedTransparent = 0;
    const char* transparentMethod = "none";
    if (chooseMostUsed(exactMagenta, &resolvedTransparent))
    {
        transparentMethod = "exact_magenta";
    }
    else if (chooseMostUsed(nearMagenta, &resolvedTransparent))
    {
        transparentMethod = "near_magenta";
    }
    else
    {
        // Last resort: assume dominant index is transparent for menu-style sheets.
        uint8_t dominant = 0;
        uint32_t dominantUsage = usage[0];
        for (int i = 1; i < 256; ++i)
        {
            if (usage[static_cast<size_t>(i)] > dominantUsage)
            {
                dominant = static_cast<uint8_t>(i);
                dominantUsage = usage[static_cast<size_t>(i)];
            }
        }
        resolvedTransparent = dominant;
        transparentMethod = "dominant_index_fallback";
    }

    outImage->transparentIndex = resolvedTransparent;
    outImage->hasTransparentIndex = true;

    mod::Log(
        "ParseEfzDatImage: parsed '%s' size=%zu firstByte=%u headerOffset=%zu width=%d height=%d transparentSrc=%u hasTransparent=%d method=%s exact=%zu near=%zu",
        path.c_str(),
        bytes.size(),
        static_cast<unsigned>(bytes[0]),
        headerOffset,
        width,
        height,
        static_cast<unsigned>(outImage->transparentIndex),
        outImage->hasTransparentIndex ? 1 : 0,
        transparentMethod,
        exactMagenta.size(),
        nearMagenta.size());

    return true;
}

struct RowRun
{
    int start = 0;
    int length = 0;
};

bool DeriveConfigStyleRowsFromDat(
    const ParsedDatImage& image,
    int optionCount,
    std::array<int, kNetplayConfigOptionCount>* topRows,
    std::array<int, kNetplayConfigOptionCount>* bottomRows,
    int* rowHeight)
{
    if (optionCount != kNetplayConfigOptionCount || topRows == nullptr || bottomRows == nullptr || rowHeight == nullptr)
    {
        return false;
    }

    const int width = image.width;
    const int height = image.height;
    const uint8_t transparent = image.transparentIndex;
    if (width <= 0 || height <= 0 || image.pixelsTopDown.size() != static_cast<size_t>(width) * static_cast<size_t>(height))
    {
        return false;
    }

    std::vector<uint8_t> activeRows(static_cast<size_t>(height), 0);
    for (int y = 0; y < height; ++y)
    {
        const uint8_t* row = image.pixelsTopDown.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
        int opaqueCount = 0;
        for (int x = 0; x < width; ++x)
        {
            if (row[x] != transparent)
            {
                ++opaqueCount;
            }
        }

        // Menu bars occupy almost full width. Threshold keeps text-only rows out.
        activeRows[static_cast<size_t>(y)] = (opaqueCount * 100 >= width * 90) ? 1u : 0u;
    }

    std::vector<RowRun> runs;
    for (int y = 0; y < height;)
    {
        if (activeRows[static_cast<size_t>(y)] == 0)
        {
            ++y;
            continue;
        }
        const int start = y;
        while (y < height && activeRows[static_cast<size_t>(y)] != 0)
        {
            ++y;
        }
        runs.push_back(RowRun{start, y - start});
    }

    std::vector<RowRun> topCandidates;
    std::vector<RowRun> bottomCandidates;
    const int half = height / 2;
    for (const RowRun& run : runs)
    {
        // Skip title bars and tiny noise runs.
        if (run.length < 8 || run.start < 40)
        {
            continue;
        }
        if (run.start < half)
        {
            topCandidates.push_back(run);
        }
        else if (run.start >= half + 20)
        {
            bottomCandidates.push_back(run);
        }
    }

    if (topCandidates.size() < static_cast<size_t>(optionCount)
        || bottomCandidates.size() < static_cast<size_t>(optionCount))
    {
        mod::Log(
            "DeriveConfigStyleRowsFromDat: insufficient runs (top=%zu bottom=%zu) for %d options",
            topCandidates.size(),
            bottomCandidates.size(),
            optionCount);
        return false;
    }

    int minHeight = kNetplayDefaultHighlightHeight;
    for (int i = 0; i < optionCount; ++i)
    {
        (*topRows)[static_cast<size_t>(i)] = topCandidates[static_cast<size_t>(i)].start;
        (*bottomRows)[static_cast<size_t>(i)] = bottomCandidates[static_cast<size_t>(i)].start;
        const int topLen = topCandidates[static_cast<size_t>(i)].length;
        const int bottomLen = bottomCandidates[static_cast<size_t>(i)].length;
        const int shorterLen = (topLen < bottomLen) ? topLen : bottomLen;
        minHeight = (minHeight < shorterLen) ? minHeight : shorterLen;
    }

    *rowHeight = (minHeight > 1) ? minHeight : 1;
    return true;
}

NetplayObjectProfile DetermineObjectProfile(const std::string& objectPath)
{
    NetplayObjectProfile profile;
    if (ContainsNoCase(objectPath, "config_ob.dat"))
    {
        profile.colorOffset = 161;
        profile.paletteDestStart = 162;
        profile.paletteCount = 32;
        profile.useConfigStyleRender = true;
        profile.renderLayout = {};
    }
    else if (ContainsNoCase(objectPath, "netplay_ob.dat"))
    {
        // Custom netplay object sheets follow title-style object palette slotting
        // but use the config-style menu layout/rows in our renderer.
        profile.colorOffset = 193;
        profile.paletteDestStart = 193;
        profile.paletteCount = 48;
        profile.useConfigStyleRender = true;
        profile.deriveLayoutFromDat = true;
        profile.renderLayout = {};
    }
    return profile;
}

int GetGameSystem(uint32_t screenContext)
{
    return *reinterpret_cast<int*>(screenContext + kOffsetGameSystem);
}

void*** GetGraphicsManager(uint32_t screenContext)
{
    return *reinterpret_cast<void****>(screenContext + kOffsetGameSystem);
}

void** GetGraphicsContext(uint32_t screenContext)
{
    return *reinterpret_cast<void***>(screenContext + kOffsetGraphicsContext);
}

TitleUpdateFn GetOriginalTitleUpdate()
{
    return reinterpret_cast<TitleUpdateFn>(RuntimeAddress(kVaUpdateTitleScreenLogic));
}

TitleRenderFn GetOriginalTitleRender()
{
    return reinterpret_cast<TitleRenderFn>(RuntimeAddress(kVaTitleRender));
}

TitleAuxFn GetOriginalTitleAux()
{
    return reinterpret_cast<TitleAuxFn>(RuntimeAddress(kVaTitleExitInputCheck));
}

void PlayUiSound(uint32_t screenContext, unsigned short soundIndex)
{
    auto const playSoundEffect = reinterpret_cast<PlaySoundEffectFn>(RuntimeAddress(kVaPlaySoundEffect));
    mod::Log("PlayUiSound: sfx=%u gameSystem=0x%08X", soundIndex, static_cast<unsigned>(GetGameSystem(screenContext)));
    playSoundEffect(reinterpret_cast<void*>(GetGameSystem(screenContext)), soundIndex);
}

void StopCurrentBgm(uint32_t screenContext, const char* reason)
{
    auto const stopBackgroundMusic = reinterpret_cast<StopBackgroundMusicFn>(RuntimeAddress(kVaStopBackgroundMusic));
    const int gameSystem = GetGameSystem(screenContext);
    const int result = stopBackgroundMusic(gameSystem);
    mod::Log("StopCurrentBgm: reason=%s gameSystem=0x%08X result=%d", reason, static_cast<unsigned>(gameSystem), result);
}

void ResetTitleMenuState(uint32_t screenContext, int8_t selection)
{
    *reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection) = selection;
    *reinterpret_cast<uint16_t*>(screenContext + kOffsetMenuAnimCounter) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetTitleMenuState) = 0;
    *reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetScreenInitState) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetScreenExitState) = 0;
    mod::Log(
        "ResetTitleMenuState: selection=%d menuState=%u initState=%u exitState=%u",
        static_cast<int>(selection),
        static_cast<unsigned>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTitleMenuState)),
        static_cast<unsigned>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetScreenInitState)),
        static_cast<unsigned>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetScreenExitState)));
}

void RunTransitionFadeOut(uint32_t screenContext, int baseVolume, int volumeAdjustment)
{
    auto const fadeWithSoundAdjustment =
        reinterpret_cast<FadeWithSoundAdjustmentFn>(RuntimeAddress(kVaFadeWithSoundAdjustment));
    mod::Log(
        "RunTransitionFadeOut: begin palette=%d baseVolume=%d volumeAdjustment=%d",
        static_cast<int>(screenContext + kOffsetPalette),
        baseVolume,
        volumeAdjustment);
    const int fadeResult = fadeWithSoundAdjustment(
        reinterpret_cast<void*>(screenContext),
        static_cast<int>(screenContext + kOffsetPalette),
        1u,
        baseVolume,
        volumeAdjustment);
    mod::Log("RunTransitionFadeOut: complete result=%d", fadeResult);
}

void RunTransitionFadeIn(uint32_t screenContext)
{
    auto const fadeScreenEffect = reinterpret_cast<FadeScreenEffectFn>(RuntimeAddress(kVaFadeScreenEffect));
    mod::Log("RunTransitionFadeIn: begin palette=%d", static_cast<int>(screenContext + kOffsetPalette));
    const int fadeResult = fadeScreenEffect(reinterpret_cast<void*>(screenContext), static_cast<int>(screenContext + kOffsetPalette), 1u);
    mod::Log("RunTransitionFadeIn: complete result=%d", fadeResult);
}

bool LoadTitleAssets(uint32_t screenContext)
{
    mod::Log("LoadTitleAssets: begin (screenContext=0x%08X)", screenContext);
    auto const loadCompressedImageFile = reinterpret_cast<LoadCompressedImageFileFn>(RuntimeAddress(kVaLoadCompressedImageFile));
    auto const loadBgrColorsFromRawFile = reinterpret_cast<LoadBgrColorsFromRawFileFn>(RuntimeAddress(kVaLoadBgrColorsFromRawFile));
    auto const readPixelValue = reinterpret_cast<ReadPixelValueFn>(RuntimeAddress(kVaReadPixelValue));
    auto const setPalette = reinterpret_cast<SetPaletteFn>(RuntimeAddress(kVaSetPalette));

    loadCompressedImageFile(
        GetGraphicsManager(screenContext),
        reinterpret_cast<uint32_t*>(screenContext + kOffsetBackgroundSurface),
        "system\\title.dat",
        0,
        1);
    loadCompressedImageFile(
        GetGraphicsManager(screenContext),
        reinterpret_cast<uint32_t*>(screenContext + kOffsetObjectsSurface),
        "system\\title_ob.dat",
        0,
        193);

    const bool bgPaletteOk = loadBgrColorsFromRawFile(static_cast<int>(screenContext + kOffsetPalette), "system\\title.dat", 0, 1, 192) != 0;
    const bool objPaletteOk = loadBgrColorsFromRawFile(static_cast<int>(screenContext + kOffsetPalette), "system\\title_ob.dat", 0, 193, 48) != 0;

    *reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor) = static_cast<uint8_t>(readPixelValue(*reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface)));
    setPalette(GetGraphicsContext(screenContext), static_cast<int>(screenContext + kOffsetPalette));

    mod::Log(
        "LoadTitleAssets: done bgPalette=%d objPalette=%d transparent=%u",
        bgPaletteOk,
        objPaletteOk,
        static_cast<unsigned>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor)));
    return bgPaletteOk && objPaletteOk;
}

bool LoadNetplayAssets(uint32_t screenContext)
{
    mod::Log("LoadNetplayAssets: begin (screenContext=0x%08X)", screenContext);
    const std::string bgPath = ResolveNetplayBackgroundPath();
    if (bgPath.empty())
    {
        mod::Log("LoadNetplayAssets: netplay background not found near DLL assets folder");
        return false;
    }

    const std::string objPath = ResolveNetplayObjectsPath();
    if (!objPath.empty() && objPath != "system\\title_ob.dat" && !FileExists(objPath))
    {
        mod::Log("LoadNetplayAssets: objects path missing (%s), fallback to title objects", objPath.c_str());
    }

    const char* objectsPath = objPath.c_str();
    NetplayObjectProfile objectProfile = DetermineObjectProfile(objPath);
    g_netplayMenuState.useConfigStyleRender = objectProfile.useConfigStyleRender;
    g_netplayMenuState.optionCount = static_cast<int>(kMainMenuEntries.size());
    g_netplayMenuState.backIndex = static_cast<int>(kMainMenuEntries.size()) - 1;
    g_netplayMenuState.paletteStart = static_cast<uint8_t>(objectProfile.paletteDestStart);
    g_netplayMenuState.paletteCount = static_cast<uint8_t>(objectProfile.paletteCount);
    g_netplayMenuState.renderLayout = objectProfile.renderLayout;
    bool hasDerivedTransparentIndex = false;
    uint8_t derivedTransparentIndex = 0;

    if (objectProfile.deriveLayoutFromDat)
    {
        ParsedDatImage image;
        if (ParseEfzDatImage(objPath, &image))
        {
            if (image.hasTransparentIndex)
            {
                hasDerivedTransparentIndex = true;
                derivedTransparentIndex = image.transparentIndex;
                mod::Log(
                    "LoadNetplayAssets: derived source transparent index=%u from netplay_ob.dat",
                    static_cast<unsigned>(derivedTransparentIndex));
            }

            std::array<int, kNetplayConfigOptionCount> topRows = {};
            std::array<int, kNetplayConfigOptionCount> bottomRows = {};
            int rowHeight = kNetplayDefaultHighlightHeight;
            if (DeriveConfigStyleRowsFromDat(image, kNetplayConfigOptionCount, &topRows, &bottomRows, &rowHeight))
            {
                objectProfile.renderLayout.highlightDestY = topRows;
                objectProfile.renderLayout.highlightSourceY = bottomRows;
                objectProfile.renderLayout.highlightHeight = rowHeight;
                const int width = (image.width > 0 && image.width <= 320) ? image.width : 320;
                objectProfile.renderLayout.highlightWidth.fill(width);
                g_netplayMenuState.renderLayout = objectProfile.renderLayout;
                mod::Log(
                    "LoadNetplayAssets: derived netplay_ob layout srcY[%d..%d] dstY[%d..%d] rowH=%d width=%d",
                    objectProfile.renderLayout.highlightSourceY.front(),
                    objectProfile.renderLayout.highlightSourceY.back(),
                    objectProfile.renderLayout.highlightDestY.front(),
                    objectProfile.renderLayout.highlightDestY.back(),
                    objectProfile.renderLayout.highlightHeight,
                    width);
            }
            else
            {
                mod::Log("LoadNetplayAssets: failed to derive netplay_ob layout; using default coordinates");
            }
        }
        else
        {
            mod::Log("LoadNetplayAssets: failed to parse netplay_ob.dat for coordinates; using default coordinates");
        }
    }

    auto const loadCompressedImageFile = reinterpret_cast<LoadCompressedImageFileFn>(RuntimeAddress(kVaLoadCompressedImageFile));
    auto const loadBgrColorsFromRawFile = reinterpret_cast<LoadBgrColorsFromRawFileFn>(RuntimeAddress(kVaLoadBgrColorsFromRawFile));
    auto const readPixelValue = reinterpret_cast<ReadPixelValueFn>(RuntimeAddress(kVaReadPixelValue));
    auto const setPalette = reinterpret_cast<SetPaletteFn>(RuntimeAddress(kVaSetPalette));

    loadCompressedImageFile(
        GetGraphicsManager(screenContext),
        reinterpret_cast<uint32_t*>(screenContext + kOffsetBackgroundSurface),
        bgPath.c_str(),
        0,
        1);
    loadCompressedImageFile(
        GetGraphicsManager(screenContext),
        reinterpret_cast<uint32_t*>(screenContext + kOffsetObjectsSurface),
        objectsPath,
        0,
        objectProfile.colorOffset);

    const bool bgPaletteOk =
        loadBgrColorsFromRawFile(static_cast<int>(screenContext + kOffsetPalette), bgPath.c_str(), 0, 1, 192) != 0;
    const bool objPaletteOk =
        loadBgrColorsFromRawFile(
            static_cast<int>(screenContext + kOffsetPalette),
            objectsPath,
            0,
            objectProfile.paletteDestStart,
            objectProfile.paletteCount)
        != 0;

    uint8_t transparentColor = static_cast<uint8_t>(readPixelValue(*reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface)));
    if (hasDerivedTransparentIndex)
    {
        // Pixel indices are loaded into palette slots offset by colorOffset.
        transparentColor = static_cast<uint8_t>(derivedTransparentIndex + objectProfile.colorOffset);
        mod::Log(
            "LoadNetplayAssets: forcing transparent color=%u (src=%u + offset=%u)",
            static_cast<unsigned>(transparentColor),
            static_cast<unsigned>(derivedTransparentIndex),
            static_cast<unsigned>(objectProfile.colorOffset));
    }
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor) = transparentColor;
    setPalette(GetGraphicsContext(screenContext), static_cast<int>(screenContext + kOffsetPalette));

    mod::Log(
        "LoadNetplayAssets: done bg='%s' obj='%s' (palette bg=%d obj=%d transparent=%u)",
        bgPath.c_str(),
        objectsPath,
        bgPaletteOk,
        objPaletteOk,
        static_cast<unsigned>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor)));
    mod::Log(
        "LoadNetplayAssets: object profile colorOffset=%u paletteStart=%d paletteCount=%d configStyle=%d optionCount=%d backIndex=%d",
        static_cast<unsigned>(objectProfile.colorOffset),
        objectProfile.paletteDestStart,
        objectProfile.paletteCount,
        g_netplayMenuState.useConfigStyleRender,
        g_netplayMenuState.optionCount,
        g_netplayMenuState.backIndex);

    const bool spriteFontLoaded = LoadNetplaySpriteFont();
    g_useRuntimeTextOverlay = spriteFontLoaded;
    if (!spriteFontLoaded)
    {
        mod::Log("LoadNetplayAssets: sprite font unavailable, using object-sheet-only rendering");
    }
    else
    {
        mod::Log("LoadNetplayAssets: sprite font available, using runtime text overlay");
    }
    return bgPaletteOk && objPaletteOk;
}

std::string TrimAscii(std::string value)
{
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
    {
        value.pop_back();
    }

    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t'))
    {
        ++start;
    }

    if (start > 0)
    {
        value.erase(0, start);
    }
    return value;
}

bool ParseIntToken(const std::string& token, int* outValue)
{
    if (outValue == nullptr || token.empty())
    {
        return false;
    }

    char* end = nullptr;
    const long parsed = std::strtol(token.c_str(), &end, 10);
    if (end == nullptr || *end != '\0')
    {
        return false;
    }

    *outValue = static_cast<int>(parsed);
    return true;
}

bool LoadSpriteFontMapFromFile(const std::string& path, SpriteFont* outFont)
{
    if (outFont == nullptr)
    {
        return false;
    }

    std::ifstream file(path);
    if (!file)
    {
        return false;
    }

    SpriteFont parsed = {};
    std::string line;
    int lineNo = 0;
    while (std::getline(file, line))
    {
        ++lineNo;
        line = TrimAscii(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        const size_t eqPos = line.find('=');
        if (eqPos == std::string::npos)
        {
            continue;
        }

        std::string key = TrimAscii(line.substr(0, eqPos));
        std::string value = TrimAscii(line.substr(eqPos + 1));
        if (key.empty() || value.empty())
        {
            continue;
        }

        if (key == "line_height")
        {
            int v = 0;
            if (ParseIntToken(value, &v) && v > 0)
            {
                parsed.lineHeight = v;
            }
            continue;
        }
        if (key == "letter_spacing")
        {
            int v = 0;
            if (ParseIntToken(value, &v) && v >= 0)
            {
                parsed.letterSpacing = v;
            }
            continue;
        }
        if (key == "uppercase_input")
        {
            parsed.uppercaseInput = !(value == "0" || value == "false" || value == "False");
            continue;
        }

        // Glyph line format:
        // A=srcX,srcY,width,height[,advance]
        if (key.size() != 1)
        {
            continue;
        }

        std::array<int, 5> numbers = {};
        int numberCount = 0;
        size_t cursor = 0;
        while (cursor < value.size() && numberCount < static_cast<int>(numbers.size()))
        {
            size_t comma = value.find(',', cursor);
            std::string token = (comma == std::string::npos) ? value.substr(cursor) : value.substr(cursor, comma - cursor);
            token = TrimAscii(token);
            int parsedValue = 0;
            if (!ParseIntToken(token, &parsedValue))
            {
                numberCount = 0;
                break;
            }
            numbers[static_cast<size_t>(numberCount)] = parsedValue;
            ++numberCount;
            if (comma == std::string::npos)
            {
                break;
            }
            cursor = comma + 1;
        }

        if (numberCount < 4)
        {
            mod::Log("LoadSpriteFontMap: ignored malformed glyph at line %d", lineNo);
            continue;
        }

        SpriteGlyph glyph = {};
        glyph.srcX = numbers[0];
        glyph.srcY = numbers[1];
        glyph.width = numbers[2];
        glyph.height = numbers[3];
        glyph.advance = (numberCount >= 5) ? numbers[4] : numbers[2];
        if (glyph.width <= 0 || glyph.height <= 0 || glyph.advance <= 0)
        {
            continue;
        }
        parsed.glyphs[key[0]] = glyph;
    }

    parsed.loaded = !parsed.glyphs.empty();
    if (!parsed.loaded)
    {
        return false;
    }

    *outFont = parsed;
    return true;
}

bool LoadNetplaySpriteFont()
{
    g_spriteFont = {};

    const std::array<const char*, 3> candidates = {
        "assets\\netplay_font_map.txt",
        "assets\\font_map.txt",
        "netplay_font_map.txt",
    };

    for (const char* candidate : candidates)
    {
        const std::string path = JoinPath(g_moduleDirectory, candidate);
        if (!FileExists(path))
        {
            continue;
        }

        if (LoadSpriteFontMapFromFile(path, &g_spriteFont))
        {
            mod::Log(
                "LoadNetplaySpriteFont: loaded '%s' glyphs=%zu lineHeight=%d spacing=%d uppercase=%d",
                path.c_str(),
                g_spriteFont.glyphs.size(),
                g_spriteFont.lineHeight,
                g_spriteFont.letterSpacing,
                g_spriteFont.uppercaseInput ? 1 : 0);
            return true;
        }
        mod::Log("LoadNetplaySpriteFont: failed to parse '%s'", path.c_str());
    }

    mod::Log("LoadNetplaySpriteFont: no sprite font map found (runtime text disabled)");
    return false;
}

std::string BuildMenuHeaderText()
{
    const NetplayMenuSpec* spec = GetMenuSpec(g_netplayMenuState.menuId);
    if (spec == nullptr || spec->headerLabel == nullptr)
    {
        return "NETPLAY";
    }
    return spec->headerLabel;
}

std::string BuildRowLabel(const NetplayMenuEntry& entry)
{
    std::string value;
    switch (entry.action)
    {
    case NetplayMenuAction::OpenHost:
        return "HOST";
    case NetplayMenuAction::OpenJoin:
        return "JOIN";
    case NetplayMenuAction::OpenNickname:
        return "CHANGE NICKNAME";
    case NetplayMenuAction::LeaveNetplay:
        return "RETURN TO TITLE";
    case NetplayMenuAction::HostStart:
        return "START HOST";
    case NetplayMenuAction::HostEditPort:
        if (GetInlineEditDisplayValue(entry.action, &value, true))
        {
            return "PORT: " + value;
        }
        return "PORT";
    case NetplayMenuAction::BackToMain:
        return "BACK";
    case NetplayMenuAction::JoinConnect:
        return "CONNECT";
    case NetplayMenuAction::JoinEditAddress:
        if (GetInlineEditDisplayValue(entry.action, &value, true))
        {
            return "ADDRESS: " + value;
        }
        return "ADDRESS";
    case NetplayMenuAction::JoinEditPort:
        if (GetInlineEditDisplayValue(entry.action, &value, true))
        {
            return "PORT: " + value;
        }
        return "PORT";
    case NetplayMenuAction::NicknameEdit:
        if (GetInlineEditDisplayValue(entry.action, &value, true))
        {
            return "NAME: " + value;
        }
        return "NAME";
    default:
        return entry.debugLabel;
    }
}

std::string BuildFooterText()
{
    if (g_inlineEditState.active)
    {
        if (!g_inlineEditState.errorMessage.empty() && GetTickCount() < g_inlineEditState.errorExpireTick)
        {
            return g_inlineEditState.errorMessage + "  ENTER=SAVE ESC=CANCEL";
        }

        switch (g_inlineEditState.action)
        {
        case NetplayMenuAction::HostEditPort:
            return "EDIT HOST PORT  ENTER=SAVE ESC=CANCEL";
        case NetplayMenuAction::JoinEditAddress:
            return "EDIT JOIN ADDRESS  ENTER=SAVE ESC=CANCEL";
        case NetplayMenuAction::JoinEditPort:
            return "EDIT JOIN PORT  ENTER=SAVE ESC=CANCEL";
        case NetplayMenuAction::NicknameEdit:
            return "EDIT NICKNAME  ENTER=SAVE ESC=CANCEL";
        default:
            return "ENTER=SAVE ESC=CANCEL";
        }
    }

    char buffer[256] = {};
    snprintf(
        buffer,
        sizeof(buffer),
        "Nick: %s   Host:%u   Join:%s:%u",
        g_netplayMenuState.nickname.c_str(),
        static_cast<unsigned>(g_netplayMenuState.hostPort),
        g_netplayMenuState.joinAddress.c_str(),
        static_cast<unsigned>(g_netplayMenuState.joinPort));
    return buffer;
}

HFONT GetMenuOverlayFont()
{
    if (g_menuOverlayFont != nullptr)
    {
        return g_menuOverlayFont;
    }

    g_menuOverlayFont = CreateFontA(
        -14,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        SHIFTJIS_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        NONANTIALIASED_QUALITY,
        FIXED_PITCH | FF_MODERN,
        "MS Gothic");

    if (g_menuOverlayFont == nullptr)
    {
        g_menuOverlayFont = CreateFontA(
            -14,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            ANSI_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            NONANTIALIASED_QUALITY,
            FIXED_PITCH | FF_MODERN,
            "Terminal");
    }

    if (g_menuOverlayFont == nullptr)
    {
        g_menuOverlayFont = reinterpret_cast<HFONT>(GetStockObject(SYSTEM_FIXED_FONT));
    }

    return g_menuOverlayFont;
}

bool IsExecutableAddress(uint32_t address)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0)
    {
        return false;
    }
    if (mbi.State != MEM_COMMIT)
    {
        return false;
    }
    const DWORD protect = mbi.Protect & 0xFF;
    return protect == PAGE_EXECUTE
        || protect == PAGE_EXECUTE_READ
        || protect == PAGE_EXECUTE_READWRITE
        || protect == PAGE_EXECUTE_WRITECOPY;
}

bool TryAcquireSurfaceDc(
    void* surface,
    const char* label,
    HDC* outDc)
{
    if (surface == nullptr || outDc == nullptr)
    {
        return false;
    }

    const uint32_t vtable = *reinterpret_cast<uint32_t*>(surface);
    if (vtable == 0)
    {
        return false;
    }
    const uint32_t getDcAddress = *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceGetDc);
    if (getDcAddress == 0 || !IsExecutableAddress(getDcAddress))
    {
        return false;
    }

    auto const getDc = reinterpret_cast<HRESULT(__stdcall*)(void*, HDC*)>(getDcAddress);
    HDC surfaceDc = nullptr;
    const HRESULT hr = getDc(surface, &surfaceDc);
    if (SUCCEEDED(hr) && surfaceDc != nullptr)
    {
        if (!g_loggedSurfaceDcOk)
        {
            mod::Log("AcquireMenuDrawDc: using %s surface DC", label);
            g_loggedSurfaceDcOk = true;
        }
        *outDc = surfaceDc;
        return true;
    }
    if (!g_loggedSurfaceDcUnavailable)
    {
        mod::Log(
            "AcquireMenuDrawDc: %s surface GetDC failed (surface=0x%08X hr=0x%08X)",
            label,
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(surface)),
            static_cast<unsigned>(hr));
    }
    return false;
}

bool AcquireMenuDrawDc(
    uint32_t screenContext,
    HDC* outDc,
    void** outSurface,
    HWND* outWindow,
    bool allowWindowDc)
{
    if (outDc == nullptr || outSurface == nullptr || outWindow == nullptr)
    {
        return false;
    }

    *outDc = nullptr;
    *outSurface = nullptr;
    *outWindow = nullptr;

    const uint32_t graphicsContext = *reinterpret_cast<uint32_t*>(screenContext + kOffsetGraphicsContext);
    if (graphicsContext != 0)
    {
        void* backBufferSurface = *reinterpret_cast<void**>(graphicsContext + kOffsetGraphicsBackBufferSurface);
        if (TryAcquireSurfaceDc(backBufferSurface, "backbuffer", outDc))
        {
            *outSurface = backBufferSurface;
            return true;
        }

        void* primarySurface = *reinterpret_cast<void**>(graphicsContext + kOffsetGraphicsPrimarySurface);
        if (TryAcquireSurfaceDc(primarySurface, "primary", outDc))
        {
            *outSurface = primarySurface;
            return true;
        }
    }

    if (allowWindowDc)
    {
        const HWND hwnd = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
        if (hwnd != nullptr && IsWindow(hwnd))
        {
            HDC windowDc = GetDC(hwnd);
            if (windowDc != nullptr)
            {
                if (!g_loggedWindowDcFallback)
                {
                    mod::Log("AcquireMenuDrawDc: using window DC fallback");
                    g_loggedWindowDcFallback = true;
                }
                *outDc = windowDc;
                *outWindow = hwnd;
                return true;
            }
        }
    }
    if (!g_loggedSurfaceDcUnavailable)
    {
        mod::Log("AcquireMenuDrawDc: no usable surface DC");
        g_loggedSurfaceDcUnavailable = true;
    }
    return false;
}

void ReleaseMenuDrawDc(HDC dc, void* surface, HWND window)
{
    if (dc == nullptr)
    {
        return;
    }

    if (surface != nullptr)
    {
        const uint32_t vtable = *reinterpret_cast<uint32_t*>(surface);
        if (vtable != 0)
        {
            const uint32_t releaseAddress = *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceReleaseDc);
            if (releaseAddress != 0 && IsExecutableAddress(releaseAddress))
            {
                auto const releaseDc = reinterpret_cast<HRESULT(__stdcall*)(void*, HDC)>(releaseAddress);
                (void)releaseDc(surface, dc);
                return;
            }
        }
    }
    if (window != nullptr && IsWindow(window))
    {
        ReleaseDC(window, dc);
    }
}

struct LockedMenuSurface
{
    void* surface = nullptr;
    uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int pitch = 0;
    void* lockToken = nullptr;
    const char* label = "";
};

bool QuerySurfaceDimensions(void* surface, int* outWidth, int* outHeight)
{
    if (surface == nullptr || outWidth == nullptr || outHeight == nullptr)
    {
        return false;
    }

    const uint32_t vtable = *reinterpret_cast<uint32_t*>(surface);
    if (vtable == 0)
    {
        return false;
    }

    constexpr uint32_t kVtableOffsetSurfaceGetDesc = 88; // IDirectDrawSurface::GetSurfaceDesc
    const uint32_t getDescAddress = *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceGetDesc);
    if (getDescAddress == 0 || !IsExecutableAddress(getDescAddress))
    {
        return false;
    }

    auto const getDesc = reinterpret_cast<HRESULT(__stdcall*)(void*, uint32_t*)>(getDescAddress);
    uint32_t desc[36] = {};
    desc[0] = 124; // DDSURFACEDESC2
    const HRESULT hr = getDesc(surface, desc);
    if (!SUCCEEDED(hr))
    {
        return false;
    }

    const int width = static_cast<int>(desc[3]);
    const int height = static_cast<int>(desc[2]);
    if (width <= 0 || height <= 0)
    {
        return false;
    }

    *outWidth = width;
    *outHeight = height;
    return true;
}

bool TryLockMenuSurface(void* surface, const char* label, LockedMenuSurface* outSurface)
{
    if (surface == nullptr || outSurface == nullptr)
    {
        return false;
    }

    const uint32_t vtable = *reinterpret_cast<uint32_t*>(surface);
    if (vtable == 0)
    {
        return false;
    }
    const uint32_t lockAddress = *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceLock);
    if (lockAddress == 0 || !IsExecutableAddress(lockAddress))
    {
        return false;
    }

    auto const lockSurface = reinterpret_cast<HRESULT(__stdcall*)(void*, void*, uint32_t*, int, uint32_t)>(lockAddress);
    auto const unlockSurface = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(
        *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceUnlock));
    uint32_t lockDesc[36] = {};
    // IDirectDrawSurface7::Lock expects DDSURFACEDESC2 (124 bytes).
    lockDesc[0] = 124;
    const HRESULT hr = lockSurface(surface, nullptr, lockDesc, 1, 0);
    if (SUCCEEDED(hr))
    {
        const uintptr_t ptr9 = static_cast<uintptr_t>(lockDesc[9]); // lpSurface in DDSURFACEDESC2
        const uintptr_t ptr8 = static_cast<uintptr_t>(lockDesc[8]); // older layout fallback
        const uintptr_t pixelPtr = (ptr9 != 0u) ? ptr9 : ptr8;
        int width = static_cast<int>(lockDesc[3]);
        int height = static_cast<int>(lockDesc[2]);
        const int pitch = static_cast<int>(lockDesc[4]);

        int descWidth = 0;
        int descHeight = 0;
        if ((width <= 0 || height <= 0) && QuerySurfaceDimensions(surface, &descWidth, &descHeight))
        {
            width = (width > 0) ? width : descWidth;
            height = (height > 0) ? height : descHeight;
        }
        if (width <= 0 && pitch > 0)
        {
            // EFZ runs 8-bit indexed surfaces, so pitch is a usable width fallback.
            width = pitch;
        }
        if (height <= 0)
        {
            height = 240;
        }

        if (pixelPtr == 0u || pitch <= 0 || width <= 0 || height <= 0)
        {
            if (unlockSurface != nullptr && IsExecutableAddress(reinterpret_cast<uint32_t>(unlockSurface)))
            {
                (void)unlockSurface(surface, reinterpret_cast<void*>(pixelPtr));
            }
            if (!g_loggedSurfaceLockUnavailable)
            {
                mod::Log(
                    "AcquireMenuDrawSurfaceLock: %s lock payload invalid (surface=0x%08X ptr9=0x%08X ptr8=0x%08X pitch=%u w=%d h=%d descW=%d descH=%d)",
                    label,
                    static_cast<unsigned>(reinterpret_cast<uintptr_t>(surface)),
                    static_cast<unsigned>(ptr9),
                    static_cast<unsigned>(ptr8),
                    static_cast<unsigned>(lockDesc[4]),
                    width,
                    height,
                    descWidth,
                    descHeight);
            }
            return false;
        }

        outSurface->surface = surface;
        outSurface->width = width;
        outSurface->height = height;
        outSurface->pitch = pitch;
        outSurface->pixels = reinterpret_cast<uint8_t*>(pixelPtr);
        outSurface->lockToken = reinterpret_cast<void*>(pixelPtr);
        outSurface->label = label;
        if (!g_loggedSurfaceLockOk)
        {
            mod::Log(
                "AcquireMenuDrawSurfaceLock: locked %s surface (surface=0x%08X size=%dx%d pitch=%d)",
                label,
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(surface)),
                outSurface->width,
                outSurface->height,
                outSurface->pitch);
            g_loggedSurfaceLockOk = true;
        }
        if ((strcmp(label, "backbuffer") == 0 && !g_loggedBackbufferSurfaceDesc)
            || (strcmp(label, "primary") == 0 && !g_loggedPrimarySurfaceDesc))
        {
            mod::Log(
                "AcquireMenuDrawSurfaceLock: %s surface details size=%dx%d pitch=%d (lockW=%u lockH=%u)",
                label,
                outSurface->width,
                outSurface->height,
                outSurface->pitch,
                static_cast<unsigned>(lockDesc[3]),
                static_cast<unsigned>(lockDesc[2]));
            if (strcmp(label, "backbuffer") == 0)
            {
                g_loggedBackbufferSurfaceDesc = true;
            }
            else if (strcmp(label, "primary") == 0)
            {
                g_loggedPrimarySurfaceDesc = true;
            }
        }
        return true;
    }

    if (!g_loggedSurfaceLockUnavailable)
    {
        mod::Log(
            "AcquireMenuDrawSurfaceLock: %s surface Lock failed (surface=0x%08X hr=0x%08X)",
            label,
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(surface)),
            static_cast<unsigned>(hr));
    }
    return false;
}

bool AcquireMenuDrawSurfaceLock(uint32_t screenContext, LockedMenuSurface* outSurface)
{
    if (outSurface == nullptr)
    {
        return false;
    }

    *outSurface = {};
    const uint32_t graphicsContext = *reinterpret_cast<uint32_t*>(screenContext + kOffsetGraphicsContext);
    if (graphicsContext == 0)
    {
        return false;
    }

    void* backBufferSurface = *reinterpret_cast<void**>(graphicsContext + kOffsetGraphicsBackBufferSurface);
    if (TryLockMenuSurface(backBufferSurface, "backbuffer", outSurface))
    {
        return true;
    }

    void* primarySurface = *reinterpret_cast<void**>(graphicsContext + kOffsetGraphicsPrimarySurface);
    if (TryLockMenuSurface(primarySurface, "primary", outSurface))
    {
        return true;
    }

    if (!g_loggedSurfaceLockUnavailable)
    {
        mod::Log("AcquireMenuDrawSurfaceLock: no lockable menu surface");
        g_loggedSurfaceLockUnavailable = true;
    }
    return false;
}

void ReleaseMenuDrawSurfaceLock(const LockedMenuSurface& surface)
{
    if (surface.surface == nullptr || surface.lockToken == nullptr)
    {
        return;
    }
    const uint32_t vtable = *reinterpret_cast<uint32_t*>(surface.surface);
    if (vtable == 0)
    {
        return;
    }
    const uint32_t unlockAddress = *reinterpret_cast<uint32_t*>(vtable + kVtableOffsetSurfaceUnlock);
    if (unlockAddress == 0 || !IsExecutableAddress(unlockAddress))
    {
        return;
    }
    auto const unlockSurface = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(unlockAddress);
    (void)unlockSurface(surface.surface, surface.lockToken);
}

const uint8_t* GetGlyph5x7(char c)
{
    switch (c)
    {
    case 'A': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}; return g; }
    case 'B': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}; return g; }
    case 'C': { static const uint8_t g[7] = {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}; return g; }
    case 'D': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}; return g; }
    case 'E': { static const uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}; return g; }
    case 'F': { static const uint8_t g[7] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}; return g; }
    case 'G': { static const uint8_t g[7] = {0x0E, 0x11, 0x10, 0x10, 0x13, 0x11, 0x0E}; return g; }
    case 'H': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}; return g; }
    case 'I': { static const uint8_t g[7] = {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case 'J': { static const uint8_t g[7] = {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E}; return g; }
    case 'K': { static const uint8_t g[7] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}; return g; }
    case 'L': { static const uint8_t g[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}; return g; }
    case 'M': { static const uint8_t g[7] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}; return g; }
    case 'N': { static const uint8_t g[7] = {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}; return g; }
    case 'O': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}; return g; }
    case 'P': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}; return g; }
    case 'Q': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}; return g; }
    case 'R': { static const uint8_t g[7] = {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}; return g; }
    case 'S': { static const uint8_t g[7] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}; return g; }
    case 'T': { static const uint8_t g[7] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}; return g; }
    case 'U': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}; return g; }
    case 'V': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}; return g; }
    case 'W': { static const uint8_t g[7] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}; return g; }
    case 'X': { static const uint8_t g[7] = {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}; return g; }
    case 'Y': { static const uint8_t g[7] = {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}; return g; }
    case 'Z': { static const uint8_t g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}; return g; }
    case 'a': { static const uint8_t g[7] = {0x00, 0x00, 0x0E, 0x01, 0x0F, 0x11, 0x0F}; return g; }
    case 'b': { static const uint8_t g[7] = {0x10, 0x10, 0x1C, 0x12, 0x11, 0x11, 0x1E}; return g; }
    case 'c': { static const uint8_t g[7] = {0x00, 0x00, 0x0E, 0x10, 0x10, 0x10, 0x0E}; return g; }
    case 'd': { static const uint8_t g[7] = {0x01, 0x01, 0x07, 0x09, 0x11, 0x11, 0x0F}; return g; }
    case 'e': { static const uint8_t g[7] = {0x00, 0x00, 0x0E, 0x11, 0x1F, 0x10, 0x0E}; return g; }
    case 'f': { static const uint8_t g[7] = {0x06, 0x08, 0x08, 0x1E, 0x08, 0x08, 0x08}; return g; }
    case 'g': { static const uint8_t g[7] = {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x0E}; return g; }
    case 'h': { static const uint8_t g[7] = {0x10, 0x10, 0x1E, 0x11, 0x11, 0x11, 0x11}; return g; }
    case 'i': { static const uint8_t g[7] = {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case 'j': { static const uint8_t g[7] = {0x02, 0x00, 0x06, 0x02, 0x02, 0x12, 0x0C}; return g; }
    case 'k': { static const uint8_t g[7] = {0x10, 0x10, 0x12, 0x14, 0x18, 0x14, 0x12}; return g; }
    case 'l': { static const uint8_t g[7] = {0x0C, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case 'm': { static const uint8_t g[7] = {0x00, 0x00, 0x1A, 0x15, 0x15, 0x11, 0x11}; return g; }
    case 'n': { static const uint8_t g[7] = {0x00, 0x00, 0x1E, 0x11, 0x11, 0x11, 0x11}; return g; }
    case 'o': { static const uint8_t g[7] = {0x00, 0x00, 0x0E, 0x11, 0x11, 0x11, 0x0E}; return g; }
    case 'p': { static const uint8_t g[7] = {0x00, 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10}; return g; }
    case 'q': { static const uint8_t g[7] = {0x00, 0x0F, 0x11, 0x11, 0x0F, 0x01, 0x01}; return g; }
    case 'r': { static const uint8_t g[7] = {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}; return g; }
    case 's': { static const uint8_t g[7] = {0x00, 0x00, 0x0F, 0x10, 0x0E, 0x01, 0x1E}; return g; }
    case 't': { static const uint8_t g[7] = {0x08, 0x08, 0x1E, 0x08, 0x08, 0x09, 0x06}; return g; }
    case 'u': { static const uint8_t g[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x13, 0x0D}; return g; }
    case 'v': { static const uint8_t g[7] = {0x00, 0x00, 0x11, 0x11, 0x11, 0x0A, 0x04}; return g; }
    case 'w': { static const uint8_t g[7] = {0x00, 0x00, 0x11, 0x11, 0x15, 0x15, 0x0A}; return g; }
    case 'x': { static const uint8_t g[7] = {0x00, 0x00, 0x11, 0x0A, 0x04, 0x0A, 0x11}; return g; }
    case 'y': { static const uint8_t g[7] = {0x00, 0x11, 0x11, 0x11, 0x0F, 0x01, 0x0E}; return g; }
    case 'z': { static const uint8_t g[7] = {0x00, 0x00, 0x1F, 0x02, 0x04, 0x08, 0x1F}; return g; }
    case '0': { static const uint8_t g[7] = {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}; return g; }
    case '1': { static const uint8_t g[7] = {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}; return g; }
    case '2': { static const uint8_t g[7] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}; return g; }
    case '3': { static const uint8_t g[7] = {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}; return g; }
    case '4': { static const uint8_t g[7] = {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}; return g; }
    case '5': { static const uint8_t g[7] = {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}; return g; }
    case '6': { static const uint8_t g[7] = {0x07, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}; return g; }
    case '7': { static const uint8_t g[7] = {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}; return g; }
    case '8': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}; return g; }
    case '9': { static const uint8_t g[7] = {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x1C}; return g; }
    case ':': { static const uint8_t g[7] = {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}; return g; }
    case '.': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}; return g; }
    case '-': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}; return g; }
    case '/': { static const uint8_t g[7] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}; return g; }
    case '_': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}; return g; }
    case '(': { static const uint8_t g[7] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}; return g; }
    case ')': { static const uint8_t g[7] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}; return g; }
    case '+': { static const uint8_t g[7] = {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}; return g; }
    case ' ': { static const uint8_t g[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; return g; }
    default:
        return nullptr;
    }
}

int MeasureText5x7(const std::string& text, int scaleX)
{
    if (text.empty())
    {
        return 0;
    }
    const int glyphW = 5 * scaleX;
    const int gap = scaleX;
    return static_cast<int>(text.size()) * (glyphW + gap) - gap;
}

void PutSurfacePixel(const LockedMenuSurface& surface, int x, int y, uint8_t color)
{
    if (surface.pixels == nullptr || x < 0 || y < 0 || x >= surface.width || y >= surface.height)
    {
        return;
    }
    surface.pixels[y * surface.pitch + x] = color;
}

void DrawGlyph5x7(
    const LockedMenuSurface& surface,
    int x,
    int y,
    char c,
    int scaleX,
    int scaleY,
    uint8_t color)
{
    const uint8_t* glyph = GetGlyph5x7(c);
    if (glyph == nullptr)
    {
        return;
    }
    for (int row = 0; row < 7; ++row)
    {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < 5; ++col)
        {
            if ((bits & (1u << (4 - col))) == 0)
            {
                continue;
            }
            const int px = x + col * scaleX;
            const int py = y + row * scaleY;
            for (int sy = 0; sy < scaleY; ++sy)
            {
                for (int sx = 0; sx < scaleX; ++sx)
                {
                    PutSurfacePixel(surface, px + sx, py + sy, color);
                }
            }
        }
    }
}

void DrawTextRight5x7(
    const LockedMenuSurface& surface,
    const std::string& text,
    int leftX,
    int rightX,
    int y,
    int scaleX,
    int scaleY,
    uint8_t color)
{
    const int maxWidth = rightX - leftX;
    if (maxWidth <= 0)
    {
        return;
    }

    std::string clipped = text;
    while (!clipped.empty() && MeasureText5x7(clipped, scaleX) > maxWidth)
    {
        clipped.erase(clipped.begin());
    }
    if (clipped.empty())
    {
        return;
    }

    const int glyphW = 5 * scaleX;
    const int gap = scaleX;
    int x = rightX - MeasureText5x7(clipped, scaleX);
    for (char c : clipped)
    {
        DrawGlyph5x7(surface, x, y, c, scaleX, scaleY, color);
        x += glyphW + gap;
    }
}

void ResolveOverlayTextPaletteColors(uint32_t screenContext, uint8_t* outSelected, uint8_t* outNormal)
{
    if (outSelected == nullptr || outNormal == nullptr)
    {
        return;
    }

    const uint8_t transparentColor = *reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor);
    const uint8_t paletteStart = g_netplayMenuState.paletteStart;
    const uint8_t paletteCount = g_netplayMenuState.paletteCount;
    const uint8_t* palette = reinterpret_cast<uint8_t*>(screenContext + kOffsetPalette);

    int bestIndex = -1;
    int bestScore = -0x7FFFFFFF;
    for (int i = 0; i < paletteCount; ++i)
    {
        const int idx = static_cast<int>(paletteStart) + i;
        if (idx < 0 || idx >= 256 || idx == static_cast<int>(transparentColor))
        {
            continue;
        }
        // Palette is stored as RGB(A=0) in game memory.
        const int r = palette[idx * 4 + 0];
        const int g = palette[idx * 4 + 1];
        const int b = palette[idx * 4 + 2];
        const int luma = 30 * r + 59 * g + 11 * b;
        const int cmax = (std::max)((std::max)(r, g), b);
        const int cmin = (std::min)((std::min)(r, g), b);
        const int saturation = cmax - cmin;
        // Prefer bright neutral colors (white/light gray), avoid vivid cyan bars.
        const int score = luma - saturation * 220;
        if (score > bestScore)
        {
            bestScore = score;
            bestIndex = idx;
        }
    }

    if (bestIndex < 0)
    {
        *outSelected = paletteStart;
        *outNormal = static_cast<uint8_t>(paletteStart + ((paletteCount > 1) ? (paletteCount - 1) : 0));
        return;
    }

    const int selR = palette[bestIndex * 4 + 0];
    const int selG = palette[bestIndex * 4 + 1];
    const int selB = palette[bestIndex * 4 + 2];
    const int selectedLuma = 30 * selR + 59 * selG + 11 * selB;
    // Pick a neutral mid-tone around ~65% brightness of selected color.
    const int targetLuma = (selectedLuma * 65) / 100;
    int normalIndex = -1;
    int bestDelta = 0x7FFFFFFF;
    for (int i = 0; i < paletteCount; ++i)
    {
        const int idx = static_cast<int>(paletteStart) + i;
        if (idx < 0 || idx >= 256 || idx == static_cast<int>(transparentColor) || idx == bestIndex)
        {
            continue;
        }
        const int r = palette[idx * 4 + 0];
        const int g = palette[idx * 4 + 1];
        const int b = palette[idx * 4 + 2];
        const int luma = 30 * r + 59 * g + 11 * b;
        const int cmax = (std::max)((std::max)(r, g), b);
        const int cmin = (std::min)((std::min)(r, g), b);
        const int saturation = cmax - cmin;
        const int delta = std::abs(luma - targetLuma) + saturation * 8;
        if (delta < bestDelta)
        {
            bestDelta = delta;
            normalIndex = idx;
        }
    }

    if (normalIndex < 0)
    {
        normalIndex = bestIndex;
    }

    *outSelected = static_cast<uint8_t>(bestIndex);
    *outNormal = static_cast<uint8_t>(normalIndex);

    if (!g_loggedOverlayPaletteChoice)
    {
        const int nr = palette[normalIndex * 4 + 0];
        const int ng = palette[normalIndex * 4 + 1];
        const int nb = palette[normalIndex * 4 + 2];
        mod::Log(
            "OverlayTextPalette: selected=%u rgb=(%d,%d,%d) normal=%u rgb=(%d,%d,%d) transparent=%u",
            static_cast<unsigned>(*outSelected),
            selR,
            selG,
            selB,
            static_cast<unsigned>(*outNormal),
            nr,
            ng,
            nb,
            static_cast<unsigned>(transparentColor));
        g_loggedOverlayPaletteChoice = true;
    }
}

bool DrawRuntimeTextOverlayGdi(uint32_t screenContext, bool allowWindowDc)
{
    if (!g_useRuntimeTextOverlay || !g_netplayMenuState.active || !g_enableGdiFallbackOverlay)
    {
        return false;
    }

    HDC dc = nullptr;
    void* surface = nullptr;
    HWND window = nullptr;
    if (!AcquireMenuDrawDc(screenContext, &dc, &surface, &window, allowWindowDc))
    {
        return false;
    }

    const bool useWindowDc = (window != nullptr);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, GetMenuOverlayFont());
    auto scaleX = [useWindowDc, window](int x)
    {
        if (!useWindowDc)
        {
            return x;
        }
        RECT clientRect = {};
        if (window == nullptr || GetClientRect(window, &clientRect) == FALSE)
        {
            return x;
        }
        const int clientW = clientRect.right - clientRect.left;
        return (clientW > 0) ? MulDiv(x, clientW, 320) : x;
    };
    auto scaleY = [useWindowDc, window](int y)
    {
        if (!useWindowDc)
        {
            return y;
        }
        RECT clientRect = {};
        if (window == nullptr || GetClientRect(window, &clientRect) == FALSE)
        {
            return y;
        }
        const int clientH = clientRect.bottom - clientRect.top;
        return (clientH > 0) ? MulDiv(y, clientH, 240) : y;
    };

    const int panelLeft = scaleX(150);
    const int panelTop = scaleY(78);
    const int panelRight = scaleX(314);
    const int rowHeight = (std::max)(16, scaleY(16));
    const int rowStep = (std::max)(rowHeight + 2, scaleY(21));

    RECT headerRect = {panelLeft, scaleY(56), panelRight, scaleY(74)};
    SetTextColor(dc, RGB(32, 32, 32));
    const std::string header = BuildMenuHeaderText();
    DrawTextA(dc, header.c_str(), -1, &headerRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    const int selection = ClampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
    int count = 0;
    const NetplayMenuEntry* entries = GetMenuEntries(g_netplayMenuState.menuId, &count);
    if (entries != nullptr && count > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            RECT rowRect = {panelLeft, panelTop + i * rowStep, panelRight, panelTop + i * rowStep + rowHeight};
            const bool isSelected = (i == selection);
            if (isSelected)
            {
                HBRUSH highlightBrush = CreateSolidBrush(RGB(110, 225, 214));
                FillRect(dc, &rowRect, highlightBrush);
                DeleteObject(highlightBrush);
            }

            const std::string label = BuildRowLabel(entries[i]);
            SetTextColor(dc, isSelected ? RGB(8, 8, 8) : RGB(108, 108, 108));
            DrawTextA(dc, label.c_str(), -1, &rowRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    RECT footerRect = {panelLeft, scaleY(224), panelRight, scaleY(238)};
    SetTextColor(dc, RGB(90, 90, 90));
    const std::string footer = BuildFooterText();
    DrawTextA(dc, footer.c_str(), -1, &footerRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (oldFont != nullptr)
    {
        SelectObject(dc, oldFont);
    }
    ReleaseMenuDrawDc(dc, surface, window);
    return true;
}

void DrawSpriteText(
    uint32_t screenContext,
    int startX,
    int startY,
    const std::string& text,
    int maxWidth,
    bool upperCaseText)
{
    if (!g_spriteFont.loaded)
    {
        return;
    }

    auto const blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));
    auto* const graphicsContext = GetGraphicsContext(screenContext);
    const int objectsSurface = *reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface);
    const char transparentColor = static_cast<char>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor));

    int x = startX;
    int y = startY;
    for (char raw : text)
    {
        char c = raw;
        if (g_spriteFont.uppercaseInput || upperCaseText)
        {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        if (c == '\n')
        {
            x = startX;
            y += g_spriteFont.lineHeight;
            continue;
        }

        if (c == ' ')
        {
            x += g_spriteFont.lineHeight / 2;
            continue;
        }

        auto it = g_spriteFont.glyphs.find(c);
        if (it == g_spriteFont.glyphs.end())
        {
            x += g_spriteFont.lineHeight / 2;
            continue;
        }

        const SpriteGlyph& glyph = it->second;
        if (maxWidth > 0 && x + glyph.width > startX + maxWidth)
        {
            break;
        }

        (void)blit(
            graphicsContext,
            x,
            y,
            x + glyph.width,
            y + glyph.height,
            objectsSurface,
            glyph.srcX,
            glyph.srcY,
            glyph.srcX + glyph.width,
            glyph.srcY + glyph.height,
            transparentColor,
            0);
        x += glyph.advance + g_spriteFont.letterSpacing;
    }
}

void DrawRuntimeSpriteOverlay(uint32_t screenContext)
{
    if (!g_useRuntimeTextOverlay || !g_netplayMenuState.active || !g_spriteFont.loaded)
    {
        return;
    }

    const int panelLeft = 152;
    const int panelRight = 316;
    const int titleY = 58;
    const int rowTextOffsetY = 1;

    DrawSpriteText(screenContext, panelLeft, titleY, BuildMenuHeaderText(), panelRight - panelLeft, true);

    const int selection = ClampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
    int count = 0;
    const NetplayMenuEntry* entries = GetMenuEntries(g_netplayMenuState.menuId, &count);
    if (entries != nullptr && count > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            const int rowIndex = entries[i].renderRow;
            if (rowIndex < 0 || rowIndex >= kNetplayConfigOptionCount)
            {
                continue;
            }

            const int y = g_netplayMenuState.renderLayout.highlightDestY[static_cast<size_t>(rowIndex)] + rowTextOffsetY;
            DrawSpriteText(screenContext, panelLeft, y, BuildRowLabel(entries[i]), panelRight - panelLeft, false);
        }
    }

    std::string footer = BuildFooterText();
    if (!g_inlineEditState.active && selection >= 0 && selection < count)
    {
        const NetplayMenuEntry& selected = entries[selection];
        if (selected.action == NetplayMenuAction::HostEditPort)
        {
            footer = "CONFIRM TO EDIT HOST PORT";
        }
        else if (selected.action == NetplayMenuAction::JoinEditAddress)
        {
            footer = "CONFIRM TO EDIT SERVER ADDRESS";
        }
        else if (selected.action == NetplayMenuAction::JoinEditPort)
        {
            footer = "CONFIRM TO EDIT SERVER PORT";
        }
        else if (selected.action == NetplayMenuAction::NicknameEdit)
        {
            footer = "CONFIRM TO EDIT NICKNAME";
        }
    }
    DrawSpriteText(screenContext, panelLeft, 224, footer, panelRight - panelLeft, false);
}

void BlitMenuRowClipped(
    BlitSurfaceWithTransparencyFn blit,
    void** graphicsContext,
    int objectsSurface,
    char transparentColor,
    int srcX,
    int srcY,
    int width,
    int height,
    int destX,
    int destY)
{
    int clippedLeft = destX;
    int clippedRight = destX + width;
    if (clippedLeft < 0)
    {
        clippedLeft = 0;
    }
    if (clippedRight > 320)
    {
        clippedRight = 320;
    }
    if (clippedRight <= clippedLeft || height <= 0)
    {
        return;
    }

    const int clippedWidth = clippedRight - clippedLeft;
    const int srcLeft = srcX + (clippedLeft - destX);
    const int srcRight = srcLeft + clippedWidth;
    (void)blit(
        graphicsContext,
        clippedLeft,
        destY,
        clippedRight,
        destY + height,
        objectsSurface,
        srcLeft,
        srcY,
        srcRight,
        srcY + height,
        transparentColor,
        0);
}

void DrawCompactMenuRows(
    uint32_t screenContext,
    NetplayMenuId menuId,
    int logicalSelection,
    int offsetX)
{
    auto const blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));
    auto* const graphicsContext = GetGraphicsContext(screenContext);
    const int objectsSurface = *reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface);
    const char transparentColor = static_cast<char>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor));

    int count = 0;
    const NetplayMenuEntry* entries = GetMenuEntries(menuId, &count);
    if (entries == nullptr || count <= 0)
    {
        return;
    }

    const int clampedSelection = (logicalSelection < 0) ? 0 : ((logicalSelection >= count) ? (count - 1) : logicalSelection);
    const int height = g_netplayMenuState.renderLayout.highlightHeight;
    const int srcX = g_netplayMenuState.renderLayout.highlightSourceX;
    const int widthDefault = 320;
    const int slideY = GetScaledNativeSlideY(screenContext);

    for (int i = 0; i < count; ++i)
    {
        const int rowIndex = entries[i].renderRow;
        if (rowIndex < 0 || rowIndex >= kNetplayConfigOptionCount)
        {
            continue;
        }

        const bool isSelected = (i == clampedSelection);
        const int srcY = isSelected
            ? g_netplayMenuState.renderLayout.highlightSourceY[static_cast<size_t>(rowIndex)]
            : g_netplayMenuState.renderLayout.highlightDestY[static_cast<size_t>(rowIndex)];
        const int width = g_netplayMenuState.renderLayout.highlightWidth[static_cast<size_t>(rowIndex)] > 0
            ? g_netplayMenuState.renderLayout.highlightWidth[static_cast<size_t>(rowIndex)]
            : widthDefault;
        const int destY = kNetplayCompactMenuTopY + i * kNetplayCompactMenuRowStep + slideY;

        BlitMenuRowClipped(
            blit,
            graphicsContext,
            objectsSurface,
            transparentColor,
            srcX,
            srcY,
            width,
            height,
            offsetX,
            destY);
    }
}

void DrawCompactMenuTitle(uint32_t screenContext)
{
    auto const blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));
    auto* const graphicsContext = GetGraphicsContext(screenContext);
    const int objectsSurface = *reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface);
    const char transparentColor = static_cast<char>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor));
    constexpr int titleY = 0;
    // Title lane occupies top strip in generated netplay_ob sheets.
    BlitMenuRowClipped(
        blit,
        graphicsContext,
        objectsSurface,
        transparentColor,
        0,
        0,
        320,
        14,
        0,
        titleY);
}

bool DrawDynamicFieldValuesGdi(uint32_t screenContext, bool allowWindowDc)
{
    if (!g_netplayMenuState.active || g_useRuntimeTextOverlay || IsMenuSlideTransitionActive())
    {
        return false;
    }

    int count = 0;
    const NetplayMenuEntry* entries = GetMenuEntries(g_netplayMenuState.menuId, &count);
    const int selected = ClampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));

    bool hasDynamicField = false;
    if (entries != nullptr && count > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            if (IsInlineEditableAction(entries[i].action))
            {
                hasDynamicField = true;
                break;
            }
        }
    }
    // Menus without editable values are considered handled; no overlay work needed.
    if (!hasDynamicField)
    {
        return true;
    }

    // Primary path: draw directly into a locked game surface (no HWND GDI flicker).
    LockedMenuSurface lockedSurface;
    if (AcquireMenuDrawSurfaceLock(screenContext, &lockedSurface))
    {
        bool drewText = false;
        const bool highResSurface = (lockedSurface.width >= 640 && lockedSurface.height >= 480);
        const int fontScaleX = 1;
        const int fontScaleY = 1;
        const int glyphHeight = 7 * fontScaleY;
        uint8_t colorSelected = g_netplayMenuState.paletteStart;
        uint8_t colorNormal = g_netplayMenuState.paletteStart;
        ResolveOverlayTextPaletteColors(screenContext, &colorSelected, &colorNormal);

        if (entries != nullptr && count > 0)
        {
            for (int i = 0; i < count; ++i)
            {
                std::string value;
                if (!GetInlineEditDisplayValue(entries[i].action, &value, true))
                {
                    continue;
                }

                const int slideY = GetScaledNativeSlideY(screenContext);
                const int rowTop = kNetplayCompactMenuTopY + i * kNetplayCompactMenuRowStep + slideY;
                const int rowBottom = rowTop + g_netplayMenuState.renderLayout.highlightHeight;
                const int leftX = highResSurface ? 182 : MulDiv(182, lockedSurface.width, 320);
                const int rightX = highResSurface ? 314 : MulDiv(314, lockedSurface.width, 320);
                const int topY = highResSurface ? rowTop : MulDiv(rowTop, lockedSurface.height, 240);
                const int bottomY = highResSurface ? rowBottom : MulDiv(rowBottom, lockedSurface.height, 240);
                const int rowHeight = bottomY - topY;
                const int textY = topY + ((rowHeight > glyphHeight) ? ((rowHeight - glyphHeight) / 2) : 0);
                const uint8_t color = (i == selected) ? colorSelected : colorNormal;

                DrawTextRight5x7(
                    lockedSurface,
                    value,
                    leftX,
                    rightX - fontScaleX,
                    textY,
                    fontScaleX,
                    fontScaleY,
                    color);
                drewText = true;
            }
        }

        ReleaseMenuDrawSurfaceLock(lockedSurface);
        if (drewText)
        {
            return true;
        }
    }

    // Fallback via surface/window DC path. Surface DC is attempted first;
    // window DC is only used as fallback by caller policy.
    HDC dc = nullptr;
    void* surface = nullptr;
    HWND window = nullptr;
    if (!AcquireMenuDrawDc(screenContext, &dc, &surface, &window, allowWindowDc))
    {
        return false;
    }

    const bool useWindowDc = (window != nullptr);
    auto scaleX = [useWindowDc, window](int x)
    {
        if (!useWindowDc)
        {
            return x;
        }
        RECT clientRect = {};
        if (window == nullptr || GetClientRect(window, &clientRect) == FALSE)
        {
            return x;
        }
        const int clientW = clientRect.right - clientRect.left;
        return (clientW > 0) ? MulDiv(x, clientW, 320) : x;
    };
    auto scaleY = [useWindowDc, window](int y)
    {
        if (!useWindowDc)
        {
            return y;
        }
        RECT clientRect = {};
        if (window == nullptr || GetClientRect(window, &clientRect) == FALSE)
        {
            return y;
        }
        const int clientH = clientRect.bottom - clientRect.top;
        return (clientH > 0) ? MulDiv(y, clientH, 240) : y;
    };

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, GetMenuOverlayFont());

    if (entries != nullptr && count > 0)
    {
        for (int i = 0; i < count; ++i)
        {
            std::string value;
            if (!GetInlineEditDisplayValue(entries[i].action, &value, true))
            {
                continue;
            }

            const int slideY = GetScaledNativeSlideY(screenContext);
            RECT rowRect = {
                scaleX(182),
                scaleY(kNetplayCompactMenuTopY + i * kNetplayCompactMenuRowStep + slideY),
                scaleX(314),
                scaleY(kNetplayCompactMenuTopY + i * kNetplayCompactMenuRowStep + g_netplayMenuState.renderLayout.highlightHeight + slideY),
            };
            SetTextColor(dc, (i == selected) ? RGB(255, 255, 255) : RGB(186, 186, 186));
            DrawTextA(dc, value.c_str(), -1, &rowRect, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    if (oldFont != nullptr)
    {
        SelectObject(dc, oldFont);
    }
    ReleaseMenuDrawDc(dc, surface, window);
    return true;
}

void DrawAnimatedCompactMenuLayer(uint32_t screenContext)
{
    auto const blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));
    auto* const graphicsContext = GetGraphicsContext(screenContext);
    const int backgroundSurface = *reinterpret_cast<int*>(screenContext + kOffsetBackgroundSurface);

    (void)blit(graphicsContext, 0, 0, 320, 240, backgroundSurface, 0, 0, 320, 240, 0, 0);
    DrawCompactMenuTitle(screenContext);
    const int logicalSelection = ClampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
    DrawCompactMenuRows(screenContext, g_netplayMenuState.menuId, logicalSelection, 0);
}

void DrawNetplayBaseLayer(uint32_t screenContext)
{
    auto const blit = reinterpret_cast<BlitSurfaceWithTransparencyFn>(RuntimeAddress(kVaBlitSurfaceWithTransparency));

    auto* const graphicsContext = GetGraphicsContext(screenContext);
    const int backgroundSurface = *reinterpret_cast<int*>(screenContext + kOffsetBackgroundSurface);
    const int objectsSurface = *reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface);
    const char transparentColor = static_cast<char>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor));

    (void)blit(graphicsContext, 0, 0, 320, 240, backgroundSurface, 0, 0, 320, 240, 0, 0);
    (void)blit(graphicsContext, 0, 0, 320, 240, objectsSurface, 0, 0, 320, 240, transparentColor, 0);

    // In object-sheet-only mode, hide rows not used by the active submenu by
    // overblitting a reserved blank row lane. This keeps static netplay_ob text
    // from leaking rows that are not navigable in the current menu.
    if (g_netplayMenuState.useConfigStyleRender && !g_useRuntimeTextOverlay)
    {
        constexpr int kBlankRowIndex = RowToIndex(NetplayObRow::Reserved5);
        if (kBlankRowIndex >= 0 && kBlankRowIndex < kNetplayConfigOptionCount)
        {
            const int blankSrcY = g_netplayMenuState.renderLayout.highlightDestY[static_cast<size_t>(kBlankRowIndex)];
            const int blankSrcX = g_netplayMenuState.renderLayout.highlightDestX;
            const int blankHeight = g_netplayMenuState.renderLayout.highlightHeight;
            const int blankWidth = g_netplayMenuState.renderLayout.highlightWidth[static_cast<size_t>(kBlankRowIndex)];

            for (int row = 0; row < kNetplayConfigOptionCount; ++row)
            {
                if (IsRenderRowUsedByMenu(g_netplayMenuState.menuId, row))
                {
                    continue;
                }

                const int dstY = g_netplayMenuState.renderLayout.highlightDestY[static_cast<size_t>(row)];
                const int dstX = g_netplayMenuState.renderLayout.highlightDestX;
                const int rowWidth = g_netplayMenuState.renderLayout.highlightWidth[static_cast<size_t>(row)];
                const int drawWidth = (rowWidth < blankWidth) ? rowWidth : blankWidth;

                (void)blit(
                    graphicsContext,
                    dstX,
                    dstY,
                    dstX + drawWidth,
                    dstY + blankHeight,
                    objectsSurface,
                    blankSrcX,
                    blankSrcY,
                    blankSrcX + drawWidth,
                    blankSrcY + blankHeight,
                    transparentColor,
                    0);
            }
        }
    }

    const int logicalSelection = ClampSelectionToCurrentMenu(static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
    const int rowIndex = GetRenderRowForSelection(logicalSelection);
    if (rowIndex >= 0 && rowIndex < static_cast<int>(g_netplayMenuState.renderLayout.highlightSourceY.size()))
    {
        const int srcY = g_netplayMenuState.renderLayout.highlightSourceY[rowIndex];
        const int dstY = g_netplayMenuState.renderLayout.highlightDestY[rowIndex];
        const int width = g_netplayMenuState.renderLayout.highlightWidth[rowIndex];
        const int srcX = g_netplayMenuState.renderLayout.highlightSourceX;
        const int dstX = g_netplayMenuState.renderLayout.highlightDestX;
        const int height = g_netplayMenuState.renderLayout.highlightHeight;
        (void)blit(
            graphicsContext,
            dstX,
            dstY,
            dstX + width,
            dstY + height,
            objectsSurface,
            srcX,
            srcY,
            srcX + width,
            srcY + height,
            transparentColor,
            0);
    }
}

BOOL RenderNetplayMenuRuntimeText(uint32_t screenContext)
{
    auto const present = reinterpret_cast<PresentFrameToScreenFn>(RuntimeAddress(kVaPresentFrameToScreen));
    DrawNetplayBaseLayer(screenContext);
    DrawRuntimeSpriteOverlay(screenContext);
    bool drewGdiOverlay = false;
    if (!g_spriteFont.loaded)
    {
        drewGdiOverlay = DrawRuntimeTextOverlayGdi(screenContext, false);
    }
    const BOOL presentResult = present(*reinterpret_cast<int*>(screenContext + kOffsetGraphicsContext));
    if (!g_spriteFont.loaded && !drewGdiOverlay)
    {
        (void)DrawRuntimeTextOverlayGdi(screenContext, true);
    }
    return presentResult;
}

BOOL RenderNetplayMenuConfigStyle(uint32_t screenContext)
{
    auto const present = reinterpret_cast<PresentFrameToScreenFn>(RuntimeAddress(kVaPresentFrameToScreen));
    DrawAnimatedCompactMenuLayer(screenContext);
    const bool drewGdiOverlay = DrawDynamicFieldValuesGdi(screenContext, false);
    const BOOL presentResult = present(*reinterpret_cast<int*>(screenContext + kOffsetGraphicsContext));
    if (!drewGdiOverlay)
    {
        (void)DrawDynamicFieldValuesGdi(screenContext, true);
    }
    return presentResult;
}

void EnterNetplayMenu(uint32_t screenContext)
{
    mod::Log("EnterNetplayMenu: request active=%d", g_netplayMenuState.active);
    if (g_netplayMenuState.active)
    {
        mod::Log("EnterNetplayMenu: already active, ignoring duplicate entry");
        return;
    }

    // Follow vanilla menu transition style: fade out current screen before asset swap.
    RunTransitionFadeOut(screenContext, 0, 0);

    if (!LoadNetplayAssets(screenContext))
    {
        mod::Log("EnterNetplayMenu: assets load failed, keeping title menu active");
        (void)LoadTitleAssets(screenContext);
        RunTransitionFadeIn(screenContext);
        return;
    }

    ResetTitleMenuState(screenContext, 0);

    // Mark netplay active before fade-in so transition rendering uses netplay renderer.
    g_netplayMenuState.active = true;
    g_netplayMenuState.bgmActive = true;
    g_netplayMenuState.menuId = NetplayMenuId::Main;
    g_netplayMenuState.mainSelection = 0;
    ResetMenuSlideTransition();
    ResetInlineEditState();
    g_lastNetplayFrameLogTick = 0;
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    SwitchToMenu(screenContext, NetplayMenuId::Main, -1);
    InstallNetplayWindowHook(screenContext);

    auto const playBackgroundMusic = reinterpret_cast<PlayBackgroundMusicFn>(RuntimeAddress(kVaPlayBackgroundMusic));
    playBackgroundMusic(GetGameSystem(screenContext), kNetplayBgmTrack);
    RunTransitionFadeIn(screenContext);
    mod::Log(
        "EnterNetplayMenu: active menu=%s selection=%d bgmTrack=%u configStyle=%d optionCount=%d backIndex=%d",
        MenuIdToString(g_netplayMenuState.menuId),
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)),
        kNetplayBgmTrack,
        g_netplayMenuState.useConfigStyleRender,
        g_netplayMenuState.optionCount,
        g_netplayMenuState.backIndex);
}

void LeaveNetplayMenu(uint32_t screenContext)
{
    mod::Log("LeaveNetplayMenu: request active=%d bgmActive=%d", g_netplayMenuState.active, g_netplayMenuState.bgmActive);
    if (!g_netplayMenuState.active)
    {
        mod::Log("LeaveNetplayMenu: already inactive");
        return;
    }

    // Mirror entry flow: fade out netplay screen before restoring title assets.
    RunTransitionFadeOut(screenContext, 0, 0);

    if (g_netplayMenuState.bgmActive)
    {
        // Mirror replay/config return behavior: stop submenu music on return to title.
        StopCurrentBgm(screenContext, "leave_netplay");
    }

    // Switch back to title render mode before fade-in to avoid mixed transition frames.
    g_netplayMenuState.active = false;
    g_netplayMenuState.bgmActive = false;
    g_netplayMenuState.useConfigStyleRender = false;
    g_netplayMenuState.menuId = NetplayMenuId::Main;
    g_netplayMenuState.mainSelection = 0;
    g_netplayMenuState.optionCount = kNetplayDefaultOptionCount;
    g_netplayMenuState.backIndex = kNetplayDefaultBackIndex;
    g_netplayMenuState.renderLayout = {};
    ResetMenuSlideTransition();
    ResetInlineEditState();
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = false;
    RemoveNetplayWindowHook();

    (void)LoadTitleAssets(screenContext);
    ResetTitleMenuState(screenContext, 4);
    RunTransitionFadeIn(screenContext);
    mod::Log(
        "LeaveNetplayMenu: returned to title assets, titleSelection=%d",
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
}

void SwitchToMenu(uint32_t screenContext, NetplayMenuId menuId, int selection)
{
    if (g_inlineEditState.active)
    {
        CancelInlineEdit();
    }
    g_netplayMenuState.menuId = menuId;
    int requestedSelection = selection;
    if (requestedSelection < 0)
    {
        requestedSelection = GetDefaultSelectionForMenu(menuId);
    }
    const int clamped = ClampSelectionToCurrentMenu(requestedSelection);
    g_netplayMenuState.optionCount = GetCurrentMenuEntryCount();
    g_netplayMenuState.backIndex = g_netplayMenuState.optionCount > 0 ? g_netplayMenuState.optionCount - 1 : 0;

    *reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection) = static_cast<int8_t>(clamped);
    *reinterpret_cast<uint16_t*>(screenContext + kOffsetMenuAnimCounter) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
    *reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter) = 0;
    g_lastLoggedSelection = static_cast<int8_t>(clamped);

    const NetplayMenuEntry* entry = GetCurrentMenuEntry(clamped);
    const int rowIndex = GetRenderRowForSelection(clamped);
    mod::Log(
        "NetplayMenuSwitch: menu=%s selection=%d count=%d row=%d(%s) entry=%s",
        MenuIdToString(menuId),
        clamped,
        g_netplayMenuState.optionCount,
        rowIndex,
        RowIndexToString(rowIndex),
        entry != nullptr ? entry->debugLabel : "none");
}

bool PromptForPort(HWND owner, const char* title, uint16_t* inOutPort)
{
    if (inOutPort == nullptr)
    {
        return false;
    }

    std::string value = std::to_string(*inOutPort);
    for (;;)
    {
        if (!ShowTextInputDialog(owner, title, "Enter port (1-65535):", &value, true))
        {
            mod::Log("PromptForPort: canceled");
            return false;
        }

        uint16_t parsedPort = 0;
        if (ParsePort(value, &parsedPort))
        {
            *inOutPort = parsedPort;
            mod::Log("PromptForPort: accepted port=%u", static_cast<unsigned>(parsedPort));
            return true;
        }

        MessageBoxA(owner, "Invalid port. Enter a number from 1 to 65535.", "Netplay", MB_OK | MB_ICONWARNING);
    }
}

bool PromptForJoinAddress(HWND owner, std::string* inOutAddress)
{
    if (inOutAddress == nullptr)
    {
        return false;
    }

    std::string value = *inOutAddress;
    for (;;)
    {
        if (!ShowTextInputDialog(owner, "Join Settings", "Enter server IP/host (Ctrl+V supported):", &value, false))
        {
            mod::Log("PromptForJoinAddress: canceled");
            return false;
        }

        if (IsValidJoinAddress(value))
        {
            *inOutAddress = value;
            mod::Log("PromptForJoinAddress: accepted address='%s'", inOutAddress->c_str());
            return true;
        }

        MessageBoxA(
            owner,
            "Invalid address. Use letters, digits, '.', '-', '_' or ':'.",
            "Netplay",
            MB_OK | MB_ICONWARNING);
    }
}

bool PromptForNickname(HWND owner, std::string* inOutNickname)
{
    if (inOutNickname == nullptr)
    {
        return false;
    }

    std::string value = *inOutNickname;
    for (;;)
    {
        if (!ShowTextInputDialog(owner, "Nickname", "Enter nickname (1-20 ASCII chars):", &value, false))
        {
            mod::Log("PromptForNickname: canceled");
            return false;
        }

        if (IsValidNickname(value))
        {
            *inOutNickname = value;
            mod::Log("PromptForNickname: accepted nickname='%s'", inOutNickname->c_str());
            return true;
        }

        MessageBoxA(owner, "Invalid nickname. Use 1-20 visible ASCII characters.", "Netplay", MB_OK | MB_ICONWARNING);
    }
}

void ShowStubActionMessage(HWND owner, const std::string& message)
{
    MessageBoxA(owner, message.c_str(), "Netplay", MB_OK | MB_ICONINFORMATION);
}

void ExecuteNetplayAction(uint32_t screenContext, NetplayMenuAction action, int logicalSelection)
{
    const HWND owner = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
    const int selectedRow = GetRenderRowForSelection(logicalSelection);
    mod::Log(
        "NetplayAction: menu=%s selection=%d row=%d(%s) action=%s",
        MenuIdToString(g_netplayMenuState.menuId),
        logicalSelection,
        selectedRow,
        RowIndexToString(selectedRow),
        MenuActionToString(action));

    switch (action)
    {
    case NetplayMenuAction::OpenHost:
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Host, -1, +1);
        break;
    case NetplayMenuAction::OpenJoin:
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Join, -1, +1);
        break;
    case NetplayMenuAction::OpenNickname:
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Nickname, -1, +1);
        break;
    case NetplayMenuAction::BackToMain:
        StartMenuSlideTransition(screenContext, NetplayMenuId::Main, g_netplayMenuState.mainSelection, -1);
        break;
    case NetplayMenuAction::LeaveNetplay:
        LeaveNetplayMenu(screenContext);
        break;
    case NetplayMenuAction::HostEditPort:
        BeginInlineEdit(NetplayMenuAction::HostEditPort);
        break;
    case NetplayMenuAction::JoinEditAddress:
        BeginInlineEdit(NetplayMenuAction::JoinEditAddress);
        break;
    case NetplayMenuAction::JoinEditPort:
        BeginInlineEdit(NetplayMenuAction::JoinEditPort);
        break;
    case NetplayMenuAction::NicknameEdit:
        BeginInlineEdit(NetplayMenuAction::NicknameEdit);
        break;
    case NetplayMenuAction::HostStart:
    {
        char text[256] = {};
        snprintf(
            text,
            sizeof(text),
            "Host flow is still in progress.\n\nNickname: %s\nPort: %u",
            g_netplayMenuState.nickname.c_str(),
            static_cast<unsigned>(g_netplayMenuState.hostPort));
        ShowStubActionMessage(owner, text);
        break;
    }
    case NetplayMenuAction::JoinConnect:
    {
        char text[320] = {};
        snprintf(
            text,
            sizeof(text),
            "Join flow is still in progress.\n\nNickname: %s\nAddress: %s\nPort: %u",
            g_netplayMenuState.nickname.c_str(),
            g_netplayMenuState.joinAddress.c_str(),
            static_cast<unsigned>(g_netplayMenuState.joinPort));
        ShowStubActionMessage(owner, text);
        break;
    }
    default:
        break;
    }
}

char UpdateNetplayMenu(uint32_t screenContext)
{
    ++g_netplayUpdateCallCount;
    auto const render = GetOriginalTitleRender();
    auto const processInput = reinterpret_cast<ProcessPlayerInputFn>(RuntimeAddress(kVaProcessPlayerInput));

    // Do not call vanilla title exit-input logic in netplay mode:
    // it processes raw ESC and can terminate the game.
    AdvanceMenuSlideTransition(screenContext);
    if (g_useRuntimeTextOverlay)
    {
        (void)RenderNetplayMenuRuntimeText(screenContext);
    }
    else if (g_netplayMenuState.useConfigStyleRender)
    {
        (void)RenderNetplayMenuConfigStyle(screenContext);
    }
    else
    {
        (void)render(screenContext);
    }

    const int gameSystem = GetGameSystem(screenContext);
    processInput(reinterpret_cast<int*>(gameSystem));
    auto* const inputBytes = reinterpret_cast<uint8_t*>(gameSystem);
    InputSnapshot currentSnapshot = {
        static_cast<int8_t>(inputBytes[12]),
        static_cast<int8_t>(inputBytes[14]),
        inputBytes[16],
        inputBytes[18],
        static_cast<int8_t>(inputBytes[13]),
        static_cast<int8_t>(inputBytes[15]),
        inputBytes[17],
        inputBytes[19],
    };

    auto* const selectionPtr = reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection);
    auto* const inactivityCounter = reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter);

    if (!g_hasLoggedInputSnapshot
        || std::memcmp(&currentSnapshot, &g_lastInputSnapshot, sizeof(InputSnapshot)) != 0)
    {
        mod::Log(
            "NetplayInput: P1(h=%d v=%d c=%u b=%u) P2(h=%d v=%d c=%u b=%u)",
            static_cast<int>(currentSnapshot.p1Horizontal),
            static_cast<int>(currentSnapshot.p1Vertical),
            static_cast<unsigned>(currentSnapshot.p1Confirm),
            static_cast<unsigned>(currentSnapshot.p1Cancel),
            static_cast<int>(currentSnapshot.p2Horizontal),
            static_cast<int>(currentSnapshot.p2Vertical),
            static_cast<unsigned>(currentSnapshot.p2Confirm),
            static_cast<unsigned>(currentSnapshot.p2Cancel));
        g_lastInputSnapshot = currentSnapshot;
        g_hasLoggedInputSnapshot = true;
    }

    const DWORD nowTick = GetTickCount();
    if (g_lastNetplayFrameLogTick == 0 || nowTick - g_lastNetplayFrameLogTick >= kNetplayFrameLogIntervalMs)
    {
        const int currentRow = GetRenderRowForSelection(static_cast<int>(*selectionPtr));
        const int nativeSlideY = *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY);
        mod::Log(
            "NetplayFrame: updates=%llu menu=%s selection=%d row=%d(%s) inactivity=%u slide=%d nativeSlideY=%d",
            static_cast<unsigned long long>(g_netplayUpdateCallCount),
            MenuIdToString(g_netplayMenuState.menuId),
            static_cast<int>(*selectionPtr),
            currentRow,
            RowIndexToString(currentRow),
            *inactivityCounter,
            IsMenuSlideTransitionActive() ? 1 : 0,
            nativeSlideY);
        g_lastNetplayFrameLogTick = nowTick;
    }

    if (IsMenuSlideTransitionActive())
    {
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    const int entryCount = GetCurrentMenuEntryCount();
    if (entryCount <= 0)
    {
        SwitchToMenu(screenContext, NetplayMenuId::Main, -1);
        return 0;
    }

    if (HandleInlineEditInput(screenContext, inputBytes))
    {
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        *inactivityCounter = 0;
        return 0;
    }

    if (ConsumeNetplayEscapeEdge())
    {
        PlayUiSound(screenContext, kSfxConfirm);
        mod::Log(
            "NetplayCancel: keyboard=ESC menu=%s selection=%d",
            MenuIdToString(g_netplayMenuState.menuId),
            static_cast<int>(*selectionPtr));

        if (g_netplayMenuState.menuId == NetplayMenuId::Main)
        {
            LeaveNetplayMenu(screenContext);
        }
        else
        {
            StartMenuSlideTransition(screenContext, NetplayMenuId::Main, g_netplayMenuState.mainSelection, -1);
        }
        return 0;
    }

    bool hadDirectionalInput = false;

    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch = reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1 + playerIndex);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);

        if (vertical != 0)
        {
            hadDirectionalInput = true;
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                PlayUiSound(screenContext, kSfxMove);
                const int current = ClampSelectionToCurrentMenu(static_cast<int>(*selectionPtr));
                const int delta = vertical > 0 ? 1 : -1;
                int next = (current + delta + entryCount) % entryCount;

                const NetplayMenuEntry* nextEntry = GetCurrentMenuEntry(next);
                mod::Log(
                    "NetplaySelection: player=%d menu=%s from=%d to=%d row=%d(%s) label=%s inputV=%d",
                    playerIndex,
                    MenuIdToString(g_netplayMenuState.menuId),
                    current,
                    next,
                    nextEntry != nullptr ? nextEntry->renderRow : -1,
                    nextEntry != nullptr ? RowIndexToString(nextEntry->renderRow) : "ROW_UNKNOWN",
                    nextEntry != nullptr ? nextEntry->debugLabel : "none",
                    static_cast<int>(vertical));
                *selectionPtr = static_cast<int8_t>(next);
                *reinterpret_cast<uint16_t*>(screenContext + kOffsetMenuAnimCounter) = 0;
                *inputLatch = 1;
                g_lastLoggedSelection = *selectionPtr;
            }
        }
        else
        {
            *inputLatch = 0;
        }

        if (inputBytes[playerIndex + 16] == 1)
        {
            const int logicalSelection = ClampSelectionToCurrentMenu(static_cast<int>(*selectionPtr));
            const NetplayMenuEntry* selectedEntry = GetCurrentMenuEntry(logicalSelection);
            if (selectedEntry == nullptr)
            {
                return 0;
            }

            PlayUiSound(screenContext, kSfxConfirm);
            mod::Log(
                "NetplayConfirm: player=%d menu=%s selection=%d row=%d(%s) label=%s action=%s",
                playerIndex,
                MenuIdToString(g_netplayMenuState.menuId),
                logicalSelection,
                selectedEntry->renderRow,
                RowIndexToString(selectedEntry->renderRow),
                selectedEntry->debugLabel,
                MenuActionToString(selectedEntry->action));
            ExecuteNetplayAction(screenContext, selectedEntry->action, logicalSelection);
            return 0;
        }

        if (inputBytes[playerIndex + 18] == 1)
        {
            PlayUiSound(screenContext, kSfxConfirm);
            mod::Log(
                "NetplayCancel: player=%d menu=%s selection=%d",
                playerIndex,
                MenuIdToString(g_netplayMenuState.menuId),
                static_cast<int>(*selectionPtr));

            if (g_netplayMenuState.menuId == NetplayMenuId::Main)
            {
                LeaveNetplayMenu(screenContext);
            }
            else
            {
                // Match replay/config-style back behavior: use submenu back transition.
                StartMenuSlideTransition(screenContext, NetplayMenuId::Main, g_netplayMenuState.mainSelection, -1);
            }
            return 0;
        }
    }

    if (!hadDirectionalInput)
    {
        ++(*inactivityCounter);
    }

    return 0;
}

void TriggerNetplayMenuEntry(uint32_t screenContext)
{
    mod::Log(
        "TriggerNetplayMenuEntry: titleSelection=%d screenContext=0x%08X",
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)),
        screenContext);
    PlayUiSound(screenContext, kSfxConfirm);
    EnterNetplayMenu(screenContext);
}

extern "C" char __cdecl HookedTitleUpdateImpl(uint32_t screenContext)
{
    ++g_titleUpdateCallCount;
    if ((g_titleUpdateCallCount % 300ull) == 0ull)
    {
        mod::Log(
            "HookedTitleUpdateImpl: calls=%llu netplayActive=%d",
            static_cast<unsigned long long>(g_titleUpdateCallCount),
            g_netplayMenuState.active);
    }

    if (!g_netplayMenuState.active)
    {
        return GetOriginalTitleUpdate()(screenContext);
    }
    return UpdateNetplayMenu(screenContext);
}

extern "C" BOOL __cdecl HookedTitleRenderImpl(uint32_t screenContext)
{
    if (g_netplayMenuState.active && g_useRuntimeTextOverlay)
    {
        return RenderNetplayMenuRuntimeText(screenContext);
    }
    if (g_netplayMenuState.active && g_netplayMenuState.useConfigStyleRender)
    {
        return RenderNetplayMenuConfigStyle(screenContext);
    }
    return GetOriginalTitleRender()(screenContext);
}

#if defined(_M_IX86)
extern "C" void __cdecl NetplayCaseImpl(uint32_t screenContext)
{
    mod::Log("NetplayCaseImpl: invoked");
    TriggerNetplayMenuEntry(screenContext);
}

extern "C" __declspec(naked) void NetplayCaseThunk()
{
    __asm
    {
        mov eax, dword ptr [ebp-8]
        push eax
        call NetplayCaseImpl
        add esp, 4
        mov al, 0
        mov edx, dword ptr [g_titleCaseReturnAddress]
        jmp edx
    }
}

extern "C" __declspec(naked) void HookedTitleUpdateThunk()
{
    __asm
    {
        push ecx
        call HookedTitleUpdateImpl
        add esp, 4
        ret
    }
}

extern "C" __declspec(naked) void HookedTitleRenderThunk()
{
    __asm
    {
        push ecx
        call HookedTitleRenderImpl
        add esp, 4
        ret
    }
}
#endif
}

namespace netplay
{
bool InstallHooks()
{
    std::lock_guard<std::mutex> lock(g_patchMutex);

    if (g_hooksInstalled.load())
    {
        mod::Log("InstallHooks: already installed");
        return true;
    }

    if (sizeof(void*) != 4)
    {
        mod::Log("InstallHooks: this mod requires x86 process");
        return false;
    }

#if !defined(_M_IX86)
    mod::Log("InstallHooks: this build target is not x86");
    return false;
#else
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (g_exeBase == 0)
    {
        mod::Log("InstallHooks: failed to resolve exe base");
        return false;
    }

    g_moduleDirectory = BuildModuleDirectory(ResolveCurrentModule());
    mod::Log("InstallHooks: module directory '%s'", g_moduleDirectory.c_str());

    if (!ValidateMenuSpecs())
    {
        mod::Log("InstallHooks: menu spec validation failed");
        return false;
    }

    g_titleCaseReturnAddress = static_cast<uint32_t>(RuntimeAddress(kVaTitleCaseEpilogue));

    g_customDispatchTable[0] = static_cast<uint32_t>(RuntimeAddress(kVaCaseArcade));
    g_customDispatchTable[1] = static_cast<uint32_t>(RuntimeAddress(kVaCaseVsCpu));
    g_customDispatchTable[2] = static_cast<uint32_t>(RuntimeAddress(kVaCaseVsHuman));
    g_customDispatchTable[3] = static_cast<uint32_t>(RuntimeAddress(kVaCasePractice));
    g_customDispatchTable[4] = reinterpret_cast<uint32_t>(&NetplayCaseThunk);
    g_customDispatchTable[5] = static_cast<uint32_t>(RuntimeAddress(kVaCaseReplay));
    g_customDispatchTable[6] = static_cast<uint32_t>(RuntimeAddress(kVaCaseOptions));
    g_customDispatchTable[7] = static_cast<uint32_t>(RuntimeAddress(kVaCaseExit));

    auto dwordToBytes = [](uint32_t value) -> std::vector<uint8_t>
    {
        return {
            static_cast<uint8_t>(value & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 24) & 0xFF),
        };
    };

    const uintptr_t addrWrapCmpMax = RuntimeAddress(kVaWrapCmpMax);
    const uintptr_t addrWrapClampNegative = RuntimeAddress(kVaWrapClampNegative);
    const uintptr_t addrSwitchCmpMax = RuntimeAddress(kVaSwitchCmpMax);
    const uintptr_t addrSwitchTableDisp = RuntimeAddress(kVaSwitchTableDisp);
    const uintptr_t addrRenderPanelDestH = RuntimeAddress(kVaRenderPanelDestH);
    const uintptr_t addrRenderPanelSourceH = RuntimeAddress(kVaRenderPanelSourceH);
    const uintptr_t addrRenderPanelDestTop = RuntimeAddress(kVaRenderPanelDestTop);
    const uintptr_t addrRenderHighlightDestBase = RuntimeAddress(kVaRenderHighlightDestBase);
    const uintptr_t addrRenderHighlightScreenBase = RuntimeAddress(kVaRenderHighlightScreenBase);
    const uintptr_t addrTitleVtableRender = RuntimeAddress(kVaTitleVtableRender);
    const uintptr_t addrTitleVtableUpdate = RuntimeAddress(kVaTitleVtableUpdate);

    bool ok = true;
    ok = ok && ApplyPatch(addrWrapCmpMax, {0x83, 0xF8, 0x06}, {0x83, 0xF8, 0x07}, "wrap compare 6->7");
    ok = ok && ApplyPatch(
        addrWrapClampNegative,
        {0x8B, 0x4D, 0xF8, 0xC6, 0x81, 0x3C, 0x04, 0x00, 0x00, 0x06},
        {0x8B, 0x4D, 0xF8, 0xC6, 0x81, 0x3C, 0x04, 0x00, 0x00, 0x07},
        "wrap clamp 6->7");
    ok = ok && ApplyPatch(addrSwitchCmpMax, {0x83, 0x7D, 0xF4, 0x06}, {0x83, 0x7D, 0xF4, 0x07}, "switch compare 6->7");
    ok = ok && ApplyPatch(addrRenderPanelDestH, {0x6A, 0x62}, {0x6A, 0x70}, "panel dest height 98->112");
    ok = ok && ApplyPatch(addrRenderPanelSourceH, {0x6A, 0x62}, {0x6A, 0x70}, "panel src height 98->112");
    ok = ok && ApplyPatch(
        addrRenderPanelDestTop,
        {0x68, 0x82, 0x00, 0x00, 0x00},
        {0x68,
         static_cast<uint8_t>(kMenuDestTopPatched & 0xFF),
         static_cast<uint8_t>((kMenuDestTopPatched >> 8) & 0xFF),
         static_cast<uint8_t>((kMenuDestTopPatched >> 16) & 0xFF),
         static_cast<uint8_t>((kMenuDestTopPatched >> 24) & 0xFF)},
        "panel dest top 130->123");
    ok = ok && ApplyPatch(
        addrRenderHighlightDestBase,
        {0x83, 0xC0, 0x62},
        {0x83, 0xC0, 0x70},
        "highlight src base 98->112");
    ok = ok && ApplyPatch(
        addrRenderHighlightScreenBase,
        {0x81, 0xC2, 0x82, 0x00, 0x00, 0x00},
        {0x81, 0xC2,
         static_cast<uint8_t>(kMenuDestTopPatched & 0xFF),
         static_cast<uint8_t>((kMenuDestTopPatched >> 8) & 0xFF),
         static_cast<uint8_t>((kMenuDestTopPatched >> 16) & 0xFF),
         static_cast<uint8_t>((kMenuDestTopPatched >> 24) & 0xFF)},
        "highlight screen base 130->123");

    const uint32_t dispatchAddress = reinterpret_cast<uint32_t>(&g_customDispatchTable[0]);
    ok = ok && ApplyPatch(
        addrSwitchTableDisp,
        {0x87, 0x64, 0x77, 0x00},
        dwordToBytes(dispatchAddress),
        "switch dispatch table -> custom");
    ok = ok && ApplyPatch(
        addrTitleVtableRender,
        dwordToBytes(static_cast<uint32_t>(RuntimeAddress(kVaTitleRender))),
        dwordToBytes(reinterpret_cast<uint32_t>(&HookedTitleRenderThunk)),
        "title vtable render -> hook");
    ok = ok && ApplyPatch(
        addrTitleVtableUpdate,
        dwordToBytes(static_cast<uint32_t>(RuntimeAddress(kVaUpdateTitleScreenLogic))),
        dwordToBytes(reinterpret_cast<uint32_t>(&HookedTitleUpdateThunk)),
        "title vtable update -> hook");

    if (!ok)
    {
        RestorePatches();
        mod::Log("InstallHooks: patch application failed");
        return false;
    }

    g_titleUpdateCallCount = 0;
    g_netplayUpdateCallCount = 0;
    g_hooksInstalled.store(true);
    mod::Log(
        "InstallHooks: success (title update=0x%08X custom table=0x%08X)",
        static_cast<unsigned>(RuntimeAddress(kVaUpdateTitleScreenLogic)),
        dispatchAddress);
    return true;
#endif
}

void RemoveHooks()
{
    std::lock_guard<std::mutex> lock(g_patchMutex);

    if (!g_hooksInstalled.load())
    {
        return;
    }

    g_netplayMenuState = {};
    g_spriteFont = {};
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = false;
    RemoveNetplayWindowHook();
    RestorePatches();
    g_hooksInstalled.store(false);
    mod::Log("RemoveHooks: restored original bytes");
}

bool AreHooksInstalled()
{
    return g_hooksInstalled.load();
}

void ShowInProgressMessage(HWND owner)
{
    mod::Log("ShowInProgressMessage: owner=0x%p", owner);
    MessageBoxA(owner, "In progress", "Netplay", MB_OK | MB_ICONINFORMATION);
}
}
