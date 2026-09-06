#include "netplay/hooks/debug_overlay.h"

#include "netplay/bridge/async_hosting.h"
#include "netplay/core/battle_log_menu.h"
#include "netplay/core/mod_settings.h"
#include "netplay/hooks/menu_query.h"
#include "netplay_resource_ids.h"
#include "logger.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Declared by the Win32 backend; we call it from our chained wndproc.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace netplay::debug_overlay
{
namespace
{
bool g_inited = false;
IDirect3DDevice9* g_device = nullptr;
HWND g_hwnd = nullptr;
WNDPROC g_prevWndProc = nullptr;
bool g_panelOpen = false;

// Crisp badge font, loaded from a system TTF at a fixed native pixel size so the
// text is not a scaled-up bitmap (the ImGui default font blurs when scaled).
// nullptr -> fall back to the default font.
ImFont* g_badgeFont = nullptr;
constexpr float kBadgeFontPx = 22.0f;

// Per-profile RT text styles.  fontPx is baked into the atlas (changing it
// needs a font rebuild - the debug panel's Apply button); cellRtPx is the RT
// height of the logical cell the text is centered on (a 5x7 row cell is
// 14 RT px); yBiasRtPx nudges the line up/down after centering.  Dial values
// in with the backslash panel (Apply logs them in paste-ready form), then
// hardcode them in the per-face defaults below.
struct RtProfileStyle
{
    const char* name;
    float fontPx;
    float cellRtPx;
    float yBiasRtPx;
};

// Every face renders at different metrics, so each gets its own tuned
// defaults; faces without a tuned set use the base set until they get one.
using RtProfileDefaults = RtProfileStyle[static_cast<size_t>(RtTextProfile::Count)];
// Base set - tuned in-game 2026-07-08 with the original face (Segoe UI).
constexpr RtProfileDefaults kRtProfileDefaultsBase = {
    {"MenuHeader",      22.0f, 14.0f, 0.0f},
    {"MenuRow",         15.0f, 14.0f, 0.0f},
    {"Footer",          22.0f, 17.0f, 0.0f},
    {"BattleLogHeader", 26.0f, 14.0f, 0.0f},
    {"BattleLogRow",    21.0f,  8.0f, 0.0f},
    {"MenuSection",     13.0f, 14.0f, 0.0f},
    {"OverlayTitle",    26.0f, 20.0f, 0.0f},
    {"OverlayBody",     21.0f, 14.0f, 0.0f},
    {"OverlayHint",      23.0f, 18.0f, 0.0f},
};
// Tuned in-game 2026-07-08 on Yu Gothic (MenuHeader/MenuRow still base
// values); the bundled Noto Sans JP shares it as the closest approximation.
constexpr RtProfileDefaults kRtProfileDefaultsJpGothic = {
    {"MenuHeader",      22.0f, 14.0f, 0.0f},
    {"MenuRow",         17.0f, 17.0f, 0.0f},
    {"Footer",          20.0f, 17.0f, 0.0f},
    {"BattleLogHeader", 24.0f, 19.0f, 0.0f},
    {"BattleLogRow",    18.0f, 13.0f, 0.0f},
    {"MenuSection",     13.0f, 14.0f, 0.0f},
    {"OverlayTitle",    24.0f, 17.0f, 0.0f},
    {"OverlayBody",     18.0f, 14.0f, 0.0f},
    {"OverlayHint",      19.0f, 21.0f, -0.2f},
};
// Tuned in-game 2026-07-08 on MS Gothic.
constexpr RtProfileDefaults kRtProfileDefaultsMsGothic = {
    {"MenuHeader",      22.0f, 14.0f, 0.0f},
    {"MenuRow",         15.0f, 14.0f, 0.0f},
    {"Footer",          19.0f, 16.0f, 0.0f},
    {"BattleLogHeader", 25.0f, 16.0f, 0.0f},
    {"BattleLogRow",    18.0f, 12.0f, 0.0f},
    {"MenuSection",     13.0f, 14.0f, 0.0f},
    {"OverlayTitle",    26.0f, 16.0f, 0.0f},
    {"OverlayBody",     18.0f, 14.0f, 0.0f},
    {"OverlayHint",      18.0f, 18.0f, 0.0f},
};
// Tuned in-game 2026-07-09 on Meiryo (its own set now, was sharing base).
constexpr RtProfileDefaults kRtProfileDefaultsMeiryo = {
    {"MenuHeader",      22.0f, 14.0f, 0.0f},
    {"MenuRow",         20.0f, 13.0f, 0.0f},
    {"Footer",          22.0f, 17.0f, 0.0f},
    {"BattleLogHeader", 26.0f, 14.0f, 0.0f},
    {"BattleLogRow",    20.0f, 12.0f, 0.0f},
    {"MenuSection",     13.0f, 14.0f, 0.0f},
    {"OverlayTitle",    30.0f, 20.0f, 0.0f},
    {"OverlayBody",     24.0f, 14.0f, 0.0f},
    {"OverlayHint",      24.0f, 21.0f, 0.0f},
};
// Tuned in-game 2026-07-09 on ITC Bolt (Latin display face; smaller body px).
constexpr RtProfileDefaults kRtProfileDefaultsItcBolt = {
    {"MenuHeader",      22.0f, 14.0f, 0.0f},
    {"MenuRow",         13.0f, 16.0f, 0.0f},
    {"Footer",          13.0f, 17.0f, 0.0f},
    {"BattleLogHeader", 24.0f, 18.0f, 0.0f},
    {"BattleLogRow",    16.0f, 11.0f, 0.0f},
    {"MenuSection",     15.0f, 16.0f, 0.0f},
    {"OverlayTitle",    23.0f, 18.0f, 0.0f},
    {"OverlayBody",     15.0f, 16.0f, 0.0f},
    {"OverlayHint",      18.0f, 18.0f, 0.0f},
};
// Tuned in-game 2026-07-24 on Arial.
constexpr RtProfileDefaults kRtProfileDefaultsArial = {
    {"MenuHeader",      22.0f, 14.0f, 0.0f},
    {"MenuRow",         15.0f, 14.0f, 0.0f},
    {"Footer",          22.0f, 17.0f, 0.0f},
    {"BattleLogHeader", 26.0f, 14.0f, 0.0f},
    {"BattleLogRow",    21.0f,  8.0f, 0.0f},
    {"MenuSection",     13.0f, 14.0f, 0.0f},
    {"OverlayTitle",    23.0f, 18.0f, 0.0f},
    {"OverlayBody",     16.0f, 14.0f, 0.0f},
    {"OverlayHint",      18.0f, 14.0f, 0.0f},
};

// Active working set: seeded from the resolved face's defaults whenever the
// face CHANGES (slider experiments survive same-face rebuilds).
RtProfileStyle g_rtProfiles[static_cast<size_t>(RtTextProfile::Count)] = {
    {"MenuHeader",      22.0f, 14.0f, 0.0f},
    {"MenuRow",         15.0f, 14.0f, 0.0f},
    {"Footer",          22.0f, 17.0f, 0.0f},
    {"BattleLogHeader", 26.0f, 14.0f, 0.0f},
    {"BattleLogRow",    21.0f,  8.0f, 0.0f},
    {"MenuSection",     13.0f, 14.0f, 0.0f},
    {"OverlayTitle",    26.0f, 20.0f, 0.0f},
    {"OverlayBody",     21.0f, 14.0f, 0.0f},
    {"OverlayHint",      23.0f, 18.0f, 0.0f},
};
char g_rtAppliedFaceLabel[64] = {};
ImFont* g_rtProfileFonts[static_cast<size_t>(RtTextProfile::Count)] = {};
char g_rtJpFontPath[MAX_PATH] = {};
volatile LONG g_rtFontRebuildRequested = 0;
// True when the atlas covers Cyrillic (from the primary face) AND a Japanese
// font was merged - i.e. non-ASCII nicknames will render instead of '?'.
bool g_rtExtendedGlyphs = false;

const RtProfileStyle& RtStyleFor(RtTextProfile profile)
{
    size_t index = static_cast<size_t>(profile);
    if (index >= static_cast<size_t>(RtTextProfile::Count))
    {
        index = static_cast<size_t>(RtTextProfile::MenuRow);
    }
    return g_rtProfiles[index];
}

// Game-RT text overlay items (see debug_overlay.h). Producers stage a full
// frame from the menu render pass, then Commit publishes it for EndScene.
// Guarded by a critical section: the menu render and EndScene are expected on
// the same thread, but the lock keeps the swap safe if that ever changes.
CRITICAL_SECTION g_rtTextLock;
bool g_rtTextLockInited = false;
std::vector<RtTextItem> g_rtTextStaging;
std::vector<RtTextItem> g_rtTextActive;
volatile LONG g_rtTextPublished = 0;

void EnsureRtTextLock()
{
    if (!g_rtTextLockInited)
    {
        InitializeCriticalSection(&g_rtTextLock);
        g_rtTextLockInited = true;
    }
}

ImFont* RtFontFor(RtTextProfile profile)
{
    size_t index = static_cast<size_t>(profile);
    if (index >= static_cast<size_t>(RtTextProfile::Count))
    {
        index = static_cast<size_t>(RtTextProfile::MenuRow);
    }
    ImFont* font = g_rtProfileFonts[index];
    return (font != nullptr) ? font : g_badgeFont;
}

float RtFontPxFor(RtTextProfile profile)
{
    return RtStyleFor(profile).fontPx;
}

// EFZ renders its scene to a fixed 640x480 D3D9 render target; EFZ Revival then
// composites/upscales that to the window backbuffer. Our overlay must draw on the
// 640x480 GAME surface (so it scales with the game) - NOT on Revival's window
// backbuffer. EndScene fires for both; we filter by render-target size. (This is
// exactly how efz-training-mode picks the correct surface.)
constexpr int kGameRtW = 640;
constexpr int kGameRtH = 480;

// Backbuffer dimensions last seen - used to detect a device RESET (window
// resize / borderless-fullscreen toggle), which invalidates ImGui's
// D3DPOOL_DEFAULT resources and makes the overlay vanish until rebuilt.
int g_lastBackBufferW = 0;
int g_lastBackBufferH = 0;

// Tunable async-host in-battle indicator parameters. Adjust live in the panel,
// then hardcode the chosen defaults here.
float g_asyncPosX = 0.5f;     // fraction of screen width  (0=left, 1=right)
float g_asyncPosY = 0.0f;     // fraction of screen height (0=top, 1=bottom)
float g_asyncFontScale = 0.65f;
float g_asyncBgAlpha = 0.48f; // tuned live via the backslash debug panel

// Shared position adjustment for the transient netplay panels. Values are in
// the 320x240 menu-logical coordinate space and apply only to their TTF text;
// panel geometry and the 5x7 fallback remain unchanged.
float g_transientTextOffsetX = 0.0f;
float g_transientTextOffsetY = 0.0f;

// stb_truetype (ImGui's built-in rasterizer) only understands TrueType
// outlines: plain TTF (version 1.0 / 'true') and TTC collections.  CFF-based
// OpenType files ('OTTO' magic, e.g. many .otf fonts) would make the whole
// atlas build fail, taking every overlay down with it - reject them up front.
bool IsStbLoadableFontFile(const char* path)
{
    HANDLE file = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    uint8_t magic[4] = {};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, magic, sizeof(magic), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != sizeof(magic))
    {
        return false;
    }
    const bool ttf = magic[0] == 0x00 && magic[1] == 0x01 && magic[2] == 0x00 && magic[3] == 0x00;
    const bool ttcf = std::memcmp(magic, "ttcf", 4) == 0;
    const bool appleTrue = std::memcmp(magic, "true", 4) == 0;
    if (std::memcmp(magic, "OTTO", 4) == 0)
    {
        mod::Log(
            "DebugOverlay: font '%s' uses CFF outlines (OTTO) - unsupported by the "
            "built-in rasterizer, convert it to TTF outlines to use it",
            path);
        return false;
    }
    return ttf || ttcf || appleTrue;
}

std::string OverlayModuleDirectory()
{
    char modulePath[MAX_PATH] = {};
    HMODULE selfModule = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&OverlayModuleDirectory),
        &selfModule);
    if (selfModule == nullptr || GetModuleFileNameA(selfModule, modulePath, MAX_PATH) == 0)
    {
        return {};
    }
    std::string dir(modulePath);
    const size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos)
    {
        return {};
    }
    dir.resize(slash);
    return dir;
}

