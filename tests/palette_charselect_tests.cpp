#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <vector>
#include "../src/netplay/interop/palette_charselect.cpp"

namespace {
// Keep EFZ's fixed screen-index fixture inside this executable, away from randomized stacks.
unsigned char screenAddressReservation[0x400000] = {};
int writes = 0;
int restores = 0;
std::uint32_t portrait[2][netplay::interop::apply::kPortraitEntries] = {};
bool writeSucceeds = true;
bool frameHookInstalled = false;
int renders = 0;
int captures = 0;
bool restoreSucceeds = true;
bool loopbackEnabled = false;
int chainedFades = 0;
int __fastcall OtherModFade(void*, void*, int palette, unsigned char kind, int volume, int adjustment)
{
    ++chainedFades;
    return palette + kind + volume + adjustment;
}
int __fastcall NativeSelected(void*, void*, int color, int, char side)
{ portrait[static_cast<int>(side)][0] = static_cast<std::uint32_t>(color); return 31; }
int __fastcall NativeRender(void*, void*) { ++renders; return 73; }
int __fastcall NativeInit(void*, void*) { return 37; }
void Expect(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}
namespace mod {
std::vector<std::string> logLines;
void Log(const char* format, ...)
{
    char line[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    logLines.emplace_back(line);
}
}
namespace netplay::mod_settings {
bool AreOnlineCustomColorsEnabled() { return true; }
bool IsModInteropLoopbackEnabled() { return loopbackEnabled; }
}
namespace netplay::bridge {
NetbridgeStatus GetStatus() { NetbridgeStatus s{}; s.role = -1; s.roleFlag = 2; return s; }
}
namespace netplay::bridge::takeover {
bool HasNetplayPerFrameTickHookInstalled() { return frameHookInstalled; }
}
namespace netplay::interop::socket_transport {
bool SendSink(const std::uint8_t*, std::size_t, void*) { return true; }
std::size_t Poll(OverlayChannel&) { return 0; }
}
namespace netplay::interop::apply {
bool ApplyRowToCharSelectPortrait(std::uint32_t, int side, const protocol::PaletteBlobBody& row)
{
    ++writes;
    if (writeSucceeds) portrait[side][0] = 0xFF000000u | row.rawBgr[0];
    return writeSucceeds;
}
bool ApplyRowToWinScreen(std::uint32_t, int, const protocol::PaletteBlobBody&) { return true; }
bool CapturePortraitEntries(std::uint32_t, int side, std::uint32_t out[kPortraitEntries])
{ ++captures; std::memcpy(out, portrait[side], sizeof(portrait[side])); return true; }
bool RestorePortraitEntries(std::uint32_t, int side, const std::uint32_t entries[kPortraitEntries])
{ ++restores; if (!restoreSucceeds) return false; std::memcpy(portrait[side], entries, sizeof(portrait[side])); return true; }
}

namespace {
void TestExistingFadeHook()
{
    using namespace netplay::interop::charselect;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    const std::uintptr_t rvas[] = {kRvaReloadCharacterPalette, kRvaInitCharacterSelect,
        kRvaRenderCharacterSelect, kRvaInitializeSelectedCharacter, kRvaFadeWithSound,
        kRvaInitResultScreen};
    DWORD protections[6];
    const auto reservation = reinterpret_cast<std::uintptr_t>(screenAddressReservation);
    for (int i = 0; i < 6; ++i)
    {
        const auto address = base + rvas[i];
        Expect(address >= reservation && address + 16 <= reservation + sizeof(screenAddressReservation),
               "hook fixture stays inside reserved image memory");
        Expect(VirtualProtect(reinterpret_cast<void*>(address), 16, PAGE_EXECUTE_READWRITE,
                              &protections[i]) != FALSE, "hook fixture executable");
        const unsigned char prologue[] = {0x55, 0x8B, 0xEC, 0x51, 0x90, 0x90, 0xC9, 0xC3};
        std::memcpy(reinterpret_cast<void*>(address), prologue, sizeof(prologue));
    }
    const unsigned char render[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x20, 0x08, 0x00, 0x00};
    const unsigned char selected[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00};
    const unsigned char fade[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x89, 0x4D, 0xEC};
    std::memcpy(reinterpret_cast<void*>(base + kRvaRenderCharacterSelect), render, sizeof(render));
    std::memcpy(reinterpret_cast<void*>(base + kRvaInitializeSelectedCharacter), selected, sizeof(selected));
    auto* entry = reinterpret_cast<unsigned char*>(base + kRvaFadeWithSound);
    std::memcpy(entry, fade, sizeof(fade));
    entry[0] = 0xE9;
    auto jump = static_cast<std::int32_t>(reservation - reinterpret_cast<std::uintptr_t>(entry + 5));
    std::memcpy(entry + 1, &jump, sizeof(jump));
    loopbackEnabled = true;
    g_installed = false;
    g_csObj = 0;
    Expect(!Install(), "jump to non-executable data is rejected");
    jump = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&OtherModFade)
                                     - reinterpret_cast<std::uintptr_t>(entry + 5));
    std::memcpy(entry + 1, &jump, sizeof(jump));
    FlushInstructionCache(GetCurrentProcess(), entry, sizeof(fade));
    Expect(Install(), "existing fade detour does not disable palette hooks");
    auto callFade = reinterpret_cast<FadeWithSound_t>(entry);
    Expect(callFade(nullptr, 123, 1, 20, 50) == 194 && chainedFades == 1,
           "existing mod's fade receives original arguments exactly once");
    MH_Uninitialize();
    g_installed = false;
    g_minhookReady = false;
    loopbackEnabled = false;
    for (int i = 0; i < 6; ++i)
    {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(base + rvas[i]), 16, protections[i], &old);
    }
}

