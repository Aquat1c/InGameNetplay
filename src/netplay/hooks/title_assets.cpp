#include "netplay/hooks/internal/shared.h"

#include "logger.h"
#include "netplay/assets/assets.h"
#include "netplay/core/validation.h"

#include <array>
#include <cctype>

namespace netplay::hooks::internal
{
using namespace netplay::constants;
using NetplayMenuId = netplay::menu::NetplayMenuId;
using NetplayMenuSpec = netplay::menu::NetplayMenuSpec;
using netplay::menu::GetMenuEntries;
using netplay::assets::DeriveConfigStyleRowsFromDat;
using netplay::assets::DetermineObjectProfile;
using netplay::assets::FileExists;
using netplay::assets::ParseEfzDatImage;
using netplay::assets::ParsedDatImage;
using netplay::assets::ResolveNetplayBackgroundPath;
using netplay::assets::ResolveNetplayObjectsPath;
using netplay::assets::ResolveTitleObjectsPath;
using netplay::assets::NetplayObjectProfile;
using netplay::validation::IsValidNickname;
using netplay::validation::ParsePort;

namespace
{
enum class FrontendSurfaceSlot
{
    Background,
    Objects,
};

const char* NetplayMenuThemeToString(NetplayMenuTheme theme)
{
    switch (theme)
    {
    case NetplayMenuTheme::Scroll:
        return "scroll";
    case NetplayMenuTheme::Classic:
        return "classic";
    default:
        return "unknown";
    }
}

bool TryParseNetplayMenuTheme(const std::string& text, NetplayMenuTheme* outTheme)
{
    if (outTheme == nullptr)
    {
        return false;
    }

    std::string lower;
    lower.reserve(text.size());
    for (char ch : text)
    {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (lower == "scroll" || lower == "config_scroll")
    {
        *outTheme = NetplayMenuTheme::Scroll;
        return true;
    }

    if (lower == "classic" || lower == "static")
    {
        *outTheme = NetplayMenuTheme::Classic;
        return true;
    }

    return false;
}

const char* FrontendSurfaceSlotName(FrontendSurfaceSlot slot)
{
    switch (slot)
    {
    case FrontendSurfaceSlot::Background:
        return "background";
    case FrontendSurfaceSlot::Objects:
        return "objects";
    default:
        return "unknown";
    }
}

uint32_t FrontendSurfaceSlotOffset(FrontendSurfaceSlot slot)
{
    switch (slot)
    {
    case FrontendSurfaceSlot::Background:
        return kOffsetBackgroundSurface;
    case FrontendSurfaceSlot::Objects:
        return kOffsetObjectsSurface;
    default:
        return 0;
    }
}

bool ReleaseFrontendSurfaceSlot(uint32_t screenContext, FrontendSurfaceSlot slot, const char* owner)
{
    const uint32_t offset = FrontendSurfaceSlotOffset(slot);
    if (offset == 0)
    {
        return false;
    }

    auto* const surfacePtr = reinterpret_cast<uint32_t*>(screenContext + offset);
    uint32_t oldSurface = 0;
    bool released = false;
    bool faulted = false;
    __try
    {
        oldSurface = *surfacePtr;
        if (oldSurface != 0)
        {
            auto** const vtable = *reinterpret_cast<void***>(oldSurface);
            auto const release = reinterpret_cast<ULONG(__stdcall*)(uint32_t)>(vtable[2]);
            (void)release(oldSurface);
            *surfacePtr = 0;
            released = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        faulted = true;
    }

    mod::Log(
        "FRONTEND_SURFACE_SLOT_RELEASE owner=%s slot=%s old=0x%08lX released=%d faulted=%d",
        owner != nullptr ? owner : "unknown",
        FrontendSurfaceSlotName(slot),
        static_cast<unsigned long>(oldSurface),
        released ? 1 : 0,
        faulted ? 1 : 0);
    return released && !faulted;
}

uint32_t ReadFrontendSurfaceSlot(uint32_t screenContext, FrontendSurfaceSlot slot)
{
    const uint32_t offset = FrontendSurfaceSlotOffset(slot);
    if (offset == 0)
    {
        return 0;
    }

    uint32_t surface = 0;
    __try
    {
        surface = *reinterpret_cast<uint32_t*>(screenContext + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        surface = 0;
    }
    return surface;
}

void ReleaseFrontendSurfaceSlots(uint32_t screenContext, const char* owner)
{
    (void)ReleaseFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Background, owner);
    (void)ReleaseFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Objects, owner);
}

std::string TrimAscii(std::string value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
    {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }

    return value.substr(begin, end - begin);
}

std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
    {
        return std::string();
    }

    const int bytes = WideCharToMultiByte(
        CP_UTF8,
        0,
        wide.data(),
        static_cast<int>(wide.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (bytes <= 0)
    {
        return std::string();
    }

    std::string utf8(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            0,
            wide.data(),
            static_cast<int>(wide.size()),
            utf8.data(),
            bytes,
            nullptr,
            nullptr)
        <= 0)
    {
        return std::string();
    }

    return utf8;
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
    {
        return std::wstring();
    }

    const int chars = MultiByteToWideChar(
        CP_UTF8,
        0,
        utf8.data(),
        static_cast<int>(utf8.size()),
        nullptr,
        0);
    if (chars <= 0)
    {
        return std::wstring();
    }

    std::wstring wide(static_cast<size_t>(chars), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8.data(),
            static_cast<int>(utf8.size()),
            wide.data(),
            chars)
        <= 0)
    {
        return std::wstring();
    }

    return wide;
}

std::wstring ResolveRevivalIniPathWide()
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath))) == 0)
    {
        return L"EfzRevival.ini";
    }

    std::wstring iniPath = exePath;
    const size_t sep = iniPath.find_last_of(L"\\/");
    if (sep == std::wstring::npos)
    {
        return L"EfzRevival.ini";
    }

    iniPath.resize(sep + 1);
    iniPath += L"EfzRevival.ini";
    return iniPath;
}