// Selectable font faces.  jpCapable faces natively cover Latin + Cyrillic +
// Japanese, so they need no fallback merging.  fallbackEligible faces may be
// picked automatically when the configured face is unavailable; fonts a user
// must opt into (display faces) keep it false.  Each face may resolve from
// the system Fonts directory and/or a bundled mod asset (system preferred:
// e.g. Meiryo uses the OS meiryo.ttc when installed, else the bundled TTF).
// The DLL additionally embeds Noto Sans CJK JP as the last-resort fallback,
// so full coverage exists even when the assets folder is missing entirely.
struct RtFontFace
{
    const char* label;
    const char* systemFile;  // in %WINDIR%\Fonts, nullptr if none
    const char* assetFile;   // in <mod>\assets\fonts, nullptr if none
    bool embeddedCopy;       // the DLL resource carries this face
    bool jpCapable;
    bool fallbackEligible;
    const RtProfileStyle* profileDefaults;
};
constexpr RtFontFace kRtFontFaces[] = {
    {"Yu Gothic",      "YuGothM.ttc",  "yugothib.ttf",                  false, true,  true,  kRtProfileDefaultsJpGothic},
    {"Meiryo",         "meiryo.ttc",   "Meiryo.ttf",                    false, true,  true,  kRtProfileDefaultsMeiryo},
    {"MS Gothic",      "msgothic.ttc", nullptr,                         false, true,  true,  kRtProfileDefaultsMsGothic},
    {"Noto Sans JP",   nullptr,        "NotoSansCJKjp-Regular.ttf",     true,  true,  true,  kRtProfileDefaultsJpGothic},
    {"Noto Sans Mono", nullptr,        "NotoSansMonoCJKjp-Regular.ttf", false, true,  true,  kRtProfileDefaultsBase},
    {"Segoe UI",       "segoeui.ttf",  nullptr,                         false, false, true,  kRtProfileDefaultsBase},
    {"Arial",          "arial.ttf",    nullptr,                         false, false, true,  kRtProfileDefaultsArial},
    {"ITC Bolt",       nullptr,        "ITC Bolt\\ITC Bolt.ttf",        false, false, false, kRtProfileDefaultsItcBolt},
};

