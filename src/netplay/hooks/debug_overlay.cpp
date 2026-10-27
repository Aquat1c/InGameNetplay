#include "netplay/hooks/debug_overlay.h"
#include "compat/overlay_reset_api.h"
#include "compat/overlay_context_scope.h"

#include "netplay/bridge/async_hosting.h"
#include "netplay/core/battle_log_menu.h"
#include "netplay/core/i18n.h"
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

#include <algorithm>
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
ImGuiContext* g_context = nullptr;
DWORD g_contextThread = 0;
EfzOverlayResetGate g_resetGate;
IDirect3DDevice9* g_device = nullptr;
HWND g_hwnd = nullptr;
WNDPROC g_prevWndProc = nullptr;
bool g_panelOpen = false;

HRESULT ResetGateResult(EfzOverlayResetGate::Result result) {
    switch (result) {
    case EfzOverlayResetGate::Result::Ok: return S_OK;
    case EfzOverlayResetGate::Result::InvalidArgument: return E_INVALIDARG;
    case EfzOverlayResetGate::Result::WrongThread: return RPC_E_WRONG_THREAD;
    default: return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
}

HRESULT __cdecl BeforeOverlayReset(void* device, uint32_t epoch) {
    EfzOverlayResetGate::Access access(g_resetGate);
    if (!access.Acquired()) return HRESULT_FROM_WIN32(ERROR_BUSY);
    bool invalidate = false;
    const auto result = g_resetGate.BeforeReset(device, epoch, GetCurrentThreadId(), invalidate);
    if (result != EfzOverlayResetGate::Result::Ok) return ResetGateResult(result);
    if (invalidate) {
        if (!g_context || !g_inited) return E_UNEXPECTED;
        EfzScopedImGuiContext context(g_context);
        ImGui_ImplDX9_InvalidateDeviceObjects();
        g_resetGate.DidInvalidateBackend();
    }
    return S_OK;
}

HRESULT __cdecl AfterOverlayReset(void* device, uint32_t epoch, uint32_t ready) {
    EfzOverlayResetGate::Access access(g_resetGate);
    if (!access.Acquired()) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if (ready > 1) return E_INVALIDARG;
    const auto result = g_resetGate.AfterReset(device, epoch, GetCurrentThreadId(), 0);
    if (result != EfzOverlayResetGate::Result::Ok) return ResetGateResult(result);
    if (!ready) return S_OK;
    if (g_resetGate.BoundDevice() == device) {
        if (!g_resetGate.BackendInvalidatedFor(device)) return HRESULT_FROM_WIN32(ERROR_BUSY);
        if (!g_context || !g_inited) return E_UNEXPECTED;
        const HRESULT cooperative = static_cast<IDirect3DDevice9*>(device)->TestCooperativeLevel();
        if (cooperative != D3D_OK) return cooperative;
        EfzScopedImGuiContext context(g_context);
        if (!ImGui_ImplDX9_CreateDeviceObjects()) {
            ImGui_ImplDX9_InvalidateDeviceObjects();
            return E_FAIL;
        }
    }
    return ResetGateResult(g_resetGate.AfterReset(device, epoch, GetCurrentThreadId(), 1));
}

ImFont* g_badgeFont = nullptr;
constexpr float kBadgeFontPx = 22.0f;

struct RtProfileStyle
{
    const char* name;
    float fontPx;
    float cellRtPx;
    float yBiasRtPx;
};

using RtProfileDefaults = RtProfileStyle[static_cast<size_t>(RtTextProfile::Count)];
// Base set (original face: Segoe UI).
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
// Tuned for Yu Gothic; the bundled Noto Sans JP shares it as the closest
// approximation.
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
// Tuned for MS Gothic.
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
// Tuned for Meiryo.
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
// Tuned for ITC Bolt (Latin display face).
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
// Tuned for Arial.
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

CRITICAL_SECTION g_rtTextLock;
bool g_rtTextLockInited = false;
std::vector<RtTextItem> g_rtTextStaging;
std::vector<RtTextItem> g_rtTextActive;
volatile LONG g_rtTextPublished = 0;
// Game thread only. 0 = no fade, 1 = fade-out, 2 = fade-in.
int g_rtFadeMode = 0;
int g_rtFadeFrame = 0;
float g_rtTextActiveBrightness = 1.0f;   // under g_rtTextLock

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

constexpr int kGameRtW = 640;
constexpr int kGameRtH = 480;

float g_asyncPosX = 0.5f;     // fraction of screen width  (0=left, 1=right)
float g_asyncPosY = 0.0f;     // fraction of screen height (0=top, 1=bottom)
float g_asyncFontScale = 0.65f;
float g_asyncBgAlpha = 0.48f; // tuned live via the backslash debug panel

float g_transientTextOffsetX = 0.0f;
float g_transientTextOffsetY = 0.0f;

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

// The embedded last-resort font.  Resource memory stays mapped for the
// module's lifetime, so ImGui can reference it without copying
// (FontDataOwnedByAtlas = false).
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

// Resolve the configured face, else the first loadable fallback. An empty
// outPath with a non-null return means the embedded DLL resource.
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

bool ProfileWantsJapanese(size_t profileIndex)
{
    return profileIndex == static_cast<size_t>(RtTextProfile::MenuRow)
        || profileIndex == static_cast<size_t>(RtTextProfile::Footer)
        || profileIndex == static_cast<size_t>(RtTextProfile::BattleLogRow)
        || profileIndex == static_cast<size_t>(RtTextProfile::OverlayBody);
}

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

    if (_stricmp(g_rtAppliedFaceLabel, faceLabel) != 0)
    {
        std::snprintf(g_rtAppliedFaceLabel, sizeof(g_rtAppliedFaceLabel), "%s", faceLabel);
        for (size_t i = 0; i < static_cast<size_t>(RtTextProfile::Count); ++i)
        {
            g_rtProfiles[i] = faceDefaults[i];
        }
        mod::Log("DebugOverlay: profile defaults applied for face '%s'", faceLabel);
    }

    const char* jpPath = jpCapablePrimary ? nullptr : ResolveJapaneseFontPath();
    const bool jpFromMemory =
        !jpCapablePrimary && jpPath == nullptr && embeddedData != nullptr;
    const ImWchar* baseRanges = io.Fonts->GetGlyphRangesCyrillic();
    bool jpMergedAnywhere = false;

    // Exactly the characters the active translation uses, merged into every
    // size (headers and badges included). Must outlive the atlas build.
    static ImVector<ImWchar> s_translationRanges;
    s_translationRanges.clear();
    {
        ImFontGlyphRangesBuilder builder;
        for (const char* text : netplay::i18n::ActiveValues())
        {
            builder.AddText(text);
        }
        builder.BuildRanges(&s_translationRanges);
    }
    const bool haveTranslationRanges = s_translationRanges.Size > 1;

    auto mergeRanges = [&](float px, const ImWchar* ranges, const char* filePath, bool fromMemory) -> bool {
        ImFontConfig mergeConfig;
        mergeConfig.MergeMode = true;
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
            (void)mergeRanges(px, baseRanges, jpPath, jpFromMemory);
        }
        if (haveTranslationRanges)
        {
            jpMergedAnywhere |= jpCapablePrimary
                ? mergeRanges(px, s_translationRanges.Data, path, primaryFromMemory)
                : mergeRanges(px, s_translationRanges.Data, jpPath, jpFromMemory);
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
    DeviceRenderScope input;
    if (input.Admitted() && g_inited)
    {
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

    input.Release();
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
    // Being unchained is the success state, and teardown must NEVER return
    // false or suspend/handoff callers retry every frame indefinitely.
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

    if (g_device) ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(g_context);
    g_context = nullptr;
    g_contextThread = 0;
    g_resetGate.UnbindBackend();
    g_inited = false;
    g_device = nullptr;
    g_hwnd = nullptr;
    g_badgeFont = nullptr;
    for (size_t i = 0; i < static_cast<size_t>(RtTextProfile::Count); ++i)
    {
        g_rtProfileFonts[i] = nullptr;
    }
    InterlockedExchange(&g_rtFontRebuildRequested, 0);

    if (detachedFromChain)
    {
        g_prevWndProc = nullptr;
    }
    return true;
}

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

bool EnsureInited(IDirect3DDevice9* device)
{
    if (!g_resetGate.CanRender(device, GetCurrentThreadId())) return false;
    if (g_inited && g_device == device)
    {
        return true;
    }
    if (g_inited && g_device != device)
    {
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
            if (g_device) ImGui_ImplDX9_Shutdown();
            g_resetGate.UnbindBackend();
            g_device = nullptr;
            if (!ImGui_ImplDX9_Init(device))
            {
                mod::Log(
                    "DebugOverlay: DX9 backend rebind failed device=%p",
                    device);
                return false;
            }
            g_device = device;
            g_resetGate.BindBackend(device, GetCurrentThreadId());
            mod::Log(
                "DebugOverlay: DX9 backend rebound device=%p hwnd=%p",
                device,
                replacementHwnd);
            return true;
        }

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
    g_context = ImGui::CreateContext();
    g_contextThread = GetCurrentThreadId();
    ImGui::SetCurrentContext(g_context);
    ImGui::GetIO().IniFilename = nullptr; // do not write imgui.ini
    ImGui::StyleColorsDark();

    LoadRtFonts();
    // Text sites wait for ImFont::IsLoaded(), which only turns true once the
    // atlas is built; the DX9 backend would build it lazily on the first
    // NewFrame, which runs only after some text was submitted.
    ImGui::GetIO().Fonts->Build();

    if (!ImGui_ImplWin32_Init(hwnd))
    {
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
        g_contextThread = 0;
        return false;
    }
    if (!ImGui_ImplDX9_Init(device))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_context);
        g_context = nullptr;
        g_contextThread = 0;
        return false;
    }

    // Chain our wndproc so ImGui receives input. Installed once, before the
    // netplay menu's own wndproc hook, so that hook chains correctly.
    g_prevWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DebugWndProc)));

    g_hwnd = hwnd;
    g_device = device;
    g_inited = true;
    g_resetGate.BindBackend(device, GetCurrentThreadId());
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

    char msg[256] = {};
    ImU32 textColor = IM_COL32(255, 255, 255, 255);
    if (ah::IsTimedOut())
    {
        std::snprintf(msg, sizeof(msg), netplay::i18n::Tr("Opponent timed out - %s to return"), ah::ReturnKeyDisplay());
        textColor = IM_COL32(255, 210, 210, 255);
    }
    else if (ah::IsPeerFoundHeld())
    {
        std::snprintf(msg, sizeof(msg), netplay::i18n::Tr("OPPONENT FOUND!  Press %s to start"), ah::ReturnKeyDisplay());
        textColor = IM_COL32(120, 255, 140, 255);
    }
    else
    {
        std::snprintf(msg, sizeof(msg), netplay::i18n::Tr("Hosting...  Press %s to return to HOST menu"), ah::ReturnKeyDisplay());
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

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(
        ImVec2(pos.x - pad, pos.y - pad),
        ImVec2(pos.x + textSize.x + pad, pos.y + textSize.y + pad),
        IM_COL32(10, 16, 28, static_cast<int>(g_asyncBgAlpha * 255.0f)),
        4.0f);
    dl->AddText(font, fontSize, pos, textColor, msg);
}