int __fastcall NativeFade(void* context, void*, int palette,
                          unsigned char kind, int volume, int adjustment)
{
    using namespace netplay::interop::charselect;
    Expect(palette == 123 && kind == 0 && volume == 20 && adjustment == 50,
           "native fade arguments preserved");
    Expect(g_lastCustom[0], "Arcade custom-confirm applied before fade begins");
    const int beforeWrites = writes, beforeRestores = restores, beforeCaptures = captures;
    auto* cs = static_cast<unsigned char*>(context);
    auto* gd = *reinterpret_cast<unsigned char**>(cs + 28);
    for (int frame = 0; frame < 21; ++frame)
    {
        gd[4920] = 0;
        HookRender(context, nullptr);
        TickGameThread();
        ProcessSide(reinterpret_cast<std::uint32_t>(context), 0, false);
    }
    gd[4920] = 1;
    Expect(writes == beforeWrites && restores == beforeRestores && captures == beforeCaptures,
           "native fade frames do no palette work even with pending state changes");
    return 57;
}
}

int main()
{
    using namespace netplay::interop;
    using namespace netplay::interop::charselect;
    char oldDir[MAX_PATH], tempDir[MAX_PATH], fixture[MAX_PATH];
    Expect(GetCurrentDirectoryA(MAX_PATH, oldDir) != 0, "current directory");
    Expect(GetTempPathA(MAX_PATH, tempDir) != 0, "temp directory");
    Expect(GetTempFileNameA(tempDir, "pal", 0, fixture) != 0, "unique fixture");
    const std::string fixtureDir = std::string(fixture) + ".dir";
    Expect(CreateDirectoryA(fixtureDir.c_str(), nullptr) != FALSE, "fixture directory");
    Expect(SetCurrentDirectoryA(fixtureDir.c_str()) != FALSE, "fixture working directory");
    Expect(CreateDirectoryA("ikumi", nullptr) != FALSE, "character folder");
    unsigned char pal[121] = {40, 10, 20, 30};
    FILE* f = std::fopen("ikumi/ikumi1.pal", "wb");
    Expect(f != nullptr, "create palette");
    Expect(std::fwrite(pal, 1, sizeof(pal), f) == sizeof(pal), "write palette");
    std::fclose(f);

    unsigned char cs[1400] = {};
    unsigned char gd[5000] = {};
    *reinterpret_cast<std::uint32_t*>(cs + 28) = reinterpret_cast<std::uint32_t>(gd);
    cs[1340] = 14;
    cs[1182] = 3;
    cs[1183] = 3;
    gd[4920] = 1;
    const auto obj = reinterpret_cast<std::uint32_t>(cs);
    g_sideSource = SideSource::Offline;
    g_localSide = 0;
    g_stockValid[0] = false;
    portrait[0][0] = 0x123456u;
    OverlayChannel::Instance().Begin(0);
    ProcessSide(obj, 0, false);
    Expect(writes == 1 && g_matchRow[0].rawBgr[0] == 10, "first custom palette applied");
    Expect(g_stockValid[0], "stock captured before first custom write");
    bool loggedLoad = false;
    for (const auto& line : mod::logLines)
        if (line.find("PaletteLoad:") == 0 && line.find("ikumi1.pal") != std::string::npos)
            loggedLoad = true;
    Expect(loggedLoad, "actual file load records its path");
    const auto logsOnApply = mod::logLines.size();
    const int capturedOnApply = captures;
    for (int i = 0; i < 120; ++i) ProcessSide(obj, 0, false);
    Expect(mod::logLines.size() == logsOnApply, "unchanged frames do not repeat palette logs");
    Expect(captures == capturedOnApply, "unchanged frames do not scan portrait banks");
    Expect(writes == 1, "unchanged frames must not rewrite the hardware palette");
    portrait[0][0] = 0x123456u;
    ProcessSide(obj, 0, true);
    Expect(writes == 2, "native reload must reapply unchanged custom colors");

    DeleteFileA("ikumi/ikumi1.pal");
    Sleep(300);
    ProcessSide(obj, 0, false);
    Expect(g_matchRow[0].slotByte != 0 && writes == 2,
           "stable selection uses cached file without timed disk rereads");
    gd[4920] = 0;
    ProcessSide(obj, 0, false);
    ProcessSide(obj, 0, false);
    Expect(restores == 1 && portrait[0][0] == 0x123456u,
           "EDIT COLOR off restores selected character stock exactly once");
    gd[4920] = 1;
    writeSucceeds = false;
    ProcessSide(obj, 0, false);
    Expect(!g_lastCustom[0], "failed palette write must not be cached as applied");
    writeSucceeds = true;
    ProcessSide(obj, 0, false);
    Expect(g_lastCustom[0], "failed write is retried");
    const int beforeNativeReload = writes;
    g_csObj = obj;
    g_origSelected = reinterpret_cast<InitializeSelected_t>(&NativeSelected);
    Expect(HookSelected(cs, nullptr, 0x654321, 0, 0) == 31,
           "native selected-character load return preserved");
    ProcessSide(obj, 0, false);
    Expect(writes == beforeNativeReload + 1 && portrait[0][0] == 0xFF00000Au,
           "native portrait initialization without HookReload reapplies custom colors");
    gd[4920] = 0;
    ProcessSide(obj, 0, false);
    Expect(portrait[0][0] == 0x654321u, "revert uses newly loaded native portrait");
    gd[4920] = 1;
    ProcessSide(obj, 0, false);

    portrait[0][0] = 0xABCDEFu;
    gd[4920] = 0;
    ProcessSide(obj, 0, false);
    Expect(portrait[0][0] == 0xABCDEFu, "native stock replacement must not be overwritten on EDIT COLOR off");
    gd[4920] = 1;
    ProcessSide(obj, 0, false);
    gd[4920] = 0;
    ProcessSide(obj, 0, false);
    Expect(portrait[0][0] == 0xABCDEFu, "next custom toggle captures latest stock");
    gd[4920] = 1;
    ProcessSide(obj, 0, false);

    cs[1183] = 4;
    gd[4920] = 0;
    ProcessSide(obj, 0, false);
    Expect(portrait[0][0] == 0xABCDEFu,
           "releasing EDIT COLOR restores P1 even while CPU/P2 is in state 4");
    gd[4920] = 1;
    ProcessSide(obj, 0, false);
    Expect(portrait[0][0] == 0xFF00000Au, "Arcade CPU state does not block human palette");
    cs[1182] = 7;
    cs[1183] = 7;
    ProcessSide(obj, 0, false);
    Expect(g_lastCustom[0], "stage transition preserves outstanding custom restore state");
    cs[1182] = 3;
    cs[1183] = 3;
    gd[4920] = 0;
    ProcessSide(obj, 0, false);
    Expect(portrait[0][0] == 0xABCDEFu, "return from stage restores released custom colors");
    gd[4920] = 1;
    ProcessSide(obj, 0, false);

    const auto reservedStart = reinterpret_cast<std::uintptr_t>(screenAddressReservation);
    Expect(kVaCurrentScreenIndex >= reservedStart
           && kVaCurrentScreenIndex + sizeof(int) <= reservedStart + sizeof(screenAddressReservation),
           "game screen-index fixture lies inside reserved image data");
    auto* screen = reinterpret_cast<int*>(kVaCurrentScreenIndex);
    *screen = 1;
    g_installed = true;
    g_csObj = obj;
    g_origRender = reinterpret_cast<InitCharSelect_t>(&NativeRender);
    g_lastCustom[0] = false;
    int beforeRender = writes;
    Expect(HookRender(cs, nullptr) == 73, "native render return preserved");
    Expect(writes == beforeRender + 1 && renders == 1,
           "Practice without a frame hook applies colors and renders once");
    HookRender(cs, nullptr);
    Expect(writes == beforeRender + 1 && renders == 2,
           "steady Practice rendering does not rewrite palettes");
    frameHookInstalled = true;
    g_lastCustom[0] = false;
    HookRender(cs, nullptr);
    Expect(writes == beforeRender + 1, "managed frame hook owns palette tick exclusively");
    TickGameThread();
    HookRender(cs, nullptr);
    Expect(writes == beforeRender + 2, "managed frame and render do not double-apply");
    frameHookInstalled = false;
    *screen = 3;
    g_lastCustom[0] = false;
    HookRender(cs, nullptr);
    Expect(writes == beforeRender + 2, "battle cannot reach palette writes");
    *screen = 1;
    gd[4920] = 0;
    ProcessSide(obj, 0, false);
    cs[1182] = 6;
    cs[1183] = 4;
    gd[4920] = 1;
    g_origFade = reinterpret_cast<FadeWithSound_t>(&NativeFade);
    const int beforeFade = writes;
    const int beforeFadeRenders = renders;
    Expect(HookFade(cs, nullptr, 123, 0, 20, 50) == 57, "native fade return preserved");
    Expect(writes == beforeFade + 1 && renders == beforeFadeRenders + 21,
           "Arcade confirm applies once then native fade renders normally");
    Expect(!g_fading, "fade gate released after native fade");
    cs[1182] = 3;
    cs[1183] = 3;

    gd[4920] = 1;
    ProcessSide(obj, 0, false);
    const int beforeFailedRestore = restores;
    gd[4920] = 0;
    restoreSucceeds = false;
    ProcessSide(obj, 0, false);
    Expect(g_lastCustom[0] && g_stockValid[0], "failed restore retains recovery state");
    restoreSucceeds = true;
    ProcessSide(obj, 0, false);
    Expect(!g_lastCustom[0] && restores == beforeFailedRestore + 2,
           "restore retries after temporary failure");
    gd[4920] = 1;
    ProcessSide(obj, 0, false);

    const int before = writes;
    cs[1182] = 7;
    ProcessSide(obj, 0, false);
    cs[1182] = 3;
    cs[45] = 1;
    ProcessSide(obj, 0, false);
    Expect(writes == before, "stage selection and leaving screen do not write palettes");
    cs[45] = 0;
    cs[1342] = 1;
    const auto beforeMissingLogs = mod::logLines.size();
    ProcessSide(obj, 0, false);
    Expect(g_matchRow[0].slotByte == 0, "missing new slot falls back to stock");
    bool loggedMissing = false;
    for (std::size_t i = beforeMissingLogs; i < mod::logLines.size(); ++i)
        if (mod::logLines[i].find("ikumi2.pal") != std::string::npos
            && mod::logLines[i].find("openError=2") != std::string::npos)
            loggedMissing = true;
    Expect(loggedMissing, "missing file reports attempted path and Windows error");
    const auto afterMissingLogs = mod::logLines.size();
    for (int i = 0; i < 120; ++i) ProcessSide(obj, 0, false);
    Expect(mod::logLines.size() == afterMissingLogs, "cached failure does not spam logs");

    g_origInit = reinterpret_cast<InitCharSelect_t>(&NativeInit);
    g_transport = Transport::Piggyback;
    Expect(HookInit(cs, nullptr) == 37, "native init return preserved");
    Expect(!g_stockValid[0] && !g_stockValid[1], "init cannot capture portraits not yet loaded");
    Expect(!g_localRowCache[0].valid && !g_localRowCache[1].valid,
           "reentering character select invalidates file cache");
    RemoveDirectoryA("ikumi");
    SetCurrentDirectoryA(oldDir);
    RemoveDirectoryA(fixtureDir.c_str());
    DeleteFileA(fixture);
    TestExistingFadeHook();
    std::puts("palette character-select tests passed");
}