bool ResolveFacePath(const RtFontFace& face, char* outPath, size_t outSize)
{
    if (face.systemFile != nullptr)
    {
        char winDir[MAX_PATH] = {};
        const UINT n = GetWindowsDirectoryA(winDir, MAX_PATH);
        if (n != 0 && n < MAX_PATH)
        {
            std::snprintf(outPath, outSize, "%s\\Fonts\\%s", winDir, face.systemFile);
            if (GetFileAttributesA(outPath) != INVALID_FILE_ATTRIBUTES
                && IsStbLoadableFontFile(outPath))
            {
                return true;
            }
        }
    }
    if (face.assetFile != nullptr)
    {
        const std::string dir = OverlayModuleDirectory();
        if (!dir.empty())
        {
            std::snprintf(outPath, outSize, "%s\\assets\\fonts\\%s", dir.c_str(), face.assetFile);
            if (GetFileAttributesA(outPath) != INVALID_FILE_ATTRIBUTES
                && IsStbLoadableFontFile(outPath))
            {
                return true;
            }
        }
    }
    return false;
}

// The embedded last-resort font (see src/netplay_mod.rc).  Resource memory
// stays mapped for the module's lifetime, so ImGui can reference it without
// copying (FontDataOwnedByAtlas = false).
bool GetEmbeddedFallbackFont(void** outData, int* outSize)
{
    *outData = nullptr;
    *outSize = 0;
    HMODULE selfModule = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetEmbeddedFallbackFont),
        &selfModule);
    if (selfModule == nullptr)
    {
        return false;
    }
    HRSRC resource = FindResourceA(
        selfModule, MAKEINTRESOURCEA(IDR_FONT_FALLBACK), MAKEINTRESOURCEA(10) /*RT_RCDATA*/);
    if (resource == nullptr)
    {
        return false;
    }
    HGLOBAL handle = LoadResource(selfModule, resource);
    const DWORD size = SizeofResource(selfModule, resource);
    if (handle == nullptr || size == 0)
    {
        return false;
    }
    void* data = LockResource(handle);
    if (data == nullptr)
    {
        return false;
    }
    *outData = data;
    *outSize = static_cast<int>(size);
    return true;
}

// Resolve the primary face: the configured one if present and loadable,
// otherwise walk the table in order.  An empty outPath with a non-null
// return means "load this face from the embedded DLL resource".  Returns
// nullptr only when nothing at all is loadable.
const RtFontFace* ResolveFontByLabel(
    const char* configured, char* outPath, size_t outSize, bool hasEmbedded)
{
    for (const RtFontFace& face : kRtFontFaces)
    {
        if (_stricmp(face.label, configured) == 0)
        {
            if (ResolveFacePath(face, outPath, outSize))
            {
                return &face;
            }
            if (face.embeddedCopy && hasEmbedded)
            {
                outPath[0] = '\0';
                return &face;
            }
            mod::Log(
                "DebugOverlay: configured font '%s' unavailable, falling back",
                configured);
            break;
        }
    }
    for (const RtFontFace& face : kRtFontFaces)
    {
        if (!face.fallbackEligible)
        {
            continue;
        }
        if (ResolveFacePath(face, outPath, outSize))
        {
            return &face;
        }
        if (face.embeddedCopy && hasEmbedded)
        {
            outPath[0] = '\0';
            return &face;
        }
    }
    return nullptr;
}

const RtFontFace* ResolvePrimaryFont(char* outPath, size_t outSize, bool hasEmbedded)
{
    return ResolveFontByLabel(
        netplay::mod_settings::MenuTtfFontFace().c_str(), outPath, outSize, hasEmbedded);
}

// Japanese/Cyrillic fill-in source for primaries that lack those glyphs.
// System fonts first, the bundled Noto TTF as the last resort (covers Wine,
// where the Japanese system fonts usually do not exist).
const char* ResolveJapaneseFontPath()
{
    if (g_rtJpFontPath[0] != '\0')
    {
        return g_rtJpFontPath;
    }
    char winDir[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryA(winDir, MAX_PATH);
    if (n != 0 && n < MAX_PATH)
    {
        static const char* kFaces[] = { "meiryo.ttc", "YuGothM.ttc", "yugothic.ttf", "msgothic.ttc" };
        for (const char* face : kFaces)
        {
            char path[MAX_PATH] = {};
            std::snprintf(path, sizeof(path), "%s\\Fonts\\%s", winDir, face);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES
                && IsStbLoadableFontFile(path))
            {
                std::snprintf(g_rtJpFontPath, sizeof(g_rtJpFontPath), "%s", path);
                return g_rtJpFontPath;
            }
        }
    }

    const std::string modDir = OverlayModuleDirectory();
    if (!modDir.empty())
    {
        static const char* kBundled[] = {
            "NotoSansCJKjp-Regular.ttf",       // proportional - preferred
            "NotoSansMonoCJKjp-Regular.ttf",
        };
        for (const char* file : kBundled)
        {
            char path[MAX_PATH] = {};
            std::snprintf(path, sizeof(path), "%s\\assets\\fonts\\%s", modDir.c_str(), file);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES
                && IsStbLoadableFontFile(path))
            {
                std::snprintf(g_rtJpFontPath, sizeof(g_rtJpFontPath), "%s", path);
                return g_rtJpFontPath;
            }
        }
    }
    return nullptr;
}

// Only the profiles that carry player-provided text (nicknames, room names,
// option values) get the ~3000-glyph Japanese ranges; header profiles show
// static English labels and skipping them keeps the atlas within the texture
// limits of older GPUs.
bool ProfileWantsJapanese(size_t profileIndex)
{
    return profileIndex == static_cast<size_t>(RtTextProfile::MenuRow)
        || profileIndex == static_cast<size_t>(RtTextProfile::Footer)
        || profileIndex == static_cast<size_t>(RtTextProfile::BattleLogRow)
        || profileIndex == static_cast<size_t>(RtTextProfile::OverlayBody);
}