// Draw committed game-RT text items. Coordinates are 320x240 menu-logical;
// the RT is 640x480, so the mapping is exactly x2.
void DrawRtTextItems()
{
    EnsureRtTextLock();
    EnterCriticalSection(&g_rtTextLock);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float brightness = g_rtTextActiveBrightness;
    if (brightness <= 0.0f)
    {
        LeaveCriticalSection(&g_rtTextLock);
        return;
    }
    for (size_t index = 0; index < g_rtTextActive.size(); ++index)
    {
        const RtTextItem& item = g_rtTextActive[index];
        if (item.occluder)
        {
            continue;
        }
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
        const float y = static_cast<float>(item.y) * 2.0f
            + (style.cellRtPx - textSize.y) * 0.5f
            + style.yBiasRtPx
            + offsetY;
        // Visible pieces of the text box: the item clip minus every occluder
        // submitted after it. Each cut leaves at most 4 pieces.
        ImVec4 pieces[16];
        size_t pieceCount = 1;
        pieces[0] = ImVec4(x - 4.0f, y - 4.0f, x + textSize.x + 4.0f, y + textSize.y + 4.0f);
        if (item.clipX1 > item.clipX0)
        {
            pieces[0].x = (std::max)(pieces[0].x, static_cast<float>(item.clipX0) * 2.0f);
            pieces[0].z = (std::min)(pieces[0].z, static_cast<float>(item.clipX1) * 2.0f);
        }
        for (size_t j = index + 1; j < g_rtTextActive.size() && pieceCount > 0; ++j)
        {
            const RtTextItem& cut = g_rtTextActive[j];
            if (!cut.occluder)
            {
                continue;
            }
            const ImVec4 c(
                static_cast<float>(cut.x0) * 2.0f, static_cast<float>(cut.y) * 2.0f,
                static_cast<float>(cut.x1) * 2.0f, static_cast<float>(cut.y1) * 2.0f);
            ImVec4 next[16];
            size_t nextCount = 0;
            for (size_t p = 0; p < pieceCount; ++p)
            {
                const ImVec4 r = pieces[p];
                if (r.x >= c.z || r.z <= c.x || r.y >= c.w || r.w <= c.y)
                {
                    if (nextCount < 16) next[nextCount++] = r;
                    continue;
                }
                const float midTop = (std::max)(r.y, c.y);
                const float midBottom = (std::min)(r.w, c.w);
                const ImVec4 parts[4] = {
                    ImVec4(r.x, r.y, r.z, c.y),
                    ImVec4(r.x, c.w, r.z, r.w),
                    ImVec4(r.x, midTop, c.x, midBottom),
                    ImVec4(c.z, midTop, r.z, midBottom),
                };
                for (const ImVec4& part : parts)
                {
                    if (part.z > part.x && part.w > part.y && nextCount < 16)
                    {
                        next[nextCount++] = part;
                    }
                }
            }
            std::memcpy(pieces, next, sizeof(ImVec4) * nextCount);
            pieceCount = nextCount;
        }

        ImU32 color = item.rgba;
        if (brightness < 1.0f)
        {
            const auto scale = [brightness](ImU32 channel) {
                return static_cast<ImU32>(static_cast<float>(channel & 0xFFu) * brightness + 0.5f);
            };
            color = (item.rgba & 0xFF000000u)
                | (scale(item.rgba >> 16) << 16)
                | (scale(item.rgba >> 8) << 8)
                | scale(item.rgba);
        }
        for (size_t p = 0; p < pieceCount; ++p)
        {
            const ImVec4& r = pieces[p];
            dl->PushClipRect(ImVec2(r.x, r.y), ImVec2(r.z, r.w), true);
            dl->AddText(font, fontPx, ImVec2(x, y), color, item.text);
            dl->PopClipRect();
        }
    }
    LeaveCriticalSection(&g_rtTextLock);
}

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