std::string ResolveRevivalIniPath()
{
    const std::wstring widePath = ResolveRevivalIniPathWide();
    const std::string utf8Path = WideToUtf8(widePath);
    return utf8Path.empty() ? "EfzRevival.ini" : utf8Path;
}

std::string ReadIniValueUtf8(
    const std::wstring& iniPath,
    const wchar_t* section,
    const wchar_t* key)
{
    std::array<wchar_t, 512> buffer = {};
    (void)GetPrivateProfileStringW(
        section,
        key,
        L"",
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        iniPath.c_str());
    return TrimAscii(WideToUtf8(std::wstring(buffer.data())));
}

bool IsValidStoredJoinAddress(const std::string& address)
{
    if (address.empty() || address.size() > 127)
    {
        return false;
    }

    for (char c : address)
    {
        const bool ok =
            std::isalnum(static_cast<unsigned char>(c)) != 0
            || c == '.'
            || c == ':'
            || c == '-'
            || c == '_'
            || c == '['
            || c == ']';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}
} // namespace

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
    *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY) = 0;
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

    // Resolve title_ob.dat - prefer mod folder override, fallback to vanilla.
    const std::string titleObjPath = ResolveTitleObjectsPath(g_moduleDirectory);
    const char* titleObjPathC = titleObjPath.c_str();
    const uint32_t oldBackgroundSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Background);
    const uint32_t oldObjectsSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Objects);
    ReleaseFrontendSurfaceSlots(screenContext, "title_assets");

    loadCompressedImageFile(
        GetGraphicsManager(screenContext),
        reinterpret_cast<uint32_t*>(screenContext + kOffsetBackgroundSurface),
        "system\\title.dat",
        0,
        1);
    loadCompressedImageFile(
        GetGraphicsManager(screenContext),
        reinterpret_cast<uint32_t*>(screenContext + kOffsetObjectsSurface),
        titleObjPathC,
        0,
        193);
    const uint32_t loadedTitleBackgroundSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Background);
    const uint32_t loadedTitleObjectsSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Objects);
    if (loadedTitleBackgroundSurface == 0 || loadedTitleObjectsSurface == 0)
    {
        mod::Log(
            "FRONTEND_SURFACE_LOAD_FAILED owner=title_assets bg=0x%08lX obj=0x%08lX",
            static_cast<unsigned long>(loadedTitleBackgroundSurface),
            static_cast<unsigned long>(loadedTitleObjectsSurface));
        return false;
    }

    const bool bgPaletteOk = loadBgrColorsFromRawFile(static_cast<int>(screenContext + kOffsetPalette), "system\\title.dat", 0, 1, 192) != 0;
    const bool objPaletteOk = loadBgrColorsFromRawFile(static_cast<int>(screenContext + kOffsetPalette), titleObjPathC, 0, 193, 48) != 0;

    *reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor) = static_cast<uint8_t>(readPixelValue(*reinterpret_cast<int*>(screenContext + kOffsetObjectsSurface)));
    setPalette(GetGraphicsContext(screenContext), static_cast<int>(screenContext + kOffsetPalette));

    const uint32_t newBackgroundSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Background);
    const uint32_t newObjectsSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Objects);
    mod::Log(
        "LoadTitleAssets: done bgPalette=%d objPalette=%d transparent=%u",
        bgPaletteOk,
        objPaletteOk,
        static_cast<unsigned>(*reinterpret_cast<uint8_t*>(screenContext + kOffsetTransparentColor)));
    mod::Log(
        "FRONTEND_SURFACE_OWNERSHIP_LOAD owner=title_assets bgOld=0x%08lX objOld=0x%08lX bgNew=0x%08lX objNew=0x%08lX paletteSet=1",
        static_cast<unsigned long>(oldBackgroundSurface),
        static_cast<unsigned long>(oldObjectsSurface),
        static_cast<unsigned long>(newBackgroundSurface),
        static_cast<unsigned long>(newObjectsSurface));
    return bgPaletteOk && objPaletteOk;
}