// (Re)bake all fonts into the atlas: the async badge font plus one font per
// distinct profile font size (profiles sharing a px share the ImFont).  The
// primary face is baked with Latin+Cyrillic ranges; a Japanese font is merged
// into the name-carrying sizes.  Caller must ensure the DX9 backend recreates
// its device objects afterwards (it does automatically on the next NewFrame
// after InvalidateDeviceObjects).
void LoadRtFonts()
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g_badgeFont = nullptr;
    g_rtExtendedGlyphs = false;
    for (size_t i = 0; i < static_cast<size_t>(RtTextProfile::Count); ++i)
    {
        g_rtProfileFonts[i] = nullptr;
    }

    // Embedded last-resort font: also serves as the JP fill-in source when
    // neither a Japanese system font nor the assets folder exists.
    void* embeddedData = nullptr;
    int embeddedSize = 0;
    (void)GetEmbeddedFallbackFont(&embeddedData, &embeddedSize);

    char primaryPath[MAX_PATH] = {};
    const RtFontFace* face =
        ResolvePrimaryFont(primaryPath, sizeof(primaryPath), embeddedData != nullptr);
    if (face == nullptr)
    {
        io.Fonts->AddFontDefault();
        mod::Log("DebugOverlay: no loadable TTF found (files or embedded), using default font");
        return;
    }
    const bool primaryFromMemory = (primaryPath[0] == '\0');
    const char* path = primaryFromMemory ? nullptr : primaryPath;
    const char* faceLabel = face->label;
    const bool jpCapablePrimary = face->jpCapable;
    const RtProfileStyle* faceDefaults = face->profileDefaults;

    // Seed the working profile set from the face's tuned defaults, but only
    // when the face actually changed - same-face rebuilds keep the values
    // the user is dialing in with the panel sliders.
    if (_stricmp(g_rtAppliedFaceLabel, faceLabel) != 0)
    {
        std::snprintf(g_rtAppliedFaceLabel, sizeof(g_rtAppliedFaceLabel), "%s", faceLabel);
        for (size_t i = 0; i < static_cast<size_t>(RtTextProfile::Count); ++i)
        {
            g_rtProfiles[i] = faceDefaults[i];
        }
        mod::Log("DebugOverlay: profile defaults applied for face '%s'", faceLabel);
    }

    // Fill-in source for primaries without native Cyrillic/Japanese glyphs:
    // a Japanese font file if one exists, else the embedded font.
    const char* jpPath = jpCapablePrimary ? nullptr : ResolveJapaneseFontPath();
    const bool jpFromMemory =
        !jpCapablePrimary && jpPath == nullptr && embeddedData != nullptr;
    // Cyrillic ranges are a superset of the default Latin ranges.
    const ImWchar* baseRanges = io.Fonts->GetGlyphRangesCyrillic();
    bool jpMergedAnywhere = false;

    // Merge |ranges| into the last-added font from either a file path or the
    // embedded blob.  Resource memory must not be freed by the atlas.
    auto mergeRanges = [&](float px, const ImWchar* ranges, const char* filePath, bool fromMemory) -> bool {
        ImFontConfig mergeConfig;
        mergeConfig.MergeMode = true;
        // CJK at oversample 2 doubles an already large bake; 1 is the
        // recommended setting and halves the atlas cost.
        mergeConfig.OversampleH = 1;
        mergeConfig.OversampleV = 1;
        if (fromMemory)
        {
            mergeConfig.FontDataOwnedByAtlas = false;
            return io.Fonts->AddFontFromMemoryTTF(
                       embeddedData, embeddedSize, px, &mergeConfig, ranges)
                != nullptr;
        }
        if (filePath == nullptr)
        {
            return false;
        }
        return io.Fonts->AddFontFromFileTTF(filePath, px, &mergeConfig, ranges) != nullptr;
    };

    auto addSizedFont = [&](float px, bool wantJapanese) -> ImFont* {
        ImFont* font = nullptr;
        if (primaryFromMemory)
        {
            ImFontConfig memoryConfig;
            memoryConfig.FontDataOwnedByAtlas = false;
            font = io.Fonts->AddFontFromMemoryTTF(
                embeddedData, embeddedSize, px, &memoryConfig, baseRanges);
        }
        else
        {
            font = io.Fonts->AddFontFromFileTTF(path, px, nullptr, baseRanges);
        }
        if (font == nullptr)
        {
            return nullptr;
        }
        if (!jpCapablePrimary && (jpPath != nullptr || jpFromMemory))
        {
            // Primary lacks Cyrillic/CJK: backfill Cyrillic everywhere so
            // e.g. a Latin-only display face still renders Cyrillic names.
            (void)mergeRanges(px, baseRanges, jpPath, jpFromMemory);
        }
        if (wantJapanese)
        {
            const ImWchar* jpRanges = io.Fonts->GetGlyphRangesJapanese();
            bool merged = false;
            if (jpCapablePrimary)
            {
                merged = mergeRanges(px, jpRanges, path, primaryFromMemory);
            }
            else
            {
                merged = mergeRanges(px, jpRanges, jpPath, jpFromMemory);
            }
            jpMergedAnywhere |= merged;
        }
        return font;
    };

    // Unique sizes with whether any consumer of that size wants Japanese.
    struct SizedFont
    {
        float px;
        bool wantJapanese;
        ImFont* font;
    };
    SizedFont sizes[static_cast<size_t>(RtTextProfile::Count) + 1] = {};
    size_t sizeCount = 0;
    auto noteSize = [&](float px, bool wantJapanese) {
        for (size_t i = 0; i < sizeCount; ++i)
        {
            if (sizes[i].px == px)
            {
                sizes[i].wantJapanese |= wantJapanese;
                return;
            }
        }
        sizes[sizeCount].px = px;
        sizes[sizeCount].wantJapanese = wantJapanese;
        ++sizeCount;
    };
    noteSize(kBadgeFontPx, false);
    for (size_t i = 0; i < static_cast<size_t>(RtTextProfile::Count); ++i)
    {
        noteSize(g_rtProfiles[i].fontPx, ProfileWantsJapanese(i));
    }

    for (size_t i = 0; i < sizeCount; ++i)
    {
        sizes[i].font = addSizedFont(sizes[i].px, sizes[i].wantJapanese);
    }

    auto fontForPx = [&](float px) -> ImFont* {
        for (size_t i = 0; i < sizeCount; ++i)
        {
            if (sizes[i].px == px)
            {
                return sizes[i].font;
            }
        }
        return nullptr;
    };
    g_badgeFont = fontForPx(kBadgeFontPx);
    for (size_t i = 0; i < static_cast<size_t>(RtTextProfile::Count); ++i)
    {
        g_rtProfileFonts[i] = fontForPx(g_rtProfiles[i].fontPx);
    }

    // The hosting-overlay badge ("Hosting... Press F1...") may use a different
    // face than the menu. Its text is always ASCII, so no JP/CJK merge is
    // needed - a plain bake at the badge px from the configured hosting-tip
    // face. When that face matches the menu face we just keep the primary bake.
    {
        const std::string& badgeConfigured = netplay::mod_settings::HostingTipFontFace();
        if (!badgeConfigured.empty() && _stricmp(badgeConfigured.c_str(), faceLabel) != 0)
        {
            char badgePath[MAX_PATH] = {};
            const RtFontFace* badgeFace = ResolveFontByLabel(
                badgeConfigured.c_str(), badgePath, sizeof(badgePath), embeddedData != nullptr);
            if (badgeFace != nullptr)
            {
                ImFont* badgeFont = nullptr;
                if (badgePath[0] != '\0')
                {
                    badgeFont = io.Fonts->AddFontFromFileTTF(badgePath, kBadgeFontPx, nullptr, baseRanges);
                }
                else if (embeddedData != nullptr)
                {
                    ImFontConfig memoryConfig;
                    memoryConfig.FontDataOwnedByAtlas = false;
                    badgeFont = io.Fonts->AddFontFromMemoryTTF(
                        embeddedData, embeddedSize, kBadgeFontPx, &memoryConfig, baseRanges);
                }
                if (badgeFont != nullptr)
                {
                    g_badgeFont = badgeFont;
                    mod::Log(
                        "DebugOverlay: hosting-tip badge baked from separate face '%s' (menu face '%s')",
                        badgeFace->label, faceLabel);
                }
            }
        }
    }

    if (io.Fonts->Fonts.empty())
    {
        io.Fonts->AddFontDefault();
    }
    g_rtExtendedGlyphs = jpMergedAnywhere;
    mod::Log(
        "DebugOverlay: fonts baked from %s (face='%s' jpCapable=%d jpMerge=%s, "
        "%u sizes, menu %.0f/%.0f/%.0f/%.0f/%.0f/%.0f px, "
        "overlay %.0f/%.0f/%.0f px)",
        path != nullptr ? path : "<embedded resource>",
        faceLabel,
        jpCapablePrimary ? 1 : 0,
        jpPath != nullptr ? jpPath : (jpFromMemory ? "<embedded resource>" : "none"),
        static_cast<unsigned>(sizeCount),
        g_rtProfiles[0].fontPx,
        g_rtProfiles[1].fontPx,
        g_rtProfiles[2].fontPx,
        g_rtProfiles[3].fontPx,
        g_rtProfiles[4].fontPx,
        g_rtProfiles[5].fontPx,
        g_rtProfiles[6].fontPx,
        g_rtProfiles[7].fontPx,
        g_rtProfiles[8].fontPx);
}