DeviceRenderScope::DeviceRenderScope(IDirect3DDevice9* device)
    : access_(g_resetGate) {
    if (!access_.Acquired()) return;
    if (!device) device = g_device;
    if (!g_resetGate.CanRender(device, GetCurrentThreadId())
        || (g_context && g_contextThread != GetCurrentThreadId())) {
        access_.Release();
        return;
    }
    previousContext_ = ImGui::GetCurrentContext();
    ownedContextAtEntry_ = g_context;
    if (g_context) ImGui::SetCurrentContext(g_context);
    admitted_ = true;
}

DeviceRenderScope::~DeviceRenderScope() { Release(); }

void DeviceRenderScope::Release() {
    if (admitted_) {
        // EnsureInited can replace a host, destroying the old owned context.
        if (previousContext_ == ownedContextAtEntry_ && g_context != ownedContextAtEntry_)
            previousContext_ = nullptr;
        ImGui::SetCurrentContext(previousContext_);
        admitted_ = false;
    }
    access_.Release();
}

const EfzOverlayResetApiV1* QueryResetApi() {
    static const EfzOverlayResetApiV1 api = {
        sizeof(EfzOverlayResetApiV1), 1, &BeforeOverlayReset, &AfterOverlayReset
    };
    return &api;
}

bool SuspendForOnlineSimulation()
{
    EfzOverlayResetGate::Access access(g_resetGate);
    if (!access.Acquired()
        || (g_context && g_contextThread != GetCurrentThreadId()))
        return false;
    EfzScopedImGuiContext context(g_context);
    ClearRtText();
    g_panelOpen = false;
    context.ForgetDestroyed(g_context);
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

    DeviceRenderScope render(device);
    if (!render.Admitted()) return;

    namespace ah = netplay::bridge::async_host;
    const bool debugAvailable = netplay::mod_settings::IsDebugMenuEnabled();
    const bool wantIndicator =
        ah::IsActive() && ah::IsMinimized() && !netplay::hooks::IsNetplayMenuActive();
    const bool wantPanel = debugAvailable && g_panelOpen;

    const bool wantRtText =
        InterlockedCompareExchange(&g_rtTextPublished, 0, 0) != 0;

    // Menu TTF text needs ImGui before anything is submitted: text sites check
    // IsRtTextAvailable() (non-ASCII footer lines wait for extended glyphs), so
    // waiting for a first submission left the first menu visit on the bitmap font.
    const bool menuWantsRtText = netplay::mod_settings::IsMenuTtfTextEnabled()
        && netplay::hooks::IsNetplayMenuActive()
        && !netplay::hooks::IsOnlineSimulationUiSuspended();
    const bool needsInitialization = (debugAvailable || menuWantsRtText) && !g_inited;
    if (!needsInitialization && !wantPanel && !wantIndicator && !wantRtText)
    {
        return;
    }

    // No normal device queries are legal while loss/recovery is pending.
    if (device->TestCooperativeLevel() != D3D_OK) return;

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

    if (!needsInitialization && !wantIndicator && !wantRtText && !g_inited)
    {
        return;
    }
    if (!EnsureInited(device))
    {
        return;
    }

    if (!wantPanel && !wantIndicator && !wantRtText)
    {
        return; // initialised, but nothing to draw this frame
    }

    if (InterlockedExchange(&g_rtFontRebuildRequested, 0) != 0)
    {
        ImGui_ImplDX9_InvalidateDeviceObjects();
        LoadRtFonts();
        ImGui::GetIO().Fonts->Build();
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

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

void SetRtText(RtTextItem& item, const char* text, size_t length)
{
    size_t bytes = (std::min)(length, sizeof(item.text) - 1);
    if (bytes < length)
    {
        while (bytes > 0
            && (static_cast<unsigned char>(text[bytes]) & 0xC0u) == 0x80u)
        {
            --bytes;   // text[bytes] continues a character: cut before it
        }
    }
    std::memcpy(item.text, text, bytes);
    item.text[bytes] = '\0';
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

void SubmitRtOccluder(int x, int y, int w, int h)
{
    if (!netplay::mod_settings::IsMenuTtfTextEnabled())
    {
        return;   // no RT frame is open to receive it
    }
    RtTextItem occluder;
    occluder.occluder = true;
    occluder.x0 = static_cast<int16_t>(x);
    occluder.x1 = static_cast<int16_t>(x + w);
    occluder.y = static_cast<int16_t>(y);
    occluder.y1 = static_cast<int16_t>(y + h);
    EnsureRtTextLock();
    EnterCriticalSection(&g_rtTextLock);
    if (g_rtTextStaging.size() < 256)
    {
        g_rtTextStaging.push_back(occluder);
    }
    LeaveCriticalSection(&g_rtTextLock);
}

void BeginRtTextFade(bool fadeIn)
{
    g_rtFadeMode = fadeIn ? 2 : 1;
    g_rtFadeFrame = 0;
}

void EndRtTextFade()
{
    const bool fadedOut = g_rtFadeMode == 1;
    g_rtFadeMode = 0;
    EnsureRtTextLock();
    EnterCriticalSection(&g_rtTextLock);
    g_rtTextActiveBrightness = fadedOut ? 0.0f : 1.0f;
    LeaveCriticalSection(&g_rtTextLock);
}

void CommitRtTextFrame()
{
    // Render step n of a fade is presented under the palette set by step n-1
    // (efz fade loop: render, then set the palette at 5% per step).
    float brightness = 1.0f;
    if (g_rtFadeMode != 0)
    {
        const int n = g_rtFadeFrame++;
        const float level = n == 0 ? 0.0f : (std::min)(static_cast<float>(n - 1) / 20.0f, 1.0f);
        brightness = g_rtFadeMode == 1 ? (n == 0 ? 1.0f : 1.0f - level) : level;
    }
    EnsureRtTextLock();
    EnterCriticalSection(&g_rtTextLock);
    g_rtTextActive.swap(g_rtTextStaging);
    g_rtTextStaging.clear();
    g_rtTextActiveBrightness = brightness;
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

extern "C" __declspec(dllexport) const EfzOverlayResetApiV1* __cdecl
EFZ_QueryOverlayResetApiV1(void) {
    return netplay::debug_overlay::QueryResetApi();
}