void LoadNetplayMenuSettingsFromIni()
{
    const std::wstring wideIniPath = ResolveRevivalIniPathWide();
    const std::string iniPath = WideToUtf8(wideIniPath).empty() ? "EfzRevival.ini" : WideToUtf8(wideIniPath);
    if (GetFileAttributesW(wideIniPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        mod::Log("LoadNetplayMenuSettingsFromIni: ini not found path='%s' (using in-memory defaults)", iniPath.c_str());
        return;
    }

    const std::string themeText = ReadIniValueUtf8(wideIniPath, L"NetplayMenu", L"Theme");
    if (!themeText.empty())
    {
        NetplayMenuTheme parsedTheme = g_netplayMenuState.theme;
        if (TryParseNetplayMenuTheme(themeText, &parsedTheme))
        {
            g_netplayMenuState.theme = parsedTheme;
            mod::Log(
                "LoadNetplayMenuSettingsFromIni: loaded NetplayMenu.Theme='%s'",
                NetplayMenuThemeToString(g_netplayMenuState.theme));
        }
        else
        {
            mod::Log(
                "LoadNetplayMenuSettingsFromIni: invalid NetplayMenu.Theme='%s' (keeping '%s')",
                themeText.c_str(),
                NetplayMenuThemeToString(g_netplayMenuState.theme));
        }
    }
    else
    {
        mod::Log(
            "LoadNetplayMenuSettingsFromIni: NetplayMenu.Theme missing/empty (keeping '%s')",
            NetplayMenuThemeToString(g_netplayMenuState.theme));
    }

    const std::string nickname = ReadIniValueUtf8(wideIniPath, L"Network", L"Name");
    if (!nickname.empty())
    {
        if (IsValidNickname(nickname))
        {
            g_netplayMenuState.nickname = nickname;
            g_netplayMenuState.nicknameSource = NetplayNicknameSource::LoadedFromIni;
            mod::Log(
                "LoadNetplayMenuSettingsFromIni: loaded Network.Name='%s' source=%s",
                g_netplayMenuState.nickname.c_str(),
                NetplayNicknameSourceToString(g_netplayMenuState.nicknameSource));
        }
        else
        {
            mod::Log(
                "LoadNetplayMenuSettingsFromIni: invalid Network.Name='%s' (keeping '%s' source=%s)",
                nickname.c_str(),
                g_netplayMenuState.nickname.c_str(),
                NetplayNicknameSourceToString(g_netplayMenuState.nicknameSource));
        }
    }
    else
    {
        mod::Log(
            "LoadNetplayMenuSettingsFromIni: Network.Name missing/empty (keeping '%s' source=%s)",
            g_netplayMenuState.nickname.c_str(),
            NetplayNicknameSourceToString(g_netplayMenuState.nicknameSource));
    }

    const std::string joinAddress = ReadIniValueUtf8(wideIniPath, L"Network", L"Address");
    if (!joinAddress.empty())
    {
        if (IsValidStoredJoinAddress(joinAddress))
        {
            g_netplayMenuState.joinAddress = joinAddress;
            mod::Log("LoadNetplayMenuSettingsFromIni: loaded Network.Address='%s'", g_netplayMenuState.joinAddress.c_str());
        }
        else
        {
            mod::Log(
                "LoadNetplayMenuSettingsFromIni: invalid Network.Address='%s' (keeping '%s')",
                joinAddress.c_str(),
                g_netplayMenuState.joinAddress.c_str());
        }
    }
    else
    {
        mod::Log(
            "LoadNetplayMenuSettingsFromIni: Network.Address missing/empty (keeping '%s')",
            g_netplayMenuState.joinAddress.c_str());
    }

    const std::string portText = ReadIniValueUtf8(wideIniPath, L"Network", L"Port");
    if (!portText.empty())
    {
        uint16_t parsedPort = 0;
        if (ParsePort(portText, &parsedPort))
        {
            g_netplayMenuState.hostPort = parsedPort;
            g_netplayMenuState.joinPort = parsedPort;
            mod::Log("LoadNetplayMenuSettingsFromIni: loaded Network.Port=%u", static_cast<unsigned>(parsedPort));
        }
        else
        {
            mod::Log(
                "LoadNetplayMenuSettingsFromIni: invalid Network.Port='%s' (keeping host=%u join=%u)",
                portText.c_str(),
                static_cast<unsigned>(g_netplayMenuState.hostPort),
                static_cast<unsigned>(g_netplayMenuState.joinPort));
        }
    }
    else
    {
        mod::Log(
            "LoadNetplayMenuSettingsFromIni: Network.Port missing/empty (keeping host=%u join=%u)",
            static_cast<unsigned>(g_netplayMenuState.hostPort),
            static_cast<unsigned>(g_netplayMenuState.joinPort));
    }
}

void SaveNetplayJoinAddressToIni()
{
    const std::wstring wideIniPath = ResolveRevivalIniPathWide();
    const std::string iniPath = WideToUtf8(wideIniPath).empty() ? "EfzRevival.ini" : WideToUtf8(wideIniPath);
    const std::string joinAddress = TrimAscii(g_netplayMenuState.joinAddress);
    if (joinAddress.empty())
    {
        mod::Log("SaveNetplayJoinAddressToIni: join address empty (skipping) path='%s'", iniPath.c_str());
        return;
    }

    if (!IsValidStoredJoinAddress(joinAddress))
    {
        mod::Log(
            "SaveNetplayJoinAddressToIni: invalid Network.Address='%s' (skipping write path='%s')",
            joinAddress.c_str(),
            iniPath.c_str());
        return;
    }

    const std::wstring wideJoinAddress = Utf8ToWide(joinAddress);
    if (WritePrivateProfileStringW(
            L"Network",
            L"Address",
            wideJoinAddress.c_str(),
            wideIniPath.c_str())
        == FALSE)
    {
        mod::Log(
            "SaveNetplayJoinAddressToIni: failed Network.Address='%s' path='%s' err=%lu",
            joinAddress.c_str(),
            iniPath.c_str(),
            static_cast<unsigned long>(GetLastError()));
        return;
    }

    mod::Log(
        "SaveNetplayJoinAddressToIni: wrote Network.Address='%s' path='%s'",
        joinAddress.c_str(),
        iniPath.c_str());
}

bool LoadNetplayAssets(uint32_t screenContext)
{
    mod::Log("LoadNetplayAssets: begin (screenContext=0x%08X)", screenContext);
    const std::string bgPath = ResolveNetplayBackgroundPath(g_moduleDirectory);
    if (bgPath.empty())
    {
        mod::Log("LoadNetplayAssets: netplay background not found near DLL assets folder");
        return false;
    }

    const std::string objPath = ResolveNetplayObjectsPath(g_moduleDirectory);
    if (!objPath.empty() && objPath != "system\\title_ob.dat" && !FileExists(objPath))
    {
        mod::Log("LoadNetplayAssets: objects path missing (%s), fallback to title objects", objPath.c_str());
    }

    const char* objectsPath = objPath.c_str();
    NetplayObjectProfile objectProfile = DetermineObjectProfile(objPath);
    g_netplayMenuState.useConfigStyleRender = objectProfile.useConfigStyleRender;
    int mainMenuCount = 0;
    (void)GetMenuEntries(NetplayMenuId::Main, &mainMenuCount);
    if (mainMenuCount <= 0)
    {
        mainMenuCount = kNetplayDefaultOptionCount;
    }
    g_netplayMenuState.optionCount = mainMenuCount;
    g_netplayMenuState.backIndex = mainMenuCount - 1;
    g_netplayMenuState.paletteStart = static_cast<uint8_t>(objectProfile.paletteDestStart);
    g_netplayMenuState.paletteCount = static_cast<uint8_t>(objectProfile.paletteCount);
    g_netplayMenuState.renderLayout = objectProfile.renderLayout;
    g_netplayMenuState.backgroundSupportsScroll = false;
    g_netplayMenuState.backgroundWidth = 320;
    g_netplayMenuState.backgroundHeight = 240;
    bool hasDerivedTransparentIndex = false;
    uint8_t derivedTransparentIndex = 0;

    ParsedDatImage backgroundImage = {};
    if (ParseEfzDatImage(bgPath, &backgroundImage))
    {
        g_netplayMenuState.backgroundWidth = backgroundImage.width;
        g_netplayMenuState.backgroundHeight = backgroundImage.height;
        g_netplayMenuState.backgroundSupportsScroll =
            backgroundImage.width == 320 && backgroundImage.height == 240;
    }
    else
    {
        mod::Log(
            "LoadNetplayAssets: failed to parse netplay background '%s' for theme validation",
            bgPath.c_str());
    }

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
    const uint32_t oldBackgroundSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Background);
    const uint32_t oldObjectsSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Objects);
    ReleaseFrontendSurfaceSlots(screenContext, "netplay_assets");

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
    const uint32_t loadedNetplayBackgroundSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Background);
    const uint32_t loadedNetplayObjectsSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Objects);
    if (loadedNetplayBackgroundSurface == 0 || loadedNetplayObjectsSurface == 0)
    {
        mod::Log(
            "FRONTEND_SURFACE_LOAD_FAILED owner=netplay_assets bg=0x%08lX obj=0x%08lX",
            static_cast<unsigned long>(loadedNetplayBackgroundSurface),
            static_cast<unsigned long>(loadedNetplayObjectsSurface));
        return false;
    }

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
    const uint32_t newBackgroundSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Background);
    const uint32_t newObjectsSurface =
        ReadFrontendSurfaceSlot(screenContext, FrontendSurfaceSlot::Objects);
    mod::Log(
        "FRONTEND_SURFACE_OWNERSHIP_LOAD owner=netplay_assets bgOld=0x%08lX objOld=0x%08lX bgNew=0x%08lX objNew=0x%08lX paletteSet=1 configStyle=%d",
        static_cast<unsigned long>(oldBackgroundSurface),
        static_cast<unsigned long>(oldObjectsSurface),
        static_cast<unsigned long>(newBackgroundSurface),
        static_cast<unsigned long>(newObjectsSurface),
        g_netplayMenuState.useConfigStyleRender ? 1 : 0);
    mod::Log(
        "LoadNetplayAssets: object profile colorOffset=%u paletteStart=%d paletteCount=%d configStyle=%d optionCount=%d backIndex=%d bgSize=%dx%d bgScrollSupported=%d",
        static_cast<unsigned>(objectProfile.colorOffset),
        objectProfile.paletteDestStart,
        objectProfile.paletteCount,
        g_netplayMenuState.useConfigStyleRender,
        g_netplayMenuState.optionCount,
        g_netplayMenuState.backIndex,
        g_netplayMenuState.backgroundWidth,
        g_netplayMenuState.backgroundHeight,
        g_netplayMenuState.backgroundSupportsScroll ? 1 : 0);

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

bool LoadNetplaySpriteFont()
{
    return netplay::fontmap::LoadNetplaySpriteFont(g_moduleDirectory, &g_spriteFont);
}
} // namespace netplay::hooks::internal