LRESULT CALLBACK DebugWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_inited)
    {
        // Toggle key: backslash ("\", next to Enter) = VK_OEM_5. DELETE is
        // already used elsewhere in the game.
        if (msg == WM_KEYDOWN && wParam == VK_OEM_5
            && netplay::mod_settings::IsDebugMenuEnabled())
        {
            g_panelOpen = !g_panelOpen;
            mod::Log("DebugOverlay: panel %s (backslash)", g_panelOpen ? "opened" : "closed");
        }

        const LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        if (handled != 0 && g_panelOpen)
        {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.WantCaptureMouse || io.WantCaptureKeyboard)
            {
                return handled;
            }
        }
    }

    if (g_prevWndProc != nullptr)
    {
        return CallWindowProcA(g_prevWndProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

bool ShutdownImGui()
{
    if (!g_inited)
    {
        return true;
    }
    // Restore our WndProc only when DebugWndProc is still the *live* top-level
    // proc and we know what preceded it. If another proc now owns the top --
    // most commonly because RemoveNetplayWindowHook() ran just before this
    // during the online-simulation suspend and already restored the stock proc,
    // unchaining us -- then rewriting GWLP_WNDPROC would clobber a proc we do
    // not own, so we leave it alone.
    //
    // Being unchained is the SUCCESS state, not a failure. Once g_inited is
    // cleared below, DebugWndProc early-outs on its g_inited guard and simply
    // forwards, never touching the destroyed ImGui context, so it is inert
    // whether or not it is still somewhere in the chain. Teardown must
    // therefore NEVER return false: returning false made
    // SuspendUiHooksForOnlineSimulation report imgui=0, which made the
    // connected-session handoff retry every frame indefinitely -- pinning the
    // game near ~10fps until the remote peer timed out ("dropped connection on
    // sync").
    bool detachedFromChain = false;
    if (g_hwnd != nullptr && IsWindow(g_hwnd))
    {
        SetLastError(NO_ERROR);
        const LONG_PTR currentValue = GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
        const DWORD readError = GetLastError();
        const bool readOk =
            !(currentValue == 0 && readError != NO_ERROR);
        const bool weOwnTop = readOk
            && reinterpret_cast<WNDPROC>(currentValue) == &DebugWndProc;

        if (weOwnTop && g_prevWndProc != nullptr)
        {
            SetLastError(NO_ERROR);
            const LONG_PTR result = SetWindowLongPtrA(
                g_hwnd,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(g_prevWndProc));
            const DWORD restoreError = GetLastError();
            if (result != 0 || restoreError == NO_ERROR)
            {
                detachedFromChain = true;
            }
            else
            {
                mod::Log(
                    "DebugOverlay: WndProc restore failed hwnd=%p err=%lu "
                    "(deinitialising anyway; detour degrades to pass-through)",
                    g_hwnd,
                    static_cast<unsigned long>(restoreError));
            }
        }
        else
        {
            mod::Log(
                "DebugOverlay: WndProc restore skipped, detour not top-level "
                "hwnd=%p current=%p hook=%p previous=%p readErr=%lu "
                "(deinitialising; detour is inert once g_inited clears)",
                g_hwnd,
                reinterpret_cast<void*>(currentValue),
                reinterpret_cast<void*>(&DebugWndProc),
                reinterpret_cast<void*>(g_prevWndProc),
                static_cast<unsigned long>(readError));
        }
    }

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_inited = false;
    g_device = nullptr;
    g_hwnd = nullptr;
    g_badgeFont = nullptr;
    for (size_t i = 0; i < static_cast<size_t>(RtTextProfile::Count); ++i)
    {
        g_rtProfileFonts[i] = nullptr;
    }
    InterlockedExchange(&g_rtFontRebuildRequested, 0);
    g_lastBackBufferW = 0;
    g_lastBackBufferH = 0;

    // Only forget our predecessor once we actually detached from the chain. If
    // DebugWndProc might still be chained below another proc (we did not own the
    // top), keep g_prevWndProc so its forwarding path still reaches the real
    // proc beneath it.
    if (detachedFromChain)
    {
        g_prevWndProc = nullptr;
    }
    return true;
}

// Returns the device's current RENDER TARGET size, or {0,0} on failure. During
// the game's scene draw this is 640x480; during Revival's window present it is the
// window size. Used to pick the correct surface.
void GetRenderTargetSize(IDirect3DDevice9* device, int* outW, int* outH)
{
    *outW = 0;
    *outH = 0;
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt != nullptr)
    {
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(rt->GetDesc(&desc)))
        {
            *outW = static_cast<int>(desc.Width);
            *outH = static_cast<int>(desc.Height);
        }
        rt->Release();
    }
}

// Returns the swap chain's backbuffer (window) size, or {0,0} on failure. Unlike
// the 640x480 render target, this changes on window resize / fullscreen toggle,
// so it is what we use to detect a device reset (ImGui object rebuild).
void GetSwapChainBackBufferSize(IDirect3DDevice9* device, int* outW, int* outH)
{
    *outW = 0;
    *outH = 0;
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc != nullptr)
    {
        D3DPRESENT_PARAMETERS pp = {};
        if (SUCCEEDED(sc->GetPresentParameters(&pp)))
        {
            *outW = static_cast<int>(pp.BackBufferWidth);
            *outH = static_cast<int>(pp.BackBufferHeight);
        }
        sc->Release();
    }
}

bool EnsureInited(IDirect3DDevice9* device)
{
    if (g_inited && g_device == device)
    {
        return true;
    }
    if (g_inited && g_device != device)
    {
        // A normal D3D9 recreation keeps the same focus window. Rebind only
        // the DX9 backend so the existing WndProc chain stays intact even when
        // NetplayWindowProc currently sits above DebugWndProc.
        HWND replacementHwnd = nullptr;
        D3DDEVICE_CREATION_PARAMETERS replacementCp = {};
        if (SUCCEEDED(device->GetCreationParameters(&replacementCp)))
        {
            replacementHwnd = replacementCp.hFocusWindow;
        }
        if (replacementHwnd == nullptr)
        {
            replacementHwnd = GetActiveWindow();
        }
        if (replacementHwnd == g_hwnd)
        {
            ImGui_ImplDX9_Shutdown();
            if (!ImGui_ImplDX9_Init(device))
            {
                mod::Log(
                    "DebugOverlay: DX9 backend rebind failed device=%p",
                    device);
                return false;
            }
            g_device = device;
            g_lastBackBufferW = 0;
            g_lastBackBufferH = 0;
            mod::Log(
                "DebugOverlay: DX9 backend rebound device=%p hwnd=%p",
                device,
                replacementHwnd);
            return true;
        }

        // A real window change needs a full ownership-checked teardown.
        if (!ShutdownImGui())
        {
            return false;
        }
    }

    HWND hwnd = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (SUCCEEDED(device->GetCreationParameters(&cp)))
    {
        hwnd = cp.hFocusWindow;
    }
    if (hwnd == nullptr)
    {
        hwnd = GetActiveWindow();
    }
    if (hwnd == nullptr)
    {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // do not write imgui.ini
    ImGui::StyleColorsDark();

    // Load crisp TTFs before the backend builds the font atlas (on first frame).
    LoadRtFonts();

    if (!ImGui_ImplWin32_Init(hwnd))
    {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX9_Init(device))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    // Chain our wndproc so ImGui receives input and DELETE toggles the panel.
    // Installed once; the netplay menu's own wndproc hook chains correctly
    // because this one is installed earlier (first EndScene of the session).
    g_prevWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DebugWndProc)));

    g_hwnd = hwnd;
    g_device = device;
    g_inited = true;
    mod::Log("DebugOverlay: ImGui initialised hwnd=0x%p device=0x%p", hwnd, device);
    return true;
}

void DrawAsyncIndicator()
{
    namespace ah = netplay::bridge::async_host;
    if (!ah::IsActive() || !ah::IsMinimized())
    {
        return;
    }

    char msg[160] = {};
    ImU32 textColor = IM_COL32(255, 255, 255, 255);
    if (ah::IsTimedOut())
    {
        std::snprintf(msg, sizeof(msg), "Opponent timed out - %s to return", ah::ReturnKeyDisplay());
        textColor = IM_COL32(255, 210, 210, 255);
    }
    else if (ah::IsPeerFoundHeld())
    {
        std::snprintf(msg, sizeof(msg), "OPPONENT FOUND!  Press %s to start", ah::ReturnKeyDisplay());
        textColor = IM_COL32(120, 255, 140, 255);
    }
    else
    {
        std::snprintf(msg, sizeof(msg), "Hosting...  Press %s to return to HOST menu", ah::ReturnKeyDisplay());
    }

    const ImGuiIO& io = ImGui::GetIO();
    ImFont* font = (g_badgeFont != nullptr) ? g_badgeFont : ImGui::GetFont();
    const float baseSize = (g_badgeFont != nullptr) ? kBadgeFontPx : ImGui::GetFontSize();
    const float fontSize = baseSize * g_asyncFontScale;
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, msg);
    const float cx = io.DisplaySize.x * g_asyncPosX;
    const float ty = io.DisplaySize.y * g_asyncPosY;
    const ImVec2 pos(cx - textSize.x * 0.5f, ty);
    const float pad = 5.0f;

    // Background list keeps the badge above the game but below the panel.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(
        ImVec2(pos.x - pad, pos.y - pad),
        ImVec2(pos.x + textSize.x + pad, pos.y + textSize.y + pad),
        IM_COL32(10, 16, 28, static_cast<int>(g_asyncBgAlpha * 255.0f)),
        4.0f);
    dl->AddText(font, fontSize, pos, textColor, msg);
}

// Draw the committed game-RT text items. Coordinates are 320x240 menu-logical;
// the RT is guaranteed 640x480 by the caller's render-target filter, so the
// mapping is exactly x2 (same result as the icon renderer's letterbox math).
void DrawRtTextItems()
{
    EnsureRtTextLock();
    EnterCriticalSection(&g_rtTextLock);
    // Background list: renders above the game but BELOW ImGui windows, so
    // the debug panel stays the top layer instead of being overdrawn.
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (const RtTextItem& item : g_rtTextActive)
    {
        ImFont* font = RtFontFor(item.profile);
        if (font == nullptr || !font->IsLoaded())
        {
            continue;
        }
        const RtProfileStyle& style = RtStyleFor(item.profile);
        const float fontPx = style.fontPx;
        const ImVec2 textSize = font->CalcTextSizeA(fontPx, FLT_MAX, 0.0f, item.text);
        const bool transientOverlay =
            item.profile == RtTextProfile::OverlayTitle
            || item.profile == RtTextProfile::OverlayBody
            || item.profile == RtTextProfile::OverlayHint;
        const float offsetX = transientOverlay ? g_transientTextOffsetX * 2.0f : 0.0f;
        const float offsetY = transientOverlay ? g_transientTextOffsetY * 2.0f : 0.0f;
        const float rtX0 = static_cast<float>(item.x0) * 2.0f + offsetX;
        const float rtX1 = static_cast<float>(item.x1) * 2.0f + offsetX;
        float x = rtX0;
        if (item.align == RtTextAlign::Center)
        {
            x = rtX0 + ((rtX1 - rtX0) - textSize.x) * 0.5f;
        }
        else if (item.align == RtTextAlign::Right)
        {
            x = rtX1 - textSize.x;
        }
        if (x < rtX0)
        {
            x = rtX0; // never spill left of the field when the text overflows
        }
        // item.y is the top of the logical cell the text replaces; center the
        // TTF line height on the profile's cell so rows keep their rhythm.
        const float y = static_cast<float>(item.y) * 2.0f
            + (style.cellRtPx - textSize.y) * 0.5f
            + style.yBiasRtPx
            + offsetY;
        const bool clip = item.clipX1 > item.clipX0;
        if (clip)
        {
            dl->PushClipRect(
                ImVec2(static_cast<float>(item.clipX0) * 2.0f, 0.0f),
                ImVec2(static_cast<float>(item.clipX1) * 2.0f, static_cast<float>(kGameRtH)),
                true);
        }
        dl->AddText(font, fontPx, ImVec2(x, y), item.rgba, item.text);
        if (clip)
        {
            dl->PopClipRect();
        }
    }
    LeaveCriticalSection(&g_rtTextLock);
}

// Log the working profile set in the exact initializer form used by the
// per-face default tables, so tuned values can be pasted straight back in.
void LogRtProfileValues(const char* reason)
{
    mod::Log(
        "DebugOverlay: RT profiles (%s) face='%s' - paste-ready:",
        reason,
        g_rtAppliedFaceLabel[0] != '\0' ? g_rtAppliedFaceLabel : "unknown");
    for (size_t i = 0; i < static_cast<size_t>(RtTextProfile::Count); ++i)
    {
        const RtProfileStyle& style = g_rtProfiles[i];
        mod::Log(
            "    {\"%s\", %.1ff, %.1ff, %.1ff},",
            style.name,
            style.fontPx,
            style.cellRtPx,
            style.yBiasRtPx);
    }
    mod::Log(
        "DebugOverlay: transient modal text offset logical=(%.1f, %.1f)",
        g_transientTextOffsetX,
        g_transientTextOffsetY);
}

void DrawDebugPanel()
{
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("EFZ Netplay Debug ( \\ to toggle)", &g_panelOpen))
    {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("DisplaySize: %.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
        ImGui::Text("Mouse: %.0f, %.0f", io.MousePos.x, io.MousePos.y);

        ImGui::SeparatorText("Async-host in-battle indicator");
        ImGui::SliderFloat("PosX (frac)", &g_asyncPosX, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("PosY (frac)", &g_asyncPosY, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Font scale", &g_asyncFontScale, 0.5f, 4.0f, "%.2f");
        ImGui::SliderFloat("BG alpha", &g_asyncBgAlpha, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Dial these in, then hardcode the defaults\nin debug_overlay.cpp.");

        ImGui::SeparatorText("Battle Log character icons");
        {
            netplay::battle_log::IconAdjust& icon = netplay::battle_log::GetIconAdjust();
            ImGui::SliderInt("icon offX (logical)", &icon.offsetX, -40, 40);
            ImGui::SliderInt("icon offY (logical)", &icon.offsetY, -40, 40);
            ImGui::SliderFloat("icon scale", &icon.scale, 0.5f, 2.0f, "%.2f");
            if (ImGui::Button("Log icon adjust"))
            {
                mod::Log(
                    "DebugOverlay: battle-log icon adjust - offsetX=%d offsetY=%d scale=%.2ff",
                    icon.offsetX, icon.offsetY, icon.scale);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset icons"))
            {
                icon = netplay::battle_log::IconAdjust{};
            }
        }

        namespace ah = netplay::bridge::async_host;
        ImGui::SeparatorText("Async-host state");
        ImGui::Text("state=%d active=%d minimized=%d held=%d timedOut=%d",
            static_cast<int>(ah::GetState()),
            ah::IsActive() ? 1 : 0,
            ah::IsMinimized() ? 1 : 0,
            ah::IsPeerFoundHeld() ? 1 : 0,
            ah::IsTimedOut() ? 1 : 0);

        ImGui::SeparatorText("RT text profiles");
        ImGui::SliderFloat(
            "Modal text X (logical)",
            &g_transientTextOffsetX,
            -80.0f,
            80.0f,
            "%.1f");
        ImGui::SliderFloat(
            "Modal text Y (logical)",
            &g_transientTextOffsetY,
            -60.0f,
            60.0f,
            "%.1f");
        if (ImGui::Button("Reset modal text position"))
        {
            g_transientTextOffsetX = 0.0f;
            g_transientTextOffsetY = 0.0f;
        }
        ImGui::TextDisabled(
            "OverlayTitle/Body/Hint below control modal text scale.\n"
            "Position applies live in the 320x240 logical space.");
        bool fontPxChanged = false;
        for (size_t i = 0; i < static_cast<size_t>(RtTextProfile::Count); ++i)
        {
            RtProfileStyle& style = g_rtProfiles[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s", style.name);
            ImGui::SliderFloat("font px", &style.fontPx, 8.0f, 32.0f, "%.0f");
            // Rebuild only on slider release - baking the atlas mid-drag
            // would hitch every frame.
            fontPxChanged |= ImGui::IsItemDeactivatedAfterEdit();
            ImGui::SliderFloat("cell px", &style.cellRtPx, 8.0f, 32.0f, "%.0f");
            ImGui::SliderFloat("y bias", &style.yBiasRtPx, -8.0f, 8.0f, "%.1f");
            ImGui::PopID();
        }
        if (ImGui::Button("Apply font sizes (rebuild atlas + log values)"))
        {
            LogRtProfileValues("apply_button");
            InterlockedExchange(&g_rtFontRebuildRequested, 1);
        }
        else if (fontPxChanged)
        {
            LogRtProfileValues("slider_release");
            InterlockedExchange(&g_rtFontRebuildRequested, 1);
        }
        ImGui::TextDisabled(
            "cell/y-bias apply live; font px rebuilds the atlas.\n"
            "Apply logs the values for hardcoding per-face defaults.");
    }
    ImGui::End();
}
} // namespace

bool SuspendForOnlineSimulation()
{
    ClearRtText();
    g_panelOpen = false;
    return ShutdownImGui();
}

bool ReplaceChainedWindowProc(
    HWND hwnd,
    WNDPROC expectedPrevious,
    WNDPROC replacement)
{
    if (!g_inited
        || hwnd == nullptr
        || hwnd != g_hwnd
        || expectedPrevious == nullptr
        || replacement == nullptr
        || g_prevWndProc != expectedPrevious)
    {
        return false;
    }

    g_prevWndProc = replacement;
    mod::Log(
        "DebugOverlay: spliced chained WndProc hwnd=%p removed=%p replacement=%p",
        hwnd,
        reinterpret_cast<void*>(expectedPrevious),
        reinterpret_cast<void*>(replacement));
    return true;
}

bool HasChainedWindowProc(HWND hwnd, WNDPROC expectedPrevious)
{
    if (!g_inited
        || hwnd == nullptr
        || hwnd != g_hwnd
        || expectedPrevious == nullptr
        || g_prevWndProc != expectedPrevious
        || !IsWindow(hwnd))
    {
        return false;
    }

    SetLastError(NO_ERROR);
    const LONG_PTR currentValue = GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    const DWORD readError = GetLastError();
    return !(currentValue == 0 && readError != NO_ERROR)
        && reinterpret_cast<WNDPROC>(currentValue) == &DebugWndProc;
}

void Render(IDirect3DDevice9* device)
{
    if (device == nullptr)
    {
        return;
    }

    namespace ah = netplay::bridge::async_host;
    const bool debugAvailable = netplay::mod_settings::IsDebugMenuEnabled();
    const bool wantIndicator =
        ah::IsActive() && ah::IsMinimized() && !netplay::hooks::IsNetplayMenuActive();
    const bool wantPanel = debugAvailable && g_panelOpen;

    // Game-RT text overlay items committed by a menu producer. Use the
    // published bit for the EndScene fast path: once menu text has been
    // cleared, gameplay frames must not take the RT-text critical section.
    const bool wantRtText =
        InterlockedCompareExchange(&g_rtTextPublished, 0, 0) != 0;

    // Once ImGui's window hook is initialized, a closed panel with no badge or
    // menu text needs no EndScene work. Return before render-target queries,
    // QPC measurements, swap-chain inspection, or backend calls. The window
    // hook remains installed and can make wantPanel true on a later frame.
    const bool needsInitialization = debugAvailable && !g_inited;
    if (!needsInitialization && !wantPanel && !wantIndicator && !wantRtText)
    {
        return;
    }

    // Surface filter - only draw on the 640x480 game render target. EndScene also
    // fires for Revival's window-backbuffer present (different size); drawing there
    // put the overlay on the wrong surface. Skip non-game frames entirely (do not
    // even init, so ImGui binds to the correct device).
    {
        int rtW = 0;
        int rtH = 0;
        GetRenderTargetSize(device, &rtW, &rtH);
        if (rtW != kGameRtW || rtH != kGameRtH)
        {
            return;
        }
    }

#if MOD_LIFECYCLE_TRACE_COMPILED
    // Trace-build measurement only. QueryPerformanceCounter/Frequency and
    // interlocked accounting must not run on every shipping EndScene.
    {
        static LARGE_INTEGER s_lastEndScene = {};
        static LONG s_pairCount = 0;
        static LONG s_pairLogged = 0;
        LARGE_INTEGER now = {}, freq = {};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (s_lastEndScene.QuadPart != 0 && freq.QuadPart != 0)
        {
            const double deltaMs =
                static_cast<double>(now.QuadPart - s_lastEndScene.QuadPart)
                * 1000.0 / static_cast<double>(freq.QuadPart);
            if (deltaMs < 4.0)
            {
                const LONG pairs = InterlockedIncrement(&s_pairCount);
                if ((pairs == 1 || pairs % 3600 == 0)
                    && InterlockedExchange(&s_pairLogged, 1) >= 0)
                {
                    mod::Log(
                        "OVERLAY: double-EndScene pair detected (count=%ld, "
                        "delta=%.2fms) - P3.1 measurement",
                        static_cast<long>(pairs), deltaMs);
                }
            }
        }
        s_lastEndScene = now;
    }
#endif

    // Initialise when the debug option is on (so DELETE works on any screen),
    // when the in-battle indicator needs drawing, or when menu text is queued.
    if (!debugAvailable && !wantIndicator && !wantRtText && !g_inited)
    {
        return;
    }
    if (!EnsureInited(device))
    {
        return;
    }

    // Device-RESET detection: when EFZ resizes the window or toggles
    // borderless-fullscreen it Reset()s the device, which silently destroys
    // ImGui's D3DPOOL_DEFAULT resources (font texture + vertex/index buffers) and
    // makes the overlay vanish. The 640x480 render target never changes, so we
    // watch the swap-chain (window) backbuffer size and rebuild on a change.
    {
        int bbW = 0;
        int bbH = 0;
        GetSwapChainBackBufferSize(device, &bbW, &bbH);
        if (bbW > 0 && bbH > 0 && (bbW != g_lastBackBufferW || bbH != g_lastBackBufferH))
        {
            if (g_lastBackBufferW != 0 || g_lastBackBufferH != 0)
            {
                mod::Log(
                    "DebugOverlay: window backbuffer %dx%d -> %dx%d (device reset) - rebuilding ImGui objects",
                    g_lastBackBufferW, g_lastBackBufferH, bbW, bbH);
                ImGui_ImplDX9_InvalidateDeviceObjects();
            }
            g_lastBackBufferW = bbW;
            g_lastBackBufferH = bbH;
        }
    }

    if (!wantPanel && !wantIndicator && !wantRtText)
    {
        return; // initialised, but nothing to draw this frame
    }

    // Profile font-size change from the debug panel: rebake the atlas before
    // the backend's NewFrame recreates the device objects.
    if (InterlockedExchange(&g_rtFontRebuildRequested, 0) != 0)
    {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        LoadRtFonts();
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // One-shot diagnostics: confirm the ImGui/EndScene path actually renders
    // (the in-battle / outside-menu indicator depends on EndScene firing for the
    // game's device). If this never logs while hosting+minimized in a match, the
    // D3D9 EndScene hook is not reaching the game's present.  Runs after the
    // backend NewFrame so the atlas dimensions and display size are real.
    {
        static bool s_loggedFirstRender = false;
        if (!s_loggedFirstRender)
        {
            s_loggedFirstRender = true;
            const ImGuiIO& io = ImGui::GetIO();
            mod::Log(
                "DebugOverlay: first ImGui render (wantIndicator=%d wantPanel=%d display=%.0fx%.0f atlas=%dx%d extendedGlyphs=%d)",
                wantIndicator ? 1 : 0, wantPanel ? 1 : 0,
                io.DisplaySize.x, io.DisplaySize.y,
                io.Fonts->TexWidth, io.Fonts->TexHeight,
                g_rtExtendedGlyphs ? 1 : 0);
        }
    }

    // We draw onto the 640x480 GAME render target, but ImGui_ImplWin32_NewFrame
    // just set io.DisplaySize to the WINDOW client size (e.g. 1920x1080 in
    // borderless fullscreen). If we leave it, ImGui lays out in window space but
    // renders into the 640-wide viewport, squishing everything to ~1/3 - small
    // text (the HOSTING badge) effectively vanishes while the big panel still
    // shows. Override DisplaySize to the render-target size (what training mode
    // does) so layout matches the surface; scale MousePos into that space so the
    // debug panel stays clickable in fullscreen.
    {
        ImGuiIO& io = ImGui::GetIO();
        const float winW = io.DisplaySize.x;
        const float winH = io.DisplaySize.y;
        if (winW > 0.0f && winH > 0.0f
            && io.MousePos.x > -FLT_MAX * 0.5f && io.MousePos.y > -FLT_MAX * 0.5f)
        {
            io.MousePos.x *= static_cast<float>(kGameRtW) / winW;
            io.MousePos.y *= static_cast<float>(kGameRtH) / winH;
        }
        io.DisplaySize = ImVec2(static_cast<float>(kGameRtW), static_cast<float>(kGameRtH));
    }

    ImGui::NewFrame();

    if (wantRtText)
    {
        DrawRtTextItems();
    }
    if (wantIndicator)
    {
        DrawAsyncIndicator();
    }
    if (wantPanel)
    {
        DrawDebugPanel();
    }

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void ToggleDebugPanel()
{
    if (netplay::mod_settings::IsDebugMenuEnabled())
    {
        g_panelOpen = !g_panelOpen;
    }
}

bool IsDebugPanelActive()
{
    return g_panelOpen;
}

bool IsRtTextAvailable()
{
    return g_inited && g_badgeFont != nullptr && g_badgeFont->IsLoaded();
}

bool RtTextHasExtendedGlyphs()
{
    return IsRtTextAvailable() && g_rtExtendedGlyphs;
}

void NotifyFontSettingsChanged()
{
    // Rebuild lazily on the next Render; harmless when nothing changed and
    // a no-op while ImGui is not initialised (init reads fresh settings).
    InterlockedExchange(&g_rtFontRebuildRequested, 1);
}

int MeasureRtTextWidth(RtTextProfile profile, const char* text)
{
    if (text == nullptr || !IsRtTextAvailable())
    {
        return -1;
    }
    ImFont* font = RtFontFor(profile);
    if (font == nullptr || !font->IsLoaded())
    {
        return -1;
    }
    const ImVec2 textSize = font->CalcTextSizeA(RtFontPxFor(profile), FLT_MAX, 0.0f, text);
    // RT pixels -> menu-logical pixels (x2 mapping), rounded up.
    return static_cast<int>((textSize.x + 1.9f) * 0.5f);
}

void BeginRtTextFrame()
{
    EnsureRtTextLock();
    EnterCriticalSection(&g_rtTextLock);
    g_rtTextStaging.clear();
    LeaveCriticalSection(&g_rtTextLock);
}

void SubmitRtText(const RtTextItem& item)
{
    if (item.text[0] == '\0')
    {
        return;
    }
    EnsureRtTextLock();
    EnterCriticalSection(&g_rtTextLock);
    if (g_rtTextStaging.size() < 256)
    {
        g_rtTextStaging.push_back(item);
    }
    LeaveCriticalSection(&g_rtTextLock);
}

void CommitRtTextFrame()
{
    EnsureRtTextLock();
    EnterCriticalSection(&g_rtTextLock);
    g_rtTextActive.swap(g_rtTextStaging);
    g_rtTextStaging.clear();
    const LONG published = g_rtTextActive.empty() ? 0 : 1;
    LeaveCriticalSection(&g_rtTextLock);
    InterlockedExchange(&g_rtTextPublished, published);
}

void ClearRtText()
{
    if (!g_rtTextLockInited)
    {
        return;
    }
    EnterCriticalSection(&g_rtTextLock);
    g_rtTextActive.clear();
    g_rtTextStaging.clear();
    LeaveCriticalSection(&g_rtTextLock);
    InterlockedExchange(&g_rtTextPublished, 0);
}
} // namespace netplay::debug_overlay
