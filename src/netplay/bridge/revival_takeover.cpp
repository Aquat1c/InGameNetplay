// Session lifecycle: host/join/spectate management, tick loop, cancel, delay.
// Internal helpers live in sister .cpp files; see takeover_internal.h.

#include <winsock2.h>
#include <ws2tcpip.h>

#include "netplay/bridge/revival_takeover.h"
#include "netplay/bridge/console_handoff_policy.h"
#include "netplay/bridge/external_launcher_guard.h"
#include "netplay/bridge/gameplay_exit_recovery.h"
#include "netplay/bridge/session_lifecycle.h"
#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/options_menu.h"

#include "crash_handler.h"
#include "logger.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include <windows.h>

extern "C" __declspec(dllexport) DWORD WINAPI nb_stub_RequestPeerQuitBroadcast(
    LPVOID);

namespace netplay::bridge::takeover
{

// ---------------------------------------------------------------------------
// Global variable definitions (declared extern in takeover_internal.h).
// ---------------------------------------------------------------------------

std::mutex g_mutex;
const RevivalAddressProfile* g_activeRevival = &kRevival_1_02e;

namespace
{
enum class RevivalProfileSource : uint8_t
{
    Default = 0,
    DllTimestamp,
    PublishedHostTimestamp,
    ExternalLauncherGuard,
};

RevivalProfileSource g_activeRevivalSource = RevivalProfileSource::Default;

bool ShouldCancelToIdle(const char* reason)
{
    return reason != nullptr
        && (std::strcmp(reason, "user_cancel") == 0
            || std::strcmp(reason, "leave_menu") == 0
            || std::strcmp(reason, "external_cancel") == 0
            || std::strcmp(reason, "dismissed_error") == 0
            || std::strcmp(reason, "no_overlay_session_ended") == 0);
}

const char* RevivalProfileSourceToString(RevivalProfileSource source)
{
    switch (source)
    {
    case RevivalProfileSource::DllTimestamp:
        return "dll_timestamp";
    case RevivalProfileSource::PublishedHostTimestamp:
        return "published_host_timestamp";
    case RevivalProfileSource::ExternalLauncherGuard:
        return "external_launcher_guard";
    case RevivalProfileSource::Default:
    default:
        return "default";
    }
}

bool IsActiveRevival102jProfile()
{
    return g_activeRevival != nullptr
        && g_activeRevival->versionTag != nullptr
        && std::strcmp(g_activeRevival->versionTag, "1.02j") == 0;
}
} // namespace

const char* RevivalWireName(const char* legacyName)
{
    if (legacyName == nullptr || !IsActiveRevival102jProfile())
    {
        return legacyName;
    }

    struct WireNamePair
    {
        const char* legacy;
        const char* revival102j;
    };
    static const WireNamePair kWireNames[] = {
        {"Init",      "Init_Spec"},
        {"Net",       "Net_Spec"},
        {"Sync",      "Sync_Spec"},
        {"Quit",      "Quit_Spec"},
        {"LoadMatch", "LoadMatch_Spec"},
        {"InputP1",   "InputP1_Spec"},
        {"InputP2",   "InputP2_Spec"},
        {"PaletteP1", "PaletteP1_Spec"},
        {"PaletteP2", "PaletteP2_Spec"},
    };

    for (const auto& candidate : kWireNames)
    {
        if (std::strcmp(legacyName, candidate.legacy) == 0)
        {
            return candidate.revival102j;
        }
    }
    return legacyName;
}

HMODULE g_localRevivalModule = nullptr;
RevivalInitFn g_localInitFn = nullptr;
HANDLE g_revivalProcess = nullptr;
DWORD g_revivalProcessId = 0;
PeerProcessOwnership g_peerProcessOwnership = PeerProcessOwnership::None;
revival_launch::LaunchDisposition g_launchDisposition =
    revival_launch::LaunchDisposition::DirectGameHost;
int g_localRoleFlag = -1;
int g_netplayRole = kNetplayRoleNone;
uintptr_t g_hostRevivalBase = 0;

HANDLE g_hostMapHandle = nullptr;
SharedBlock* g_hostBlock = nullptr;
HANDLE g_hostInitEvent = nullptr;
HANDLE g_hostConsoleEvent = nullptr;

bool g_injectedReady = false;
HANDLE g_injectedMapHandle = nullptr;
SharedBlock* g_injectedBlock = nullptr;
HANDLE g_injectedInitEvent = nullptr;
HANDLE g_injectedConsoleEvent = nullptr;
volatile LONG g_injectedLastConsoleSerialServed = 0;
volatile LONG g_injectedLastConsoleAuxSerialServed = 0;
volatile LONG g_injectedActiveConsoleAuxSerial = 0;
volatile LONG g_injectedConsoleAuxScriptOffset = 0;
volatile LONG g_injectedAutoConsoleFallbackCount = 0;
volatile LONG g_injectedConsoleOutputHits = 0;
volatile LONG g_injectedTerminateUnknownPidHits = 0;
volatile LONG g_injectedDelayPromptSerial = 0;
volatile LONG g_injectedDelayPromptServedSerial = 0;
volatile LONG g_injectedConnectedFromDelayPromptSerial = 0;
volatile LONG g_injectedSpectateConfirmPromptSerial = 0;
volatile LONG g_injectedSpectateConfirmPromptServedSerial = 0;
DWORD g_injectedSpectateConfirmPromptWaitStartTick = 0;
volatile LONG g_injectedFingerprintReadHits = 0;
uintptr_t g_injectedInitAddress = 0;
bool g_injectedLazyBound = false;
volatile LONG g_injectedLazyBootstrapState = 0;
volatile LONG g_remoteThreadCallIndex = 0;
volatile LONG g_startAbortRequested = 0;
// Successful explicit Quit sends are coalesced per helper process. Character
// select and Quit-ring recovery can observe the same local exit on adjacent
// frames; the remote peer must receive one native MessageQuit, not a burst.
static volatile LONG g_peerQuitBroadcastHelperPid = 0;
static volatile LONG g_peerQuitBroadcastState = 0; // 0=idle, 1=in flight, 2=sent
HANDLE g_fakeProcessThreadHandle = nullptr;
bool g_initCapturedFromWrite = false;
DWORD g_lastConnectingDiagnosticTick = 0;
uintptr_t g_lastSessionPtrOffset = 0;
uintptr_t g_lastValidatedSessionPtr = 0;
DWORD g_lastSessionPointerMismatchTick = 0;
DWORD g_lastRuntimeReadyProbeLogTick = 0;
uint32_t g_lastRuntimeReadyProbeMask = 0;
bool g_lastRuntimeReadyProbeMaskValid = false;
static LONG g_lastNativeDelayTimeoutTraceSerial = 0;
static DWORD g_lastNativeDelayTimeoutTraceTick = 0;
DWORD g_lastSpectateConsoleSnapshotTick = 0;
bool g_localInitAppliedForSession = false;
bool g_spectatorPostInitAttemptedForSession = false;
bool g_spectatorPostInitSucceededForSession = false;
volatile LONG g_deferredLifecycleWorkRequested = 0;
volatile LONG g_deferredTitleSelection = -1;
uintptr_t g_remoteInjectedSelfBase = 0;
uintptr_t g_externalLauncherGuardRemoteBase = 0;
std::unordered_map<std::string, uint32_t> g_remoteInjectedPatchMap;
DWORD g_lastLatePatchRetryTick = 0;
DWORD g_lastLatePatchRetryLogTick = 0;
DWORD g_latePatchRetryAttempts = 0;
DWORD g_latePatchRetrySuccesses = 0;
bool g_lastLatePatchRetryResultValid = false;
bool g_lastLatePatchRetryResult = false;
bool g_observedTakeoverCreatePath = false;
bool g_tournamentReturnCleanupPending = false;
volatile LONG g_externalTournamentInitialTitleLeft = 0;
static volatile LONG g_tournamentReturnCleanupStage = 0;
static volatile LONG g_tournamentReturnCleanupTerminalFailure = 0;
std::mutex g_fakeThreadMutex;
std::vector<FakeThreadInfo> g_fakeThreads;
std::mutex g_redirectAllocMutex;
std::vector<RedirectAllocationInfo> g_redirectAllocations;
volatile LONG g_redirectWriteBlockedHits = 0;
volatile LONG g_sessionHistoryRepairHits = 0;
std::mutex g_consoleLogMutex;
std::string g_consolePendingWriteFile;
std::string g_consolePendingWriteFileDisk;
std::string g_consolePendingWriteConsoleA;
std::string g_consolePendingWriteConsoleW;
std::string g_consolePendingWriteConsoleOutputCharacterA;
std::string g_consolePendingWriteConsoleOutputCharacterW;
std::string g_consolePendingOutputDebugStringA;
std::string g_consolePendingOutputDebugStringW;
bool g_captureRevivalNativeLogsConfigured = false;
bool g_captureRevivalNativeLogs = false;
bool g_revivalErrorCodeNullGuardPatched = false;
uintptr_t g_revivalErrorCodeNullGuardPatchedBase = 0;
void* g_revivalErrorCodeNullGuardStub = nullptr;
DelayPromptMetrics g_delayPromptMetrics = {};
volatile LONG g_revivalExitIntercepted = 0;
volatile LONG g_revivalExitMode = -1;
bool g_nativeWorkflowLoadedSeen = false;
bool g_nativeWorkflowMatchLoopSeen = false;
bool g_nativeWorkflowTournamentSeen = false;
bool g_nativeWorkflowPeerDiedSeen = false;
bool g_nativeWorkflowHolePunchDiedSeen = false;
static uintptr_t g_injectedPeerManagerCachePtr = 0;
static DWORD g_injectedPeerManagerCacheHostPid = 0;
static bool g_injectedPeerManagerCacheStrictHostMatch = false;
static DWORD g_injectedPeerQuitLastDetail = 0;
bool g_holePunchServerConfigLoaded = false;
std::string g_configuredHolePunchServer;

// Job object for automatic child-process cleanup.  When the host process
// terminates (even by crash), the kernel closes all handles to the job,
// which kills every process assigned to it - ensuring EfzRevival.exe and
// any grandchildren (cmd.exe / conhost.exe) never linger in the background.
static HANDLE g_childJobObject = nullptr;

namespace
{

std::string TrimAsciiCopy(const std::string& text)
{
    size_t start = 0;
    while (start < text.size()
        && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n'))
    {
        ++start;
    }
    size_t end = text.size();
    while (end > start
        && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }
    return text.substr(start, end - start);
}

void AppendConsolePreview(std::string* out, const char* tag, const std::string& text)
{
    if (out == nullptr || tag == nullptr)
    {
        return;
    }

    const std::string trimmed = TrimAsciiCopy(text);
    if (trimmed.empty())
    {
        return;
    }

    if (!out->empty())
    {
        out->append(" | ");
    }
    out->append(tag);
    out->push_back('=');
    out->push_back('\'');
    constexpr size_t kMaxPreviewLen = 80;
    if (trimmed.size() > kMaxPreviewLen)
    {
        out->append(trimmed.substr(0, kMaxPreviewLen));
        out->append("...");
    }
    else
    {
        out->append(trimmed);
    }
    out->push_back('\'');
}

void LogPendingSpectateConsoleSnapshot()
{
    std::string summary;
    {
        std::lock_guard<std::mutex> lock(g_consoleLogMutex);
        AppendConsolePreview(&summary, "WriteFile", g_consolePendingWriteFile);
        AppendConsolePreview(&summary, "WriteFileDisk", g_consolePendingWriteFileDisk);
        AppendConsolePreview(&summary, "WriteConsoleA", g_consolePendingWriteConsoleA);
        AppendConsolePreview(&summary, "WriteConsoleW", g_consolePendingWriteConsoleW);
        AppendConsolePreview(&summary, "WriteConsoleOutputCharacterA", g_consolePendingWriteConsoleOutputCharacterA);
        AppendConsolePreview(&summary, "WriteConsoleOutputCharacterW", g_consolePendingWriteConsoleOutputCharacterW);
        AppendConsolePreview(&summary, "OutputDebugStringA", g_consolePendingOutputDebugStringA);
        AppendConsolePreview(&summary, "OutputDebugStringW", g_consolePendingOutputDebugStringW);
    }

    if (!summary.empty())
    {
        mod::Log("Takeover: spectate console snapshot %s", summary.c_str());
    }
}

} // namespace

void EnsureHostLogEfzIatPatched(bool verboseLogs)
{
    (void)verboseLogs;
    mod::Log(
        "Takeover: host logEfz IAT capture disabled (simulation parity); "
        "helper-process console capture remains active");
}

// ---------------------------------------------------------------------------
// Version detection
// ---------------------------------------------------------------------------

static const RevivalAddressProfile* FindRevivalProfileByTimestamp(uint32_t timestamp)
{
    for (size_t i = 0; i < kRevivalProfileCount; ++i)
    {
        if (kAllRevivalProfiles[i]->peTimestamp == timestamp)
        {
            return kAllRevivalProfiles[i];
        }
    }
    return nullptr;
}

static bool ModuleBytesMatch(
    HMODULE module,
    uintptr_t rva,
    const uint8_t* expected,
    size_t expectedSize)
{
    if (module == nullptr || expected == nullptr || expectedSize == 0)
    {
        return false;
    }

    __try
    {
        const auto* const bytes = reinterpret_cast<const uint8_t*>(module) + rva;
        return std::memcmp(bytes, expected, expectedSize) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool ModuleEntryLooksLikeInstalledJmp(HMODULE module, uintptr_t rva, size_t patchSize)
{
    if (module == nullptr || patchSize < 5)
    {
        return false;
    }

    __try
    {
        const auto* const bytes = reinterpret_cast<const uint8_t*>(module) + rva;
        if (bytes[0] != 0xE9)
        {
            return false;
        }
        for (size_t i = 5; i < patchSize; ++i)
        {
            if (bytes[i] != 0x90)
            {
                return false;
            }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool ModuleBytesMatchOrInstalledJmp(
    HMODULE module,
    uintptr_t rva,
    const uint8_t* expected,
    size_t expectedSize,
    size_t patchSize)
{
    return ModuleBytesMatch(module, rva, expected, expectedSize)
        || ModuleEntryLooksLikeInstalledJmp(module, rva, patchSize);
}

static bool ModuleBytesMatchMasked(
    HMODULE module,
    uintptr_t rva,
    const uint8_t* expected,
    const char* mask,
    size_t expectedSize)
{
    if (module == nullptr || expected == nullptr || mask == nullptr || expectedSize == 0)
    {
        return false;
    }

    __try
    {
        const auto* const bytes = reinterpret_cast<const uint8_t*>(module) + rva;
        for (size_t i = 0; i < expectedSize; ++i)
        {
            if (mask[i] == 'x' && bytes[i] != expected[i])
            {
                return false;
            }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool ModuleWindowContainsU32(
    HMODULE module,
    uintptr_t rva,
    size_t size,
    uint32_t value)
{
    if (module == nullptr || size < sizeof(uint32_t))
    {
        return false;
    }

    __try
    {
        const auto* const bytes = reinterpret_cast<const uint8_t*>(module) + rva;
        for (size_t i = 0; i + sizeof(uint32_t) <= size; ++i)
        {
            uint32_t current = 0;
            std::memcpy(&current, bytes + i, sizeof(current));
            if (current == value)
            {
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return false;
}

static bool ModuleDirectCallCountEquals(
    HMODULE module,
    uintptr_t windowRva,
    size_t windowSize,
    uintptr_t targetRva,
    size_t expectedCount)
{
    if (module == nullptr || windowSize < 5)
    {
        return false;
    }

    size_t count = 0;
    __try
    {
        const auto* const bytes =
            reinterpret_cast<const uint8_t*>(module) + windowRva;
        for (size_t i = 0; i + 5 <= windowSize; ++i)
        {
            if (bytes[i] != 0xE8)
            {
                continue;
            }

            int32_t disp = 0;
            std::memcpy(&disp, bytes + i + 1, sizeof(disp));
            const uintptr_t callTarget = windowRva + i + 5 + disp;
            if (callTarget == targetRva)
            {
                ++count;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return count == expectedCount;
}

static bool ModuleVtableSlotsMatch(
    HMODULE module,
    uintptr_t vtableRva,
    const uintptr_t* expectedTargetRvas,
    size_t expectedCount)
{
    if (module == nullptr || expectedTargetRvas == nullptr || expectedCount == 0)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    __try
    {
        const auto* const slots =
            reinterpret_cast<const uintptr_t*>(base + vtableRva);
        for (size_t i = 0; i < expectedCount; ++i)
        {
            if (slots[i] != base + expectedTargetRvas[i])
            {
                return false;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

static bool SectionNameEquals(const IMAGE_SECTION_HEADER& section, const char* expected)
{
    char name[9] = {};
    std::memcpy(name, section.Name, 8);
    return std::strcmp(name, expected) == 0;
}

static bool ModuleRvaInNamedSection(
    const IMAGE_NT_HEADERS32* nt,
    const char* sectionName,
    uintptr_t rva,
    size_t size)
{
    if (nt == nullptr || sectionName == nullptr || size == 0)
    {
        return false;
    }

    const IMAGE_SECTION_HEADER* const sections = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        const IMAGE_SECTION_HEADER& section = sections[i];
        if (!SectionNameEquals(section, sectionName))
        {
            continue;
        }

        const uintptr_t sectionStart = section.VirtualAddress;
        const uintptr_t rawSectionEnd =
            sectionStart + std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        const uintptr_t sectionEnd = (rawSectionEnd > sectionStart) ? rawSectionEnd : sectionStart;
        const uintptr_t rvaEnd = rva + size;
        return rva >= sectionStart && rvaEnd >= rva && rvaEnd <= sectionEnd;
    }

    return false;
}

static bool TryGetModuleNtHeaders32(HMODULE module, const IMAGE_NT_HEADERS32** outNt)
{
    if (outNt != nullptr)
    {
        *outNt = nullptr;
    }
    if (module == nullptr)
    {
        return false;
    }

    __try
    {
        const auto* const base = reinterpret_cast<const uint8_t*>(module);
        const auto* const dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 0x1000)
        {
            return false;
        }

        const auto* const nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            return false;
        }

        if (outNt != nullptr)
        {
            *outNt = nt;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool VerifyRevival102jBinary(HMODULE module)
{
    const IMAGE_NT_HEADERS32* nt = nullptr;
    if (!TryGetModuleNtHeaders32(module, &nt))
    {
        mod::Log("DetectRevivalVersion: 1.02j binary verification failed (bad PE32 header)");
        return false;
    }

    bool ok = true;
    auto require = [&ok](bool condition, const char* label) {
        if (!condition)
        {
            mod::Log("DetectRevivalVersion: 1.02j binary verification failed (%s)", label);
            ok = false;
        }
    };

    require(nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386, "machine");
    require(nt->FileHeader.NumberOfSections == 9, "section_count");
    require(nt->OptionalHeader.SizeOfImage == 0x001D9000u, "size_of_image");
    require(nt->OptionalHeader.AddressOfEntryPoint == 0x000011F0u, "entry_point");
    if (nt->OptionalHeader.ImageBase != kRevival_1_02j.defaultImageBase)
    {
        mod::Log(
            "DetectRevivalVersion: 1.02j PE image base relocated in memory "
            "header=0x%08lX expectedPreferred=0x%08lX loadedBase=%p",
            static_cast<unsigned long>(nt->OptionalHeader.ImageBase),
            static_cast<unsigned long>(kRevival_1_02j.defaultImageBase),
            static_cast<void*>(module));
    }

    require(ModuleRvaInNamedSection(nt, ".text", kRevival_1_02j.frameHookRva, 14), "frameHook section");
    require(ModuleRvaInNamedSection(nt, ".text", kRevival_1_02j.perFrameTickRva, 20), "perFrameTick section");
    require(ModuleRvaInNamedSection(nt, ".text", kRevival_1_02j.inputSwapPairRva, 24), "inputSwap section");
    require(ModuleRvaInNamedSection(nt, ".text", kRevival_1_02j.startInitPlayerRva, 20), "startInit section");
    require(ModuleRvaInNamedSection(nt, ".text", kRevival_1_02j.clearTextRva, 8), "clearText section");
    require(ModuleRvaInNamedSection(nt, ".text", kRevival_1_02j.setTextEnabledRva, 17), "setText section");
    require(ModuleRvaInNamedSection(nt, ".data", kRevival_1_02j.sessionPtrOffsets[0], sizeof(uintptr_t)), "sessionPtr section");
    require(ModuleRvaInNamedSection(nt, ".data", kRevival_1_02j.roleFlagOffsets[0], sizeof(int)), "roleFlag section");
    require(ModuleRvaInNamedSection(nt, ".data", kRevival_1_02j.globalStatePtrOffset, sizeof(uintptr_t)), "globalState section");
    require(ModuleRvaInNamedSection(nt, ".data", kRevival_1_02j.initFlagOffset, sizeof(int)), "initFlag section");
    require(ModuleRvaInNamedSection(nt, ".data", kRevival_1_02j.initByteOffset, sizeof(uint8_t)), "initByte section");
    require(ModuleRvaInNamedSection(nt, ".data", kRevival_1_02j.timerPtrOffset, sizeof(uintptr_t)), "timerPtr section");
    require(ModuleRvaInNamedSection(nt, ".data", kRevival_1_02j.renderContextBaseOffset, sizeof(uintptr_t)), "renderContextBase section");
    require(ModuleRvaInNamedSection(nt, ".data", kRevival_1_02j.renderContextGlobalOffset, sizeof(uintptr_t)), "renderContextGlobal section");

    const uintptr_t loadedBase = reinterpret_cast<uintptr_t>(module);
    require(ModuleWindowContainsU32(
        module,
        0x0012E2B0u,
        0x2200u,
        static_cast<uint32_t>(loadedBase + kRevival_1_02j.roleFlagOffsets[0])),
        "init roleFlag ref");
    require(ModuleWindowContainsU32(
        module,
        0x0012E2B0u,
        0x2200u,
        static_cast<uint32_t>(loadedBase + kRevival_1_02j.sessionPtrOffsets[0])),
        "init sessionPtr ref");
    require(ModuleWindowContainsU32(
        module,
        0x0012E2B0u,
        0x2200u,
        static_cast<uint32_t>(loadedBase + kRevival_1_02j.initFlagOffset)),
        "init initFlag ref");
    require(ModuleWindowContainsU32(
        module,
        0x0012E2B0u,
        0x2200u,
        static_cast<uint32_t>(loadedBase + kRevival_1_02j.initByteOffset)),
        "init initByte ref");
    require(ModuleWindowContainsU32(
        module,
        0x0012E2B0u,
        0x2200u,
        0x00401582u),
        "init frame EXE hook addr");
    require(ModuleWindowContainsU32(
        module,
        0x0012E2B0u,
        0x2200u,
        static_cast<uint32_t>(loadedBase + kRevival_1_02j.frameHookRva)),
        "init frame hook target");
    require(!ModuleWindowContainsU32(
        module,
        0x0012E2B0u,
        0x2200u,
        0x00401642u),
        "init must not claim persistent 0x401642 hook");

    static const uint8_t kFrameHookPrefix[] = {
        0x55, 0x89, 0xE5, 0x57, 0x56, 0x53, 0x31, 0xDB,
        0x81, 0xEC, 0xF0, 0x02, 0x00, 0x00,
    };
    static const uint8_t kPerFrameTickPrefix[] = {
        0x83, 0xEC, 0x28, 0xE8, 0x48, 0xE0, 0xFF, 0xFF,
        0x8D, 0x44, 0x24, 0x0C, 0xC7, 0x44, 0x24, 0x0C,
        0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t kInputSwapPrefix[] = {
        0xA1, 0x00, 0x00, 0x00, 0x00, 0xA8, 0x1F, 0x74,
        0x0F, 0x83, 0xC0, 0x01, 0xA3, 0x00, 0x00, 0x00,
        0x00, 0xC3, 0x8D, 0xB6, 0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t kStartInitPrefix[] = {
        0x55, 0x89, 0xE5, 0x57, 0x56, 0x8D, 0x85, 0xF0,
        0xFE, 0xFF, 0xFF, 0x53, 0x89, 0xCB, 0x81, 0xEC,
        0x38, 0x02, 0x00, 0x00,
    };
    static const uint8_t kSetTextPrefix[] = {
        0x8B, 0x49, 0x18, 0x0F, 0xB6, 0x44, 0x24, 0x04,
        0x89, 0x44, 0x24, 0x04, 0xE9, 0x2F, 0x96, 0x00,
        0x00,
    };
    static const uint8_t kClearTextPrefix[] = {
        0x8B, 0x49, 0x18, 0xE9, 0xF8, 0x94, 0x00, 0x00,
    };
    static const uint8_t kTournamentExitGuardPrefix[] = {
        0x74, 0x7A,
    };
    static const uint8_t kTournamentExitGuardPatched[] = {
        0x90, 0x90,
    };
    static const uint8_t kSpectatorStoreHelperHandle[] = {
        0x89, 0x43, 0x64,
    };
    static const uint8_t kCompactDequeInitPrefix[] = {
        0xC7, 0x83, 0x40, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC7, 0x83, 0x48, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC7, 0x83, 0x4C, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC7, 0x83, 0x50, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC7, 0x83, 0x54, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xC7, 0x83, 0x58, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    static const uintptr_t kCompactVtableSlots[] = {
        0x000486B0u, 0x00048550u, 0x00046770u,
        0x000467C0u, 0x000458F0u, 0x00046060u,
        0x000465E0u, 0x00064610u, 0x00064630u,
        0x00064660u,
    };
    static const uintptr_t kRollbackVtableSlots[] = {
        0x000585A0u, 0x00058580u, 0x000533B0u,
        0x00054CD0u, 0x00050FF0u, 0x00052740u,
        0x00052800u, 0x0004E9E0u, 0x0004EDE0u,
        0x00051A80u,
    };
    static const uintptr_t kSpectatorVtableSlots[] = {
        0x00062920u, 0x00062900u, 0x0005E640u,
        0x0005F790u, 0x0005D720u, 0x0005E380u,
        0x0005E3E0u, 0x0005C400u, 0x0005C4E0u,
        0x0005DE40u,
    };
    static const uintptr_t kReplayVtableSlots[] = {
        0x00075CF0u, 0x00075CD0u, 0x000744A0u,
        0x00074920u, 0x00073190u, 0x00074090u,
        0x000741E0u, 0x00072F80u, 0x00073060u,
        0x00073F40u,
    };
    static const uintptr_t kPracticeVtableSlots[] = {
        0x00080650u, 0x00080630u, 0x0007DE40u,
        0x0007E140u, 0x0007CF60u, 0x0007DBC0u,
        0x00064680u, 0x0007CCC0u, 0x0007CDA0u,
        0x0007D780u,
    };

    require(ModuleBytesMatchOrInstalledJmp(
        module,
        kRevival_1_02j.frameHookRva,
        kFrameHookPrefix,
        sizeof(kFrameHookPrefix),
        6), "frameHook bytes");
    require(ModuleBytesMatchOrInstalledJmp(
        module,
        kRevival_1_02j.perFrameTickRva,
        kPerFrameTickPrefix,
        sizeof(kPerFrameTickPrefix),
        8), "perFrameTick bytes");
    require(ModuleBytesMatchMasked(module, kRevival_1_02j.inputSwapPairRva, kInputSwapPrefix, "x????xxxxxxxx????xxxxxxx", sizeof(kInputSwapPrefix)), "inputSwap bytes");
    require(ModuleWindowContainsU32(
        module,
        kRevival_1_02j.inputSwapPairRva,
        0x80u,
        static_cast<uint32_t>(kRevival_1_02j.sessionOffsetActivePlayer)),
        "inputSwap activePlayer offset ref");
    require(ModuleDirectCallCountEquals(
        module,
        0x00054CD0u,
        0x800u,
        kRevival_1_02j.inputSwapPairRva,
        1), "inputSwap rollback call count");
    require(ModuleBytesMatch(module, kRevival_1_02j.startInitPlayerRva, kStartInitPrefix, sizeof(kStartInitPrefix)), "startInit bytes");
    require(ModuleBytesMatch(module, kRevival_1_02j.setTextEnabledRva, kSetTextPrefix, sizeof(kSetTextPrefix)), "setText bytes");
    require(ModuleBytesMatch(module, kRevival_1_02j.clearTextRva, kClearTextPrefix, sizeof(kClearTextPrefix)), "clearText bytes");
    static const uint32_t kInstallExeHookTargets[] = {
        0x00776053u, 0x0040D131u, 0x007656A7u, 0x00401642u,
        0x0076479Cu, 0x00777D61u, 0x00406020u, 0x00405FB0u,
        0x00405F00u, 0x00405E90u, 0x00405F50u, 0x0040DE98u,
        0x0040DE80u, 0x007668E5u, 0x0040DE56u, 0x0040DE40u,
        0x0040E5E0u, 0x0040E5D0u,
    };
    for (size_t i = 0; i < sizeof(kInstallExeHookTargets) / sizeof(kInstallExeHookTargets[0]); ++i)
    {
        require(ModuleWindowContainsU32(
            module,
            kRevival_1_02j.frameHookRva,
            0x800u,
            kInstallExeHookTargets[i]),
            "install_exe_hooks target ref");
    }
    require(
        ModuleBytesMatch(module, 0x0004683Fu, kTournamentExitGuardPrefix, sizeof(kTournamentExitGuardPrefix))
        || ModuleBytesMatch(module, 0x0004683Fu, kTournamentExitGuardPatched, sizeof(kTournamentExitGuardPatched)),
        "tournamentExitGuard bytes");
    require(ModuleVtableSlotsMatch(module, 0x0016FEB0u, kCompactVtableSlots, sizeof(kCompactVtableSlots) / sizeof(kCompactVtableSlots[0])), "compact vtable slots");
    require(ModuleVtableSlotsMatch(module, 0x0016FEF0u, kRollbackVtableSlots, sizeof(kRollbackVtableSlots) / sizeof(kRollbackVtableSlots[0])), "rollback vtable slots");
    require(ModuleVtableSlotsMatch(module, 0x0016FF20u, kSpectatorVtableSlots, sizeof(kSpectatorVtableSlots) / sizeof(kSpectatorVtableSlots[0])), "spectator vtable slots");
    require(ModuleVtableSlotsMatch(module, 0x0016FF50u, kReplayVtableSlots, sizeof(kReplayVtableSlots) / sizeof(kReplayVtableSlots[0])), "replay vtable slots");
    require(ModuleVtableSlotsMatch(module, 0x0016FF80u, kPracticeVtableSlots, sizeof(kPracticeVtableSlots) / sizeof(kPracticeVtableSlots[0])), "practice vtable slots");
    require(ModuleBytesMatch(
        module,
        0x0005E7A0u,
        kSpectatorStoreHelperHandle,
        sizeof(kSpectatorStoreHelperHandle)),
        "spectator post-init helper handle offset");
    require(ModuleWindowContainsU32(
        module,
        0x0005E640u,
        0x1000u,
        0x000002E0u),
        "spectator post-init helper pid offset");
    require(ModuleWindowContainsU32(
        module,
        0x0005E640u,
        0x1000u,
        0x00000588u),
        "spectator post-init netplay control offset");
    require(ModuleBytesMatch(module, 0x00047BA8u, kCompactDequeInitPrefix, sizeof(kCompactDequeInitPrefix)), "compact deque init bytes");
    require(ModuleDirectCallCountEquals(module, 0x000479B0u, 0xA00u, 0x00102A20u, 22), "compact deque push count");
    require(kRevival_1_02j.tournamentInputQueueOffset == 0x340u, "compact deque profile offset");
    static const uint32_t kCompactExePatchTargets[] = {
        0x00763F04u, 0x00763E50u, 0x00754C1Au, 0x007599EDu,
    };
    for (size_t i = 0; i < sizeof(kCompactExePatchTargets) / sizeof(kCompactExePatchTargets[0]); ++i)
    {
        require(ModuleWindowContainsU32(module, 0x000479B0u, 0xA00u, kCompactExePatchTargets[i]), "compact EXE patch target ref");
    }

    if (ok)
    {
        mod::Log(
            "DetectRevivalVersion: 1.02j binary verification OK preferredImageBase=0x%08lX loadedBase=%p size=0x%08lX",
            static_cast<unsigned long>(nt->OptionalHeader.ImageBase),
            static_cast<void*>(module),
            static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage));
    }
    return ok;
}

static const RevivalAddressProfile* FindLoadedRevivalProfile(
    HMODULE module,
    uint32_t timestamp)
{
    if (timestamp == kRevival_1_02j.peTimestamp)
    {
        return VerifyRevival102jBinary(module) ? &kRevival_1_02j : nullptr;
    }

    if (timestamp != kRevival_1_02f.peTimestamp)
    {
        return FindRevivalProfileByTimestamp(timestamp);
    }

    // Modified 1.02f DLLs can preserve the stock PE timestamp.  Only accept
    // the stock 1.02f profile when its key code anchors still match stock.
    static const uint8_t kStockFrameHookPrefix[] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0,
    };
    static const uint8_t kStockStartInitPlayerPrefix[] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8,
    };

    const bool frameHookMatches = ModuleBytesMatch(
        module,
        kRevival_1_02f.frameHookRva,
        kStockFrameHookPrefix,
        sizeof(kStockFrameHookPrefix));
    const bool startInitMatches = ModuleBytesMatch(
        module,
        kRevival_1_02f.startInitPlayerRva,
        kStockStartInitPlayerPrefix,
        sizeof(kStockStartInitPlayerPrefix));

    if (frameHookMatches && startInitMatches)
    {
        return &kRevival_1_02f;
    }

    mod::Log(
        "DetectRevivalVersion: timestamp 0x%08X matches 1.02f but stock byte verification failed frameHook=%d startInit=%d",
        static_cast<unsigned>(timestamp),
        frameHookMatches ? 1 : 0,
        startInitMatches ? 1 : 0);
    return nullptr;
}

static bool TryReadModulePeTimestamp(HMODULE module, uint32_t* outTimestamp)
{
    if (outTimestamp != nullptr)
    {
        *outTimestamp = 0;
    }
    if (module == nullptr)
    {
        return false;
    }

    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto dosE_lfanew = *reinterpret_cast<const int32_t*>(base + 0x3C);
    if (dosE_lfanew < 0 || dosE_lfanew > 0x1000)
    {
        return false;
    }

    const auto* peSignature = reinterpret_cast<const uint32_t*>(base + dosE_lfanew);
    if (*peSignature != 0x00004550u)
    {
        return false;
    }

    if (outTimestamp != nullptr)
    {
        *outTimestamp = *reinterpret_cast<const uint32_t*>(base + dosE_lfanew + 8);
    }
    return true;
}

static void SetActiveRevivalProfile(
    const RevivalAddressProfile* profile,
    RevivalProfileSource source)
{
    g_activeRevival = (profile != nullptr) ? profile : &kRevival_1_02e;
    g_activeRevivalSource = source;
}

void EnsureActiveRevivalProfile()
{
    const HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    const bool hasPublishedTimestamp =
        g_injectedBlock != nullptr && g_injectedBlock->hostRevivalTimestamp != 0;

    switch (g_activeRevivalSource)
    {
    case RevivalProfileSource::DllTimestamp:
        if (revival != nullptr)
        {
            return;
        }
        break;
    case RevivalProfileSource::PublishedHostTimestamp:
        if (revival == nullptr && hasPublishedTimestamp)
        {
            return;
        }
        break;
    case RevivalProfileSource::ExternalLauncherGuard:
        if (IsExternalLauncherGuardProcess())
        {
            return;
        }
        break;
    case RevivalProfileSource::Default:
    default:
        if (revival == nullptr && !hasPublishedTimestamp)
        {
            return;
        }
        break;
    }

    DetectRevivalVersion();
}

bool ActiveRevivalProfileSupportsSessionStart()
{
    EnsureActiveRevivalProfile();
    if (g_activeRevival == nullptr || g_activeRevival->versionTag == nullptr)
    {
        return false;
    }
    if (std::strcmp(g_activeRevival->versionTag, "unsupported") == 0)
    {
        return false;
    }

    return g_activeRevival->roleFlagOffsetCount > 0
        && g_activeRevival->roleFlagOffsets[0] != 0
        && g_activeRevival->sessionPtrOffsetCount > 0
        && g_activeRevival->sessionPtrOffsets[0] != 0
        && g_activeRevival->startInitPlayerRva != 0
        && g_activeRevival->frameHookRva != 0
        && g_activeRevival->perFrameTickRva != 0;
}

/// Detect the loaded Revival DLL version by reading its PE TimeDateStamp and,
/// for ambiguous 1.02f builds, verifying stock code bytes before accepting the
/// stock profile. Unsupported builds switch to the zeroed unsupported profile
/// so later hook installation fails closed.
void DetectRevivalVersion()
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    const uint32_t publishedTimestamp =
        (g_injectedBlock != nullptr) ? g_injectedBlock->hostRevivalTimestamp : 0;

    if (revival != nullptr)
    {
        uint32_t timestamp = 0;
        if (!TryReadModulePeTimestamp(revival, &timestamp))
        {
            SetActiveRevivalProfile(&kRevival_Unsupported, RevivalProfileSource::DllTimestamp);
            mod::Log("DetectRevivalVersion: invalid EfzRevival.dll PE header - fail closed");
            return;
        }

        if (publishedTimestamp != 0 && publishedTimestamp != timestamp)
        {
            mod::Log(
                "DetectRevivalVersion: local/published timestamp mismatch local=0x%08X published=0x%08X",
                static_cast<unsigned>(timestamp),
                static_cast<unsigned>(publishedTimestamp));
        }

        if (const RevivalAddressProfile* const profile = FindLoadedRevivalProfile(revival, timestamp))
        {
            SetActiveRevivalProfile(profile, RevivalProfileSource::DllTimestamp);
            mod::Log("DetectRevivalVersion: matched timestamp 0x%08X → %s",
                     static_cast<unsigned>(timestamp),
                     g_activeRevival->versionTag);
            return;
        }

        SetActiveRevivalProfile(&kRevival_Unsupported, RevivalProfileSource::DllTimestamp);
        mod::Log(
            "DetectRevivalVersion: unsupported or modified DLL build timestamp=0x%08X - fail closed as %s",
            static_cast<unsigned>(timestamp),
            g_activeRevival->versionTag);
        return;
    }

    if (publishedTimestamp != 0)
    {
        if (const RevivalAddressProfile* const profile = FindRevivalProfileByTimestamp(publishedTimestamp))
        {
            SetActiveRevivalProfile(profile, RevivalProfileSource::PublishedHostTimestamp);
            mod::Log(
                "DetectRevivalVersion: adopted published host timestamp 0x%08X → %s",
                static_cast<unsigned>(publishedTimestamp),
                g_activeRevival->versionTag);
            return;
        }

        SetActiveRevivalProfile(&kRevival_Unsupported, RevivalProfileSource::PublishedHostTimestamp);
        mod::Log(
            "DetectRevivalVersion: published host timestamp 0x%08X unknown - fail closed as %s",
            static_cast<unsigned>(publishedTimestamp),
            g_activeRevival->versionTag);
        return;
    }

    SetActiveRevivalProfile(&kRevival_Unsupported, RevivalProfileSource::Default);
    if (revival == nullptr)
    {
        mod::Log(
            "DetectRevivalVersion: EfzRevival.dll not loaded and no published host profile - fail closed as %s",
            g_activeRevival->versionTag);
        return;
    }

    mod::Log("DetectRevivalVersion: fail closed as %s",
             g_activeRevival->versionTag);
}

static uintptr_t ResolveHelperSendQuitAllRva()
{
    EnsureActiveRevivalProfile();

    // Native helper routine that broadcasts EfzPackets::MessageQuit to every
    // connected peer. RVAs were confirmed from the versioned decompilations
    // and, for 1.02j, by checking the helper EXE bytes at RVA 0x86390.
    if (g_activeRevival == nullptr || g_activeRevival->versionTag == nullptr)
    {
        return 0;
    }

    const char* const tag = g_activeRevival->versionTag;
    if (std::strcmp(tag, "1.02e") == 0)
    {
        return 0x41150u;
    }
    if (std::strcmp(tag, "1.02f") == 0
        || std::strcmp(tag, "1.02f-framestepping") == 0)
    {
        return 0x41190u;
    }
    if (std::strcmp(tag, "1.02g") == 0)
    {
        return 0x414B0u;
    }
    if (std::strcmp(tag, "1.02h") == 0)
    {
        return 0x41440u;
    }
    if (std::strcmp(tag, "1.02i") == 0)
    {
        return 0x425C0u;
    }
    if (std::strcmp(tag, "1.02j") == 0)
    {
        return 0x86390u;
    }
    return 0;
}

static uintptr_t ResolveHelperQuitRoleCheckRva()
{
    EnsureActiveRevivalProfile();

    if (g_activeRevival == nullptr || g_activeRevival->versionTag == nullptr)
    {
        return 0;
    }

    const char* const tag = g_activeRevival->versionTag;
    if (std::strcmp(tag, "1.02e") == 0 || std::strcmp(tag, "1.02h") == 0)
    {
        return 0x201B0u;
    }
    if (std::strcmp(tag, "1.02f") == 0
        || std::strcmp(tag, "1.02f-framestepping") == 0)
    {
        return 0x20140u;
    }
    if (std::strcmp(tag, "1.02g") == 0)
    {
        return 0x20040u;
    }
    if (std::strcmp(tag, "1.02i") == 0)
    {
        return 0x20C80u;
    }
    if (std::strcmp(tag, "1.02j") == 0)
    {
        return 0x81CF0u;
    }
    return 0;
}

struct RevivalQuitEndpointStorage
{
    uint16_t family = 0;
    uint16_t reserved = 0;
    uint32_t ipv4Address = 0;
    uint8_t ipv6Address[16] = {};
    uint32_t ipv6ScopeId = 0;
};

static_assert(
    sizeof(RevivalQuitEndpointStorage) == 28,
    "Revival quit endpoint storage must match the helper's 28-byte endpoint object");

static bool TryBuildRevivalQuitEndpointFromText(
    const char* endpointText,
    RevivalQuitEndpointStorage* outEndpoint)
{
    if (outEndpoint != nullptr)
    {
        std::memset(outEndpoint, 0, sizeof(*outEndpoint));
    }
    if (endpointText == nullptr || endpointText[0] == '\0' || outEndpoint == nullptr)
    {
        return false;
    }

    const std::string trimmed = TrimAscii(endpointText);
    if (trimmed.empty() || trimmed.size() >= 256)
    {
        return false;
    }

    char addressBuffer[256] = {};
    std::memcpy(addressBuffer, trimmed.c_str(), trimmed.size());
    addressBuffer[trimmed.size()] = '\0';

    SOCKADDR_STORAGE storage = {};
    int storageLen = sizeof(storage);
    if (WSAStringToAddressA(
            addressBuffer,
            AF_INET,
            nullptr,
            reinterpret_cast<LPSOCKADDR>(&storage),
            &storageLen)
        == 0)
    {
        const sockaddr_in* const address =
            reinterpret_cast<const sockaddr_in*>(&storage);
        outEndpoint->family = AF_INET;
        outEndpoint->ipv4Address = address->sin_addr.S_un.S_addr;
        return true;
    }

    std::memset(&storage, 0, sizeof(storage));
    storageLen = sizeof(storage);
    if (WSAStringToAddressA(
            addressBuffer,
            AF_INET6,
            nullptr,
            reinterpret_cast<LPSOCKADDR>(&storage),
            &storageLen)
        != 0)
    {
        return false;
    }

    const sockaddr_in6* const address6 =
        reinterpret_cast<const sockaddr_in6*>(&storage);
    outEndpoint->family = AF_INET6;
    std::memcpy(
        outEndpoint->ipv6Address,
        &address6->sin6_addr,
        sizeof(outEndpoint->ipv6Address));
    outEndpoint->ipv6ScopeId = address6->sin6_scope_id;
    return true;
}

static bool IsWritableUserRegion(const MEMORY_BASIC_INFORMATION& mbi)
{
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    {
        return false;
    }

    const DWORD protect = mbi.Protect & 0xFFu;
    return protect == PAGE_READWRITE
        || protect == PAGE_WRITECOPY
        || protect == PAGE_EXECUTE_READWRITE
        || protect == PAGE_EXECUTE_WRITECOPY;
}

static bool TryGetProcessIdFromHandleCompat(HANDLE processHandle, DWORD* outPid)
{
    if (outPid != nullptr)
    {
        *outPid = 0;
    }
    if (processHandle == nullptr || processHandle == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    typedef DWORD (WINAPI *GetProcessIdFn)(HANDLE);
    static GetProcessIdFn s_getProcessId = []() -> GetProcessIdFn {
        HMODULE kernel = GetModuleHandleA("kernel32.dll");
        return kernel != nullptr
            ? reinterpret_cast<GetProcessIdFn>(GetProcAddress(kernel, "GetProcessId"))
            : nullptr;
    }();

    if (s_getProcessId == nullptr)
    {
        return false;
    }

    const DWORD pid = s_getProcessId(processHandle);
    if (pid == 0)
    {
        return false;
    }

    if (outPid != nullptr)
    {
        *outPid = pid;
    }
    return true;
}

struct InjectedPeerManagerValidation
{
    bool valid = false;
    bool strictCheckUsed = false;
    bool structureCheckUsed = false;
    bool peerContainerSnapshotRead = false;
    bool peerBackingSnapshotRead = false;
    bool quitRingSnapshotRead = false;
    DWORD selfPid = 0;
    DWORD childPid = 0;
    DWORD handlePid = 0;
    DWORD exitCode = 0;
    DWORD quitCapacity = 0;
    DWORD handleFlags = 0;
    LONG quitHead = 0;
    LONG quitTail = 0;
    SIZE_T objectRegionSize = 0;
    uintptr_t objectBase = 0;
    uintptr_t peerContainerBase = 0;
    uintptr_t peerBackingBase = 0;
    uintptr_t quitRingBase = 0;
    size_t layoutObjectSize = 0;
    size_t layoutPeerContainerOffset = 0;
    size_t layoutPeerBackingOffset = 0;
    size_t layoutQuitRingOffset = 0;
    HANDLE hostProcessHandle = nullptr;
    DWORD peerContainerWords[8] = {};
    DWORD peerBackingWords[8] = {};
    DWORD quitRingWords[8] = {};
    const char* reason = "not_checked";
};

constexpr size_t kInjectedPeerManagerSelfPidOffset = 328u;
constexpr size_t kInjectedPeerManagerHostHandleOffset = 360u;
constexpr size_t kInjectedPeerManagerChildPidOffset = 368u;
constexpr size_t kInjectedPeerManagerPeerContainerOffset = 488u;
// Native decomp shows the live session object stores the peer-manager pointer
// at +1176 for e/h/i families; the same session object also carries the
// versioned currentFrame field that our session-pointer code already tracks.
constexpr size_t kInjectedPeerManagerSessionFieldOffset = 1176u;
constexpr size_t kInjectedPeerManagerQuitRingCapacityOffset = 36u;
constexpr DWORD kInjectedPeerManagerQuitRingExpectedCapacity = 1u;
constexpr size_t kInjectedPeerManagerMinScanSpan =
    kInjectedPeerManagerHostHandleOffset + sizeof(HANDLE);
constexpr size_t kInjectedPeerManagerDumpWordCount = 8u;

struct InjectedPeerManagerLayout
{
    size_t objectSize = 0;
    size_t peerContainerOffset = 0;
    size_t peerBackingOffset = 0;
    size_t quitRingOffset = 0;
};

static InjectedPeerManagerLayout ResolveInjectedPeerManagerLayout()
{
    EnsureActiveRevivalProfile();

    InjectedPeerManagerLayout layout = {};
    if (g_activeRevival == nullptr || g_activeRevival->versionTag == nullptr)
    {
        return layout;
    }

    const char* const tag = g_activeRevival->versionTag;
    if (std::strcmp(tag, "1.02i") == 0)
    {
        layout.objectSize = 0x1318u;
        layout.peerContainerOffset = kInjectedPeerManagerPeerContainerOffset;
        layout.peerBackingOffset = 684u;
        layout.quitRingOffset = 4624u;
        return layout;
    }

    if (std::strcmp(tag, "1.02j") == 0)
    {
        // 1.02j MinGW helper:
        //   sendQuitAll uses this + 0x218 for the peer container.
        //   ctor allocates 0x1488 bytes and initializes Quit_Spec at +0x1338.
        layout.objectSize = 0x1488u;
        layout.peerContainerOffset = 0x218u;
        layout.peerBackingOffset = 0x300u;
        layout.quitRingOffset = 0x1338u;
        return layout;
    }

    if (std::strcmp(tag, "1.02e") == 0
        || std::strcmp(tag, "1.02f") == 0
        || std::strcmp(tag, "1.02f-framestepping") == 0
        || std::strcmp(tag, "1.02g") == 0
        || std::strcmp(tag, "1.02h") == 0)
    {
        layout.objectSize = 0x760u;
        layout.peerContainerOffset = kInjectedPeerManagerPeerContainerOffset;
        layout.peerBackingOffset = 680u;
        layout.quitRingOffset = 1624u;
    }

    return layout;
}

static DWORD EncodeInjectedPeerManagerReason(const char* reason)
{
    if (reason == nullptr)
    {
        return 0;
    }
    if (std::strcmp(reason, "identity_unreadable") == 0)
    {
        return 1;
    }
    if (std::strcmp(reason, "self_pid_mismatch") == 0)
    {
        return 2;
    }
    if (std::strcmp(reason, "invalid_host_handle") == 0)
    {
        return 3;
    }
    if (std::strcmp(reason, "host_handle_query_failed") == 0)
    {
        return 4;
    }
    if (std::strcmp(reason, "host_pid_mismatch") == 0)
    {
        return 5;
    }
    if (std::strcmp(reason, "host_process_dead") == 0)
    {
        return 6;
    }
    if (std::strcmp(reason, "layout_inconsistent") == 0)
    {
        return 7;
    }
    if (std::strcmp(reason, "object_query_failed") == 0)
    {
        return 8;
    }
    if (std::strcmp(reason, "object_region_invalid") == 0)
    {
        return 9;
    }
    if (std::strcmp(reason, "object_base_invalid") == 0)
    {
        return 10;
    }
    if (std::strcmp(reason, "object_out_of_range") == 0)
    {
        return 11;
    }
    if (std::strcmp(reason, "structure_unreadable") == 0)
    {
        return 12;
    }
    if (std::strcmp(reason, "quit_ring_base_null") == 0)
    {
        return 13;
    }
    if (std::strcmp(reason, "quit_ring_capacity_invalid") == 0)
    {
        return 14;
    }
    if (std::strcmp(reason, "quit_ring_region_invalid") == 0)
    {
        return 15;
    }
    if (std::strcmp(reason, "quit_ring_range_invalid") == 0)
    {
        return 16;
    }
    if (std::strcmp(reason, "quit_ring_state_invalid") == 0)
    {
        return 17;
    }
    return 255;
}

static const char* DecodeInjectedPeerManagerReason(DWORD detail)
{
    switch (detail)
    {
    case 1:
        return "identity_unreadable";
    case 2:
        return "self_pid_mismatch";
    case 3:
        return "invalid_host_handle";
    case 4:
        return "host_handle_query_failed";
    case 5:
        return "host_pid_mismatch";
    case 6:
        return "host_process_dead";
    case 7:
        return "layout_inconsistent";
    case 8:
        return "object_query_failed";
    case 9:
        return "object_region_invalid";
    case 10:
        return "object_base_invalid";
    case 11:
        return "object_out_of_range";
    case 12:
        return "structure_unreadable";
    case 13:
        return "quit_ring_base_null";
    case 14:
        return "quit_ring_capacity_invalid";
    case 15:
        return "quit_ring_region_invalid";
    case 16:
        return "quit_ring_range_invalid";
    case 17:
        return "quit_ring_state_invalid";
    case 255:
        return "other_validation_reason";
    default:
        return "unknown_detail";
    }
}

static DWORD EncodeInjectedExceptionDetail(DWORD exceptionCode)
{
    switch (exceptionCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
        return 1;
    case EXCEPTION_IN_PAGE_ERROR:
        return 2;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return 3;
    case EXCEPTION_STACK_OVERFLOW:
        return 4;
    case EXCEPTION_GUARD_PAGE:
        return 5;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return 6;
    default:
        return 255;
    }
}

static const char* DecodeInjectedExceptionDetail(DWORD detail)
{
    switch (detail)
    {
    case 1:
        return "access_violation";
    case 2:
        return "in_page_error";
    case 3:
        return "illegal_instruction";
    case 4:
        return "stack_overflow";
    case 5:
        return "guard_page";
    case 6:
        return "datatype_misalignment";
    case 255:
        return "other_exception";
    default:
        return "unknown_exception";
    }
}

static bool ConsoleErrorIndicatesDesync(const char* text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return false;
    }

    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lowered.find("desync") != std::string::npos
        || lowered.find("header crc mismatch") != std::string::npos
        || lowered.find("state not recoverable") != std::string::npos;
}

static void LogInjectedPeerQuitDiagnosticLine(const char* fmt, ...)
{
    char buffer[1024] = {};
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (written <= 0)
    {
        return;
    }

    mod::Log("%s", buffer);
    AppendPeerQuitDiagnostic(buffer);
}

static void FlushInjectedPeerQuitDiagnosticToHostLog(const char* reason, DWORD helperPid)
{
    LONG diagnosticSerial = 0;
    char diagnosticText[8192] = {};
    ReadPeerQuitDiagnostic(&diagnosticSerial, diagnosticText, sizeof(diagnosticText));
    if (diagnosticText[0] == '\0')
    {
        mod::Log(
            "Takeover: peer-quit helper dump empty reason='%s' serial=%ld helperPid=%lu",
            reason != nullptr ? reason : "",
            static_cast<long>(diagnosticSerial),
            static_cast<unsigned long>(helperPid));
        return;
    }

    const char* cursor = diagnosticText;
    while (*cursor != '\0')
    {
        while (*cursor == '\r' || *cursor == '\n')
        {
            ++cursor;
        }
        if (*cursor == '\0')
        {
            break;
        }

        const char* lineEnd = cursor;
        while (*lineEnd != '\0' && *lineEnd != '\r' && *lineEnd != '\n')
        {
            ++lineEnd;
        }

        char line[1024] = {};
        const size_t lineLen = static_cast<size_t>(lineEnd - cursor);
        const size_t copyLen = (std::min)(lineLen, sizeof(line) - 1u);
        std::memcpy(line, cursor, copyLen);
        line[copyLen] = '\0';
        mod::Log(
            "Takeover: peer-quit helper dump reason='%s' serial=%ld helperPid=%lu | %s",
            reason != nullptr ? reason : "",
            static_cast<long>(diagnosticSerial),
            static_cast<unsigned long>(helperPid),
            line);
        cursor = lineEnd;
    }
}

static bool TryReadInjectedPeerManagerIdentity(
    uintptr_t candidatePtr,
    DWORD* outSelfPid,
    HANDLE* outHostProcessHandle)
{
    if (outSelfPid != nullptr)
    {
        *outSelfPid = 0;
    }
    if (outHostProcessHandle != nullptr)
    {
        *outHostProcessHandle = nullptr;
    }
    if (candidatePtr == 0)
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(candidatePtr), &mbi, sizeof(mbi))
        != sizeof(mbi))
    {
        return false;
    }
    if (!IsWritableUserRegion(mbi)
        || static_cast<uintptr_t>(mbi.RegionSize) < kInjectedPeerManagerMinScanSpan)
    {
        return false;
    }

    const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (candidatePtr < regionBase)
    {
        return false;
    }

    const uintptr_t maxOffset =
        static_cast<uintptr_t>(mbi.RegionSize) - kInjectedPeerManagerMinScanSpan;
    const uintptr_t candidateOffset = candidatePtr - regionBase;
    if (candidateOffset > maxOffset)
    {
        return false;
    }

    __try
    {
        if (outSelfPid != nullptr)
        {
            *outSelfPid = *reinterpret_cast<const DWORD*>(
                candidatePtr + kInjectedPeerManagerSelfPidOffset);
        }
        if (outHostProcessHandle != nullptr)
        {
            *outHostProcessHandle = *reinterpret_cast<HANDLE const*>(
                candidatePtr + kInjectedPeerManagerHostHandleOffset);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    return true;
}

static bool TryReadInjectedWordSnapshot(
    uintptr_t address,
    DWORD* outWords,
    size_t wordCount)
{
    if (outWords == nullptr || wordCount == 0 || address == 0)
    {
        return false;
    }

    std::fill(outWords, outWords + wordCount, 0u);
    __try
    {
        for (size_t index = 0; index < wordCount; ++index)
        {
            outWords[index] = *reinterpret_cast<const DWORD*>(
                address + index * sizeof(DWORD));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    return true;
}

static void LogInjectedWordSnapshot(
    const char* context,
    const char* label,
    uintptr_t address,
    const DWORD* words,
    size_t wordCount,
    bool readOk)
{
    if (address == 0 || words == nullptr || wordCount == 0)
    {
        return;
    }

    char buffer[512] = {};
    int written = std::snprintf(
        buffer,
        sizeof(buffer),
        "Takeover: %s %s addr=0x%08lX read=%d words=",
        context != nullptr ? context : "injected peer-manager dump",
        label != nullptr ? label : "snapshot",
        static_cast<unsigned long>(address),
        readOk ? 1 : 0);
    if (written < 0)
    {
        return;
    }

    size_t used = static_cast<size_t>(written);
    for (size_t index = 0; index < wordCount && used < sizeof(buffer); ++index)
    {
        written = std::snprintf(
            buffer + used,
            sizeof(buffer) - used,
            "%s%08lX",
            index == 0 ? "" : " ",
            static_cast<unsigned long>(words[index]));
        if (written < 0)
        {
            return;
        }
        const size_t delta = static_cast<size_t>(written);
        if (delta >= sizeof(buffer) - used)
        {
            used = sizeof(buffer) - 1u;
            break;
        }
        used += delta;
    }

    LogInjectedPeerQuitDiagnosticLine("%s", buffer);
}

static void LogInjectedPeerManagerLayout(const char* context)
{
    const InjectedPeerManagerLayout layout = ResolveInjectedPeerManagerLayout();
    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: %s version=%s objectSize=0x%08lX peerContainerOff=0x%08lX peerBackingOff=0x%08lX quitRingOff=0x%08lX",
        context != nullptr ? context : "injected peer-manager layout",
        (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
            ? g_activeRevival->versionTag
            : "unknown",
        static_cast<unsigned long>(layout.objectSize),
        static_cast<unsigned long>(layout.peerContainerOffset),
        static_cast<unsigned long>(layout.peerBackingOffset),
        static_cast<unsigned long>(layout.quitRingOffset));
}

static bool ValidateInjectedPeerManagerStructure(
    uintptr_t candidatePtr,
    DWORD hostPid,
    InjectedPeerManagerValidation* result)
{
    if (result == nullptr)
    {
        return false;
    }

    (void)hostPid;

    const InjectedPeerManagerLayout layout = ResolveInjectedPeerManagerLayout();
    result->layoutObjectSize = layout.objectSize;
    result->layoutPeerContainerOffset = layout.peerContainerOffset;
    result->layoutPeerBackingOffset = layout.peerBackingOffset;
    result->layoutQuitRingOffset = layout.quitRingOffset;
    result->peerContainerBase = candidatePtr + layout.peerContainerOffset;
    result->peerBackingBase = candidatePtr + layout.peerBackingOffset;
    if (layout.objectSize == 0)
    {
        return true;
    }

    result->structureCheckUsed = true;

    const size_t peerContainerEnd =
        layout.peerContainerOffset + kInjectedPeerManagerDumpWordCount * sizeof(DWORD);
    const size_t peerBackingEnd = layout.peerBackingOffset + 28u * 4u;
    const size_t quitRingEnd =
        layout.quitRingOffset + kInjectedPeerManagerQuitRingCapacityOffset + sizeof(DWORD);
    if (peerContainerEnd > layout.objectSize
        || peerBackingEnd > layout.objectSize
        || quitRingEnd > layout.objectSize)
    {
        result->reason = "layout_inconsistent";
        return false;
    }

    MEMORY_BASIC_INFORMATION objectMbi = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(candidatePtr), &objectMbi, sizeof(objectMbi))
        != sizeof(objectMbi))
    {
        result->reason = "object_query_failed";
        return false;
    }
    if (!IsWritableUserRegion(objectMbi)
        || static_cast<uintptr_t>(objectMbi.RegionSize) < layout.objectSize)
    {
        result->reason = "object_region_invalid";
        return false;
    }

    const uintptr_t objectBase = reinterpret_cast<uintptr_t>(objectMbi.BaseAddress);
    result->objectBase = objectBase;
    result->objectRegionSize = objectMbi.RegionSize;
    if (candidatePtr < objectBase)
    {
        result->reason = "object_base_invalid";
        return false;
    }

    const uintptr_t objectOffset = candidatePtr - objectBase;
    const uintptr_t maxObjectOffset =
        static_cast<uintptr_t>(objectMbi.RegionSize) - layout.objectSize;
    if (objectOffset > maxObjectOffset)
    {
        result->reason = "object_out_of_range";
        return false;
    }

    uintptr_t quitRingBase = 0;
    __try
    {
        result->childPid = *reinterpret_cast<const DWORD*>(
            candidatePtr + kInjectedPeerManagerChildPidOffset);
        quitRingBase = *reinterpret_cast<const uintptr_t*>(
            candidatePtr + layout.quitRingOffset);
        result->quitCapacity = *reinterpret_cast<const DWORD*>(
            candidatePtr + layout.quitRingOffset + kInjectedPeerManagerQuitRingCapacityOffset);
        if (quitRingBase != 0)
        {
            result->quitHead = *reinterpret_cast<const LONG*>(quitRingBase);
            result->quitTail = *reinterpret_cast<const LONG*>(quitRingBase + sizeof(LONG));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        result->reason = "structure_unreadable";
        return false;
    }

    result->quitRingBase = quitRingBase;
    result->peerContainerSnapshotRead = TryReadInjectedWordSnapshot(
        result->peerContainerBase,
        result->peerContainerWords,
        kInjectedPeerManagerDumpWordCount);
    result->peerBackingSnapshotRead = TryReadInjectedWordSnapshot(
        result->peerBackingBase,
        result->peerBackingWords,
        kInjectedPeerManagerDumpWordCount);
    if (quitRingBase != 0)
    {
        result->quitRingSnapshotRead = TryReadInjectedWordSnapshot(
            quitRingBase,
            result->quitRingWords,
            kInjectedPeerManagerDumpWordCount);
    }

    // Native manager construction initializes the Quit shared-memory ring
    // immediately with capacity 1. sendQuitAll checks and dereferences that
    // ring before enqueueing, so a null ring base here means this candidate is
    // not the live peer manager object we want.
    if (quitRingBase == 0)
    {
        result->reason = "quit_ring_base_null";
        return false;
    }
    if (result->quitCapacity != kInjectedPeerManagerQuitRingExpectedCapacity)
    {
        result->reason = "quit_ring_capacity_invalid";
        return false;
    }

    MEMORY_BASIC_INFORMATION ringMbi = {};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(quitRingBase), &ringMbi, sizeof(ringMbi))
            != sizeof(ringMbi)
        || !IsWritableUserRegion(ringMbi))
    {
        result->reason = "quit_ring_region_invalid";
        return false;
    }

    const uintptr_t ringRegionBase = reinterpret_cast<uintptr_t>(ringMbi.BaseAddress);
    if (quitRingBase < ringRegionBase)
    {
        result->reason = "quit_ring_range_invalid";
        return false;
    }

    const uintptr_t ringOffset = quitRingBase - ringRegionBase;
    const uintptr_t ringRequiredSpan =
        8u + static_cast<uintptr_t>(result->quitCapacity);
    if (static_cast<uintptr_t>(ringMbi.RegionSize) < ringRequiredSpan
        || ringOffset > static_cast<uintptr_t>(ringMbi.RegionSize) - ringRequiredSpan)
    {
        result->reason = "quit_ring_range_invalid";
        return false;
    }

    if (result->quitTail < result->quitHead)
    {
        result->reason = "quit_ring_state_invalid";
        return false;
    }

    const DWORD pendingCount = static_cast<DWORD>(result->quitTail - result->quitHead);
    if (pendingCount > result->quitCapacity)
    {
        result->reason = "quit_ring_state_invalid";
        return false;
    }

    return true;
}

static InjectedPeerManagerValidation ValidateInjectedPeerManagerPointer(
    uintptr_t candidatePtr,
    DWORD hostPid)
{
    InjectedPeerManagerValidation result = {};
    result.reason = "identity_unreadable";

    if (!TryReadInjectedPeerManagerIdentity(
            candidatePtr,
            &result.selfPid,
            &result.hostProcessHandle))
    {
        return result;
    }

    const DWORD selfPid = GetCurrentProcessId();
    if (result.selfPid != selfPid)
    {
        result.reason = "self_pid_mismatch";
        return result;
    }

    if (result.hostProcessHandle == nullptr
        || result.hostProcessHandle == INVALID_HANDLE_VALUE)
    {
        result.reason = "invalid_host_handle";
        return result;
    }

    if (!GetHandleInformation(result.hostProcessHandle, &result.handleFlags)
        || !GetExitCodeProcess(result.hostProcessHandle, &result.exitCode))
    {
        result.reason = "host_handle_query_failed";
        return result;
    }

    if (hostPid != 0
        && TryGetProcessIdFromHandleCompat(
            result.hostProcessHandle,
            &result.handlePid))
    {
        result.strictCheckUsed = true;
        if (result.handlePid != hostPid)
        {
            result.reason = "host_pid_mismatch";
            return result;
        }

        if (!ValidateInjectedPeerManagerStructure(candidatePtr, hostPid, &result))
        {
            return result;
        }

        result.valid = true;
        result.reason = "strict_match";
        return result;
    }

    if (result.exitCode != STILL_ACTIVE)
    {
        result.reason = "host_process_dead";
        return result;
    }

    if (!ValidateInjectedPeerManagerStructure(candidatePtr, hostPid, &result))
    {
        return result;
    }

    result.valid = true;
    result.reason = "fallback_alive";
    return result;
}

static void LogInjectedPeerManagerValidation(
    const char* context,
    uintptr_t candidatePtr,
    DWORD hostPid,
    const InjectedPeerManagerValidation& validation)
{
    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: %s ptr=0x%08lX hostPid=%lu valid=%d reason=%s selfPid=%lu childPid=%lu hostHandle=%p handlePid=%lu exit=%lu strict=%d structure=%d objectBase=0x%08lX objectSpan=0x%08lX layoutObj=0x%08lX peerContainer=0x%08lX peerBacking=0x%08lX quitRing=0x%08lX quitHead=%ld quitTail=%ld quitCap=%lu flags=0x%08lX",
        context != nullptr ? context : "injected peer-manager validate",
        static_cast<unsigned long>(candidatePtr),
        static_cast<unsigned long>(hostPid),
        validation.valid ? 1 : 0,
        validation.reason != nullptr ? validation.reason : "unknown",
        static_cast<unsigned long>(validation.selfPid),
        static_cast<unsigned long>(validation.childPid),
        validation.hostProcessHandle,
        static_cast<unsigned long>(validation.handlePid),
        static_cast<unsigned long>(validation.exitCode),
        validation.strictCheckUsed ? 1 : 0,
        validation.structureCheckUsed ? 1 : 0,
        static_cast<unsigned long>(validation.objectBase),
        static_cast<unsigned long>(validation.objectRegionSize),
        static_cast<unsigned long>(validation.layoutObjectSize),
        static_cast<unsigned long>(validation.peerContainerBase),
        static_cast<unsigned long>(validation.peerBackingBase),
        static_cast<unsigned long>(validation.quitRingBase),
        static_cast<long>(validation.quitHead),
        static_cast<long>(validation.quitTail),
        static_cast<unsigned long>(validation.quitCapacity),
        static_cast<unsigned long>(validation.handleFlags));

    if (validation.structureCheckUsed)
    {
        LogInjectedPeerQuitDiagnosticLine(
            "Takeover: %s layout version=%s peerContainerOff=0x%08lX peerBackingOff=0x%08lX quitRingOff=0x%08lX",
            context != nullptr ? context : "injected peer-manager validate",
            (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
                ? g_activeRevival->versionTag
                : "unknown",
            static_cast<unsigned long>(validation.layoutPeerContainerOffset),
            static_cast<unsigned long>(validation.layoutPeerBackingOffset),
            static_cast<unsigned long>(validation.layoutQuitRingOffset));
        LogInjectedWordSnapshot(
            context,
            "peerContainer",
            validation.peerContainerBase,
            validation.peerContainerWords,
            kInjectedPeerManagerDumpWordCount,
            validation.peerContainerSnapshotRead);
        LogInjectedWordSnapshot(
            context,
            "peerBacking",
            validation.peerBackingBase,
            validation.peerBackingWords,
            kInjectedPeerManagerDumpWordCount,
            validation.peerBackingSnapshotRead);
        LogInjectedWordSnapshot(
            context,
            "quitRing",
            validation.quitRingBase,
            validation.quitRingWords,
            kInjectedPeerManagerDumpWordCount,
            validation.quitRingSnapshotRead);
    }
}

static void CacheInjectedPeerManagerPointer(
    uintptr_t candidatePtr,
    DWORD hostPid,
    bool strictHostMatch)
{
    const bool changed =
        g_injectedPeerManagerCachePtr != candidatePtr
        || g_injectedPeerManagerCacheHostPid != hostPid
        || g_injectedPeerManagerCacheStrictHostMatch != strictHostMatch;

    g_injectedPeerManagerCachePtr = candidatePtr;
    g_injectedPeerManagerCacheHostPid = hostPid;
    g_injectedPeerManagerCacheStrictHostMatch = strictHostMatch;

    if (changed)
    {
        mod::Log(
            "Takeover: injected peer-manager cache store ptr=0x%08lX hostPid=%lu strictHostMatch=%d",
            static_cast<unsigned long>(candidatePtr),
            static_cast<unsigned long>(hostPid),
            strictHostMatch ? 1 : 0);
    }
}

static void InvalidateInjectedPeerManagerCache(const char* reason)
{
    if (g_injectedPeerManagerCachePtr != 0)
    {
        mod::Log(
            "Takeover: injected peer-manager cache clear ptr=0x%08lX hostPid=%lu strictHostMatch=%d reason=%s",
            static_cast<unsigned long>(g_injectedPeerManagerCachePtr),
            static_cast<unsigned long>(g_injectedPeerManagerCacheHostPid),
            g_injectedPeerManagerCacheStrictHostMatch ? 1 : 0,
            reason != nullptr ? reason : "unknown");
    }

    g_injectedPeerManagerCachePtr = 0;
    g_injectedPeerManagerCacheHostPid = 0;
    g_injectedPeerManagerCacheStrictHostMatch = false;
}

static uintptr_t TryGetCachedInjectedPeerManagerPointer(
    DWORD hostPid,
    bool* outUsedStrictHostMatch)
{
    if (outUsedStrictHostMatch != nullptr)
    {
        *outUsedStrictHostMatch = false;
    }
    if (g_injectedPeerManagerCachePtr == 0)
    {
        return 0;
    }

    const InjectedPeerManagerValidation validation =
        ValidateInjectedPeerManagerPointer(g_injectedPeerManagerCachePtr, hostPid);
    LogInjectedPeerManagerValidation(
        "injected peer-manager cache validate",
        g_injectedPeerManagerCachePtr,
        hostPid,
        validation);
    if (!validation.valid)
    {
        g_injectedPeerQuitLastDetail = EncodeInjectedPeerManagerReason(validation.reason);
        InvalidateInjectedPeerManagerCache(validation.reason);
        return 0;
    }

    g_injectedPeerQuitLastDetail = 0;

    const bool strictHostMatch =
        validation.strictCheckUsed || g_injectedPeerManagerCacheStrictHostMatch;
    CacheInjectedPeerManagerPointer(
        g_injectedPeerManagerCachePtr,
        hostPid,
        strictHostMatch);
    if (outUsedStrictHostMatch != nullptr)
    {
        *outUsedStrictHostMatch = strictHostMatch;
    }

    mod::Log(
        "Takeover: injected peer-manager cache hit ptr=0x%08lX hostPid=%lu strictHostMatch=%d",
        static_cast<unsigned long>(g_injectedPeerManagerCachePtr),
        static_cast<unsigned long>(hostPid),
        strictHostMatch ? 1 : 0);
    return g_injectedPeerManagerCachePtr;
}

static uintptr_t TryResolveInjectedPeerManagerFromSessionPointer(
    uintptr_t sessionPtr,
    const char* source,
    DWORD hostPid,
    bool* outUsedStrictHostMatch)
{
    if (outUsedStrictHostMatch != nullptr)
    {
        *outUsedStrictHostMatch = false;
    }
    if (sessionPtr == 0)
    {
        return 0;
    }

    uintptr_t managerPtr = 0;
    if (!SafeReadPtr(
            reinterpret_cast<const void*>(
                sessionPtr + kInjectedPeerManagerSessionFieldOffset),
            &managerPtr))
    {
        LogInjectedPeerQuitDiagnosticLine(
            "Takeover: injected peer-manager session source=%s session=0x%08lX managerField=unreadable fieldOff=0x%08lX",
            source != nullptr ? source : "unknown",
            static_cast<unsigned long>(sessionPtr),
            static_cast<unsigned long>(kInjectedPeerManagerSessionFieldOffset));
        return 0;
    }

    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: injected peer-manager session source=%s session=0x%08lX manager=0x%08lX fieldOff=0x%08lX",
        source != nullptr ? source : "unknown",
        static_cast<unsigned long>(sessionPtr),
        static_cast<unsigned long>(managerPtr),
        static_cast<unsigned long>(kInjectedPeerManagerSessionFieldOffset));
    if (managerPtr == 0)
    {
        return 0;
    }

    char context[96] = {};
    std::snprintf(
        context,
        sizeof(context),
        "injected peer-manager session %s",
        source != nullptr ? source : "unknown");
    const InjectedPeerManagerValidation validation =
        ValidateInjectedPeerManagerPointer(managerPtr, hostPid);
    LogInjectedPeerManagerValidation(context, managerPtr, hostPid, validation);
    if (!validation.valid)
    {
        g_injectedPeerQuitLastDetail =
            EncodeInjectedPeerManagerReason(validation.reason);
        return 0;
    }

    const bool strictHostMatch = validation.strictCheckUsed;
    CacheInjectedPeerManagerPointer(managerPtr, hostPid, strictHostMatch);
    g_injectedPeerQuitLastDetail = 0;
    if (outUsedStrictHostMatch != nullptr)
    {
        *outUsedStrictHostMatch = strictHostMatch;
    }

    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: injected peer-manager resolved source=%s session=0x%08lX ptr=0x%08lX hostPid=%lu strictHostMatch=%d",
        source != nullptr ? source : "unknown",
        static_cast<unsigned long>(sessionPtr),
        static_cast<unsigned long>(managerPtr),
        static_cast<unsigned long>(hostPid),
        strictHostMatch ? 1 : 0);
    return managerPtr;
}

static uintptr_t TryResolveInjectedPeerManagerFromSession(
    DWORD hostPid,
    bool* outUsedStrictHostMatch)
{
    if (outUsedStrictHostMatch != nullptr)
    {
        *outUsedStrictHostMatch = false;
    }

    const uintptr_t cachedSessionPtrBeforeRead = g_lastValidatedSessionPtr;
    HMODULE sessionModule = GetModuleHandleA("EfzRevival.dll");
    auto trySession = [&](uintptr_t sessionPtr, const char* source) -> uintptr_t {
        bool strictHostMatch = false;
        const uintptr_t managerPtr = TryResolveInjectedPeerManagerFromSessionPointer(
            sessionPtr,
            source,
            hostPid,
            &strictHostMatch);
        if (managerPtr != 0 && outUsedStrictHostMatch != nullptr)
        {
            *outUsedStrictHostMatch = strictHostMatch;
        }
        return managerPtr;
    };

    if (cachedSessionPtrBeforeRead != 0)
    {
        const uintptr_t managerPtr =
            trySession(cachedSessionPtrBeforeRead, "cached_preexisting");
        if (managerPtr != 0)
        {
            return managerPtr;
        }
    }

    const uintptr_t strictSessionPtr = ReadSessionPointerFromRevival();
    if (cachedSessionPtrBeforeRead == 0 && strictSessionPtr == 0 && sessionModule == nullptr)
    {
        LogInjectedPeerQuitDiagnosticLine(
            "Takeover: injected peer-manager session lookup skipped (EfzRevival.dll not loaded in helper)");
    }
    if (strictSessionPtr != 0)
    {
        const uintptr_t managerPtr = trySession(strictSessionPtr, "strict");
        if (managerPtr != 0)
        {
            return managerPtr;
        }
    }

    if (sessionModule != nullptr && g_activeRevival != nullptr)
    {
        const uintptr_t dllBase = reinterpret_cast<uintptr_t>(sessionModule);
        for (size_t index = 0; index < g_activeRevival->sessionPtrOffsetCount; ++index)
        {
            const uintptr_t offset = g_activeRevival->sessionPtrOffsets[index];
            if (offset == 0)
            {
                continue;
            }

            uintptr_t rawSessionPtr = 0;
            if (!SafeReadPtr(reinterpret_cast<const void*>(dllBase + offset), &rawSessionPtr)
                || rawSessionPtr == 0
                || rawSessionPtr == strictSessionPtr
                || rawSessionPtr == cachedSessionPtrBeforeRead)
            {
                continue;
            }

            char source[48] = {};
            std::snprintf(
                source,
                sizeof(source),
                "raw_global[%lu]",
                static_cast<unsigned long>(index));
            const uintptr_t managerPtr = trySession(rawSessionPtr, source);
            if (managerPtr != 0)
            {
                return managerPtr;
            }
        }
    }

    const uintptr_t looseSessionPtr = ReadSessionPointerFromRevivalLoose();
    if (looseSessionPtr != 0
        && looseSessionPtr != strictSessionPtr
        && looseSessionPtr != cachedSessionPtrBeforeRead)
    {
        const uintptr_t managerPtr = trySession(looseSessionPtr, "loose");
        if (managerPtr != 0)
        {
            return managerPtr;
        }
    }

    return 0;
}

static uintptr_t FindInjectedPeerManagerPointer(DWORD hostPid, bool* outUsedStrictHostMatch)
{
    if (outUsedStrictHostMatch != nullptr)
    {
        *outUsedStrictHostMatch = false;
    }

    const DWORD selfPid = GetCurrentProcessId();
    const bool hostPidKnown = hostPid != 0;
    bool usedStrictHostMatch = false;
    SIZE_T regionCount = 0;
    SIZE_T writableRegionCount = 0;
    SIZE_T selfPidHits = 0;
    SIZE_T invalidHandleRejects = 0;
    SIZE_T handleQueryRejects = 0;
    SIZE_T strictPidMatches = 0;
    SIZE_T strictPidMismatches = 0;
    SIZE_T fallbackAliveMatches = 0;
    SIZE_T fallbackDeadRejects = 0;
    DWORD lastValidationDetail = 0;
    std::vector<uintptr_t> candidates;

    const uintptr_t cachedPtr =
        TryGetCachedInjectedPeerManagerPointer(hostPid, &usedStrictHostMatch);
    if (cachedPtr != 0)
    {
        if (outUsedStrictHostMatch != nullptr)
        {
            *outUsedStrictHostMatch = usedStrictHostMatch;
        }
        return cachedPtr;
    }

    const uintptr_t sessionManagerPtr =
        TryResolveInjectedPeerManagerFromSession(hostPid, &usedStrictHostMatch);
    if (sessionManagerPtr != 0)
    {
        if (outUsedStrictHostMatch != nullptr)
        {
            *outUsedStrictHostMatch = usedStrictHostMatch;
        }
        return sessionManagerPtr;
    }

    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: injected peer-manager scan start selfPid=%lu hostPid=%lu hostPidKnown=%d version=%s",
        static_cast<unsigned long>(selfPid),
        static_cast<unsigned long>(hostPid),
        hostPidKnown ? 1 : 0,
        (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
            ? g_activeRevival->versionTag
            : "unknown");
    LogInjectedPeerManagerLayout("injected peer-manager scan layout");

    SYSTEM_INFO sysInfo = {};
    GetSystemInfo(&sysInfo);

    uintptr_t cursor = reinterpret_cast<uintptr_t>(sysInfo.lpMinimumApplicationAddress);
    const uintptr_t maxAddress = reinterpret_cast<uintptr_t>(sysInfo.lpMaximumApplicationAddress);
    while (cursor < maxAddress)
    {
        ++regionCount;
        MEMORY_BASIC_INFORMATION mbi = {};
        const SIZE_T queried = VirtualQuery(
            reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi));
        if (queried != sizeof(mbi))
        {
            LogInjectedPeerQuitDiagnosticLine(
                "Takeover: injected peer-manager scan stopped cursor=0x%08lX queried=%lu err=%lu",
                static_cast<unsigned long>(cursor),
                static_cast<unsigned long>(queried),
                static_cast<unsigned long>(GetLastError()));
            break;
        }

        if (IsWritableUserRegion(mbi)
            && mbi.RegionSize >= kInjectedPeerManagerMinScanSpan)
        {
            ++writableRegionCount;
            const auto* regionBase = static_cast<const uint8_t*>(mbi.BaseAddress);
            const SIZE_T limit = mbi.RegionSize - kInjectedPeerManagerMinScanSpan;
            for (SIZE_T off = 0; off <= limit; off += sizeof(uint32_t))
            {
                const auto* candidate = regionBase + off;
                if (*reinterpret_cast<const DWORD*>(candidate + kInjectedPeerManagerSelfPidOffset)
                    != selfPid)
                {
                    continue;
                }
                ++selfPidHits;

                const HANDLE hostProcessHandle =
                    *reinterpret_cast<HANDLE const*>(candidate + kInjectedPeerManagerHostHandleOffset);
                if (hostProcessHandle == nullptr || hostProcessHandle == INVALID_HANDLE_VALUE)
                {
                    ++invalidHandleRejects;
                    continue;
                }

                DWORD handleFlags = 0;
                DWORD exitCode = 0;
                if (!GetHandleInformation(hostProcessHandle, &handleFlags)
                    || !GetExitCodeProcess(hostProcessHandle, &exitCode))
                {
                    ++handleQueryRejects;
                    continue;
                }

                bool accept = false;
                DWORD handlePid = 0;
                bool strictCheckUsed = false;
                if (hostPidKnown && TryGetProcessIdFromHandleCompat(hostProcessHandle, &handlePid))
                {
                    strictCheckUsed = true;
                    accept = (handlePid == hostPid);
                    usedStrictHostMatch = true;
                    if (accept)
                    {
                        ++strictPidMatches;
                    }
                    else
                    {
                        ++strictPidMismatches;
                    }
                }
                else
                {
                    accept = (exitCode == STILL_ACTIVE);
                    if (accept)
                    {
                        ++fallbackAliveMatches;
                    }
                    else
                    {
                        ++fallbackDeadRejects;
                    }
                }

                if (!accept)
                {
                    continue;
                }

                const uintptr_t candidatePtr =
                    reinterpret_cast<uintptr_t>(candidate);
                const InjectedPeerManagerValidation validation =
                    ValidateInjectedPeerManagerPointer(candidatePtr, hostPid);
                LogInjectedPeerManagerValidation(
                    validation.valid
                        ? "injected peer-manager scan accept"
                        : "injected peer-manager scan reject",
                    candidatePtr,
                    hostPid,
                    validation);
                if (!validation.valid)
                {
                    lastValidationDetail = EncodeInjectedPeerManagerReason(validation.reason);
                    continue;
                }

                lastValidationDetail = 0;
                usedStrictHostMatch = usedStrictHostMatch || validation.strictCheckUsed;
                candidates.push_back(candidatePtr);
                if (candidates.size() > 8u)
                {
                    break;
                }
            }
        }

        const uintptr_t next =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + static_cast<uintptr_t>(mbi.RegionSize);
        if (next <= cursor)
        {
            break;
        }
        cursor = next;
    }

    if (outUsedStrictHostMatch != nullptr)
    {
        *outUsedStrictHostMatch = usedStrictHostMatch;
    }

    g_injectedPeerQuitLastDetail = lastValidationDetail;

    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: injected peer-manager scan summary regions=%lu writable=%lu "
        "selfPidHits=%lu invalidHandle=%lu handleQueryFail=%lu strictMatch=%lu "
        "strictMismatch=%lu fallbackAlive=%lu fallbackDead=%lu candidates=%u",
        static_cast<unsigned long>(regionCount),
        static_cast<unsigned long>(writableRegionCount),
        static_cast<unsigned long>(selfPidHits),
        static_cast<unsigned long>(invalidHandleRejects),
        static_cast<unsigned long>(handleQueryRejects),
        static_cast<unsigned long>(strictPidMatches),
        static_cast<unsigned long>(strictPidMismatches),
        static_cast<unsigned long>(fallbackAliveMatches),
        static_cast<unsigned long>(fallbackDeadRejects),
        static_cast<unsigned>(candidates.size()));

    if (candidates.size() != 1u)
    {
        LogInjectedPeerQuitDiagnosticLine(
            "Takeover: injected peer-manager scan ambiguous hostPid=%lu strictHostMatch=%d candidates=%u",
            static_cast<unsigned long>(hostPid),
            usedStrictHostMatch ? 1 : 0,
            static_cast<unsigned>(candidates.size()));
        return 0;
    }

    CacheInjectedPeerManagerPointer(
        candidates.front(),
        hostPid,
        usedStrictHostMatch);

    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: injected peer-manager resolved ptr=0x%08lX hostPid=%lu strictHostMatch=%d",
        static_cast<unsigned long>(candidates.front()),
        static_cast<unsigned long>(hostPid),
        usedStrictHostMatch ? 1 : 0);
    return candidates.front();
}

enum InjectedPeerQuitBroadcastResult : DWORD
{
    kInjectedPeerQuitBroadcastResultSuccess = 1,
    kInjectedPeerQuitBroadcastResultNotRevival = 2,
    kInjectedPeerQuitBroadcastResultUnsupportedVersion = 3,
    kInjectedPeerQuitBroadcastResultManagerUnresolved = 4,
    kInjectedPeerQuitBroadcastResultHelperBaseUnresolved = 5,
    kInjectedPeerQuitBroadcastResultNativeCallCrashed = 6,
    kInjectedPeerQuitBroadcastResultInvalidGuardContext = 7,
};

constexpr DWORD kInjectedPeerQuitBroadcastResultMask = 0xFFu;

bool TryClassifyRevivalQuitEndpoint(
    const char* endpointText,
    bool* outIsActivePeer,
    bool* outIsSpectator)
{
    if (outIsActivePeer != nullptr)
    {
        *outIsActivePeer = false;
    }
    if (outIsSpectator != nullptr)
    {
        *outIsSpectator = false;
    }

    if (!IsCurrentProcessRevival())
    {
        mod::Log(
            "Takeover: native quit packet classification skipped endpoint='%s' (not in EfzRevival.exe)",
            endpointText != nullptr ? endpointText : "");
        return false;
    }

    EnsureActiveRevivalProfile();

    RevivalQuitEndpointStorage endpoint = {};
    if (!TryBuildRevivalQuitEndpointFromText(endpointText, &endpoint))
    {
        mod::Log(
            "Takeover: native quit packet classification failed endpoint='%s' reason=parse_failed version=%s",
            endpointText != nullptr ? endpointText : "",
            (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
                ? g_activeRevival->versionTag
                : "unknown");
        return false;
    }

    const uintptr_t roleCheckRva = ResolveHelperQuitRoleCheckRva();
    const uintptr_t helperBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (roleCheckRva == 0 || helperBase == 0)
    {
        mod::Log(
            "Takeover: native quit packet classification failed endpoint='%s' reason=helper_unresolved version=%s roleCheckRva=0x%08lX helperBase=0x%08lX",
            endpointText != nullptr ? endpointText : "",
            (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
                ? g_activeRevival->versionTag
                : "unknown",
            static_cast<unsigned long>(roleCheckRva),
            static_cast<unsigned long>(helperBase));
        return false;
    }

    bool isActivePeer = false;
    bool isSpectator = false;
    DWORD exceptionCode = 0;
    const char* const versionTag =
        (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
            ? g_activeRevival->versionTag
            : "unknown";

    if (std::strcmp(versionTag, "1.02i") == 0
        || std::strcmp(versionTag, "1.02j") == 0)
    {
        if (!HasInjectedContext())
        {
            (void)EnsureInjectedContextFast();
        }

        const DWORD hostPid =
            (g_injectedBlock != nullptr) ? static_cast<DWORD>(g_injectedBlock->hostPid) : 0;
        bool usedStrictHostMatch = false;
        const uintptr_t managerPtr =
            FindInjectedPeerManagerPointer(hostPid, &usedStrictHostMatch);
        const InjectedPeerManagerLayout layout = ResolveInjectedPeerManagerLayout();
        if (managerPtr == 0 || layout.peerContainerOffset == 0)
        {
            mod::Log(
                "Takeover: native quit packet classification failed endpoint='%s' reason=manager_unresolved version=%s hostPid=%lu manager=0x%08lX peerContainerOff=0x%08lX",
                endpointText != nullptr ? endpointText : "",
                versionTag,
                static_cast<unsigned long>(hostPid),
                static_cast<unsigned long>(managerPtr),
                static_cast<unsigned long>(layout.peerContainerOffset));
            return false;
        }

        const uintptr_t peerContainerPtr = managerPtr + layout.peerContainerOffset;
        typedef bool (__thiscall *RoleCheckFn)(void*, const void*, int);
        const auto roleCheck =
            reinterpret_cast<RoleCheckFn>(helperBase + roleCheckRva);
        __try
        {
            isActivePeer = roleCheck(
                reinterpret_cast<void*>(peerContainerPtr),
                &endpoint,
                2);
            isSpectator = roleCheck(
                reinterpret_cast<void*>(peerContainerPtr),
                &endpoint,
                3);
        }
        __except (exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            mod::Log(
                "Takeover: native quit packet classification failed endpoint='%s' reason=exception version=%s helperBase=0x%08lX roleCheckRva=0x%08lX manager=0x%08lX peerContainer=0x%08lX exception=0x%08lX",
                endpointText != nullptr ? endpointText : "",
                versionTag,
                static_cast<unsigned long>(helperBase),
                static_cast<unsigned long>(roleCheckRva),
                static_cast<unsigned long>(managerPtr),
                static_cast<unsigned long>(peerContainerPtr),
                static_cast<unsigned long>(exceptionCode));
            return false;
        }

        mod::Log(
            "Takeover: native quit packet classified endpoint='%s' version=%s roleCheckRva=0x%08lX manager=0x%08lX peerContainer=0x%08lX strictHostMatch=%d activePeer=%d spectator=%d",
            endpointText != nullptr ? endpointText : "",
            versionTag,
            static_cast<unsigned long>(roleCheckRva),
            static_cast<unsigned long>(managerPtr),
            static_cast<unsigned long>(peerContainerPtr),
            usedStrictHostMatch ? 1 : 0,
            isActivePeer ? 1 : 0,
            isSpectator ? 1 : 0);
    }
    else
    {
        typedef bool (__stdcall *RoleCheckFn)(const void*, int);
        const auto roleCheck =
            reinterpret_cast<RoleCheckFn>(helperBase + roleCheckRva);
        __try
        {
            isActivePeer = roleCheck(&endpoint, 2);
            isSpectator = roleCheck(&endpoint, 3);
        }
        __except (exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            mod::Log(
                "Takeover: native quit packet classification failed endpoint='%s' reason=exception version=%s helperBase=0x%08lX roleCheckRva=0x%08lX exception=0x%08lX",
                endpointText != nullptr ? endpointText : "",
                versionTag,
                static_cast<unsigned long>(helperBase),
                static_cast<unsigned long>(roleCheckRva),
                static_cast<unsigned long>(exceptionCode));
            return false;
        }

        mod::Log(
            "Takeover: native quit packet classified endpoint='%s' version=%s roleCheckRva=0x%08lX activePeer=%d spectator=%d",
            endpointText != nullptr ? endpointText : "",
            versionTag,
            static_cast<unsigned long>(roleCheckRva),
            isActivePeer ? 1 : 0,
            isSpectator ? 1 : 0);
    }

    if (outIsActivePeer != nullptr)
    {
        *outIsActivePeer = isActivePeer;
    }
    if (outIsSpectator != nullptr)
    {
        *outIsSpectator = isSpectator;
    }
    return true;
}
constexpr unsigned kInjectedPeerQuitBroadcastDetailShift = 8u;

static DWORD EncodeInjectedPeerQuitBroadcastExitCode(
    InjectedPeerQuitBroadcastResult result,
    DWORD detail)
{
    return (detail << kInjectedPeerQuitBroadcastDetailShift)
        | (static_cast<DWORD>(result) & kInjectedPeerQuitBroadcastResultMask);
}

static DWORD DecodeInjectedPeerQuitBroadcastResultCode(DWORD exitCode)
{
    return exitCode & kInjectedPeerQuitBroadcastResultMask;
}

static DWORD DecodeInjectedPeerQuitBroadcastDetailCode(DWORD exitCode)
{
    return exitCode >> kInjectedPeerQuitBroadcastDetailShift;
}

static const char* InjectedPeerQuitBroadcastResultToString(DWORD result)
{
    switch (result)
    {
    case kInjectedPeerQuitBroadcastResultSuccess:
        return "success";
    case kInjectedPeerQuitBroadcastResultNotRevival:
        return "not_revival";
    case kInjectedPeerQuitBroadcastResultUnsupportedVersion:
        return "unsupported_version";
    case kInjectedPeerQuitBroadcastResultManagerUnresolved:
        return "manager_unresolved";
    case kInjectedPeerQuitBroadcastResultHelperBaseUnresolved:
        return "helper_base_unresolved";
    case kInjectedPeerQuitBroadcastResultNativeCallCrashed:
        return "native_call_crashed";
    case kInjectedPeerQuitBroadcastResultInvalidGuardContext:
        return "invalid_guard_context";
    default:
        return "unknown";
    }
}

static DWORD FinishInjectedPeerQuitBroadcast(InjectedPeerQuitBroadcastResult result)
{
    const DWORD detail = g_injectedPeerQuitLastDetail;
    const DWORD exitCode = EncodeInjectedPeerQuitBroadcastExitCode(result, detail);
    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: injected peer-quit broadcast returning code=%lu result=%s detail=%lu",
        static_cast<unsigned long>(exitCode),
        InjectedPeerQuitBroadcastResultToString(static_cast<DWORD>(result)),
        static_cast<unsigned long>(detail));
    mod::FlushLoggerSync();
    return exitCode;
}

DWORD RunInjectedPeerQuitBroadcast()
{
    g_injectedPeerQuitLastDetail = 0;
    ClearPeerQuitDiagnostic();
    const bool narrowExternalGuard = IsExternalLauncherGuardProcess();
    if (!IsCurrentProcessRevival() && !narrowExternalGuard)
    {
        LogInjectedPeerQuitDiagnosticLine("Takeover: injected peer-quit broadcast skipped (not in EfzRevival.exe)");
        return FinishInjectedPeerQuitBroadcast(kInjectedPeerQuitBroadcastResultNotRevival);
    }

    if (!narrowExternalGuard && !HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }

    DWORD hostPid = 0;
    uint32_t guardRevivalTimestamp = 0;
    if (narrowExternalGuard)
    {
        if (!GetExternalLauncherGuardChildProcessId(
                &hostPid,
                &guardRevivalTimestamp)
            || hostPid == 0)
        {
            LogInjectedPeerQuitDiagnosticLine(
                "Takeover: injected peer-quit broadcast refused (external guard child identity unavailable)");
            return FinishInjectedPeerQuitBroadcast(
                kInjectedPeerQuitBroadcastResultInvalidGuardContext);
        }
        const RevivalAddressProfile* const guardProfile =
            FindRevivalProfileByTimestamp(guardRevivalTimestamp);
        if (guardProfile == nullptr)
        {
            LogInjectedPeerQuitDiagnosticLine(
                "Takeover: injected peer-quit broadcast refused (external guard profile unavailable timestamp=0x%08lX)",
                static_cast<unsigned long>(guardRevivalTimestamp));
            return FinishInjectedPeerQuitBroadcast(
                kInjectedPeerQuitBroadcastResultInvalidGuardContext);
        }
        SetActiveRevivalProfile(
            guardProfile,
            RevivalProfileSource::ExternalLauncherGuard);
    }
    else
    {
        DetectRevivalVersion();
        if (g_injectedBlock != nullptr)
        {
            hostPid = static_cast<DWORD>(g_injectedBlock->hostPid);
        }
    }
    const uintptr_t sendQuitAllRva = ResolveHelperSendQuitAllRva();
    const InjectedPeerManagerLayout layout = ResolveInjectedPeerManagerLayout();
    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: injected peer-quit broadcast begin ready=%d hostPid=%lu version=%s sendQuitAllRva=0x%08lX layoutObj=0x%08lX peerContainerOff=0x%08lX peerBackingOff=0x%08lX quitRingOff=0x%08lX cachedManager=0x%08lX cachedHostPid=%lu cachedStrict=%d",
        (HasInjectedContext() || narrowExternalGuard) ? 1 : 0,
        static_cast<unsigned long>(hostPid),
        (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
            ? g_activeRevival->versionTag
            : "unknown",
        static_cast<unsigned long>(sendQuitAllRva),
        static_cast<unsigned long>(layout.objectSize),
        static_cast<unsigned long>(layout.peerContainerOffset),
        static_cast<unsigned long>(layout.peerBackingOffset),
        static_cast<unsigned long>(layout.quitRingOffset),
        static_cast<unsigned long>(g_injectedPeerManagerCachePtr),
        static_cast<unsigned long>(g_injectedPeerManagerCacheHostPid),
        g_injectedPeerManagerCacheStrictHostMatch ? 1 : 0);
    if (sendQuitAllRva == 0)
    {
        LogInjectedPeerQuitDiagnosticLine(
            "Takeover: injected peer-quit broadcast skipped (unsupported helper version=%s)",
            (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
                ? g_activeRevival->versionTag
                : "unknown");
        return FinishInjectedPeerQuitBroadcast(kInjectedPeerQuitBroadcastResultUnsupportedVersion);
    }

    bool usedStrictHostMatch = false;
    const uintptr_t managerPtr =
        FindInjectedPeerManagerPointer(hostPid, &usedStrictHostMatch);
    if (managerPtr == 0)
    {
        if (g_injectedPeerQuitLastDetail == 0)
        {
            g_injectedPeerQuitLastDetail = 255;
        }
        LogInjectedPeerQuitDiagnosticLine(
            "Takeover: injected peer-quit broadcast failed (manager unresolved) hostPid=%lu cachedManager=0x%08lX cachedHostPid=%lu cachedStrict=%d",
            static_cast<unsigned long>(hostPid),
            static_cast<unsigned long>(g_injectedPeerManagerCachePtr),
            static_cast<unsigned long>(g_injectedPeerManagerCacheHostPid),
            g_injectedPeerManagerCacheStrictHostMatch ? 1 : 0);
        return FinishInjectedPeerQuitBroadcast(kInjectedPeerQuitBroadcastResultManagerUnresolved);
    }

    const uintptr_t helperBase =
        reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (helperBase == 0)
    {
        LogInjectedPeerQuitDiagnosticLine("Takeover: injected peer-quit broadcast failed (helper base unresolved)");
        return FinishInjectedPeerQuitBroadcast(kInjectedPeerQuitBroadcastResultHelperBaseUnresolved);
    }

    typedef void (__thiscall *SendQuitAllFn)(void*);
    const auto sendQuitAll =
        reinterpret_cast<SendQuitAllFn>(helperBase + sendQuitAllRva);
    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: injected peer-quit broadcast invoking helperBase=0x%08lX manager=0x%08lX target=0x%08lX",
        static_cast<unsigned long>(helperBase),
        static_cast<unsigned long>(managerPtr),
        static_cast<unsigned long>(helperBase + sendQuitAllRva));

    DWORD exceptionCode = 0;
    __try
    {
        sendQuitAll(reinterpret_cast<void*>(managerPtr));
    }
    __except (exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        g_injectedPeerQuitLastDetail = EncodeInjectedExceptionDetail(exceptionCode);
        LogInjectedPeerQuitDiagnosticLine(
            "Takeover: injected peer-quit broadcast crashed manager=0x%08lX rva=0x%08lX exception=0x%08lX detail=%s",
            static_cast<unsigned long>(managerPtr),
            static_cast<unsigned long>(sendQuitAllRva),
            static_cast<unsigned long>(exceptionCode),
            DecodeInjectedExceptionDetail(g_injectedPeerQuitLastDetail));
        return FinishInjectedPeerQuitBroadcast(kInjectedPeerQuitBroadcastResultNativeCallCrashed);
    }
    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: injected peer-quit broadcast native call returned manager=0x%08lX",
        static_cast<unsigned long>(managerPtr));

    // Native helper paths send MessageQuit and then immediately continue into
    // their own shutdown logic. Give the packet a small head start before the
    // host tears the helper down from the outside.
    Sleep(100u);
    LogInjectedPeerQuitDiagnosticLine(
        "Takeover: injected peer-quit broadcast sent manager=0x%08lX rva=0x%08lX hostPid=%lu strictHostMatch=%d",
        static_cast<unsigned long>(managerPtr),
        static_cast<unsigned long>(sendQuitAllRva),
        static_cast<unsigned long>(hostPid),
        usedStrictHostMatch ? 1 : 0);
    return FinishInjectedPeerQuitBroadcast(kInjectedPeerQuitBroadcastResultSuccess);
}

static uintptr_t ResolveRemoteSelfExportAddress(
    uintptr_t remoteSelfBase,
    const void* localExport)
{
    if (remoteSelfBase == 0 || localExport == nullptr)
    {
        return 0;
    }

    HMODULE selfModule = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(localExport),
            &selfModule)
        || selfModule == nullptr)
    {
        return 0;
    }

    const uintptr_t localBase = reinterpret_cast<uintptr_t>(selfModule);
    const uintptr_t localAddr = reinterpret_cast<uintptr_t>(localExport);
    return remoteSelfBase + (localAddr - localBase);
}

static bool RequestInjectedPeerQuitBroadcastImpl(
    HANDLE helperProcess,
    uintptr_t remoteSelfBase,
    DWORD helperPid,
    const char* reason,
    DWORD waitMs)
{
    mod::Log(
        "Takeover: peer-quit broadcast request reason='%s' waitMs=%lu helperPid=%lu helperDup=%p remoteSelfBase=0x%08lX",
        reason != nullptr ? reason : "",
        static_cast<unsigned long>(waitMs),
        static_cast<unsigned long>(helperPid),
        helperProcess,
        static_cast<unsigned long>(remoteSelfBase));

    if (helperProcess == nullptr || remoteSelfBase == 0)
    {
        if (helperProcess != nullptr)
        {
            CloseHandle(helperProcess);
        }
        mod::Log(
            "Takeover: peer-quit broadcast skipped reason='%s' helper=%p remoteSelfBase=0x%08lX",
            reason != nullptr ? reason : "",
            helperProcess,
            static_cast<unsigned long>(remoteSelfBase));
        return false;
    }

    const uintptr_t remoteStart =
        ResolveRemoteSelfExportAddress(
            remoteSelfBase,
            reinterpret_cast<const void*>(&nb_stub_RequestPeerQuitBroadcast));
    if (remoteStart == 0)
    {
        CloseHandle(helperProcess);
        mod::Log(
            "Takeover: peer-quit broadcast failed reason='%s' (remote export unresolved)",
            reason != nullptr ? reason : "");
        return false;
    }
    mod::Log(
        "Takeover: peer-quit broadcast resolved remote export start=0x%08lX helperPid=%lu",
        static_cast<unsigned long>(remoteStart),
        static_cast<unsigned long>(helperPid));

    DWORD threadId = 0;
    HANDLE remoteThread = CreateRemoteThread(
        helperProcess,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteStart),
        nullptr,
        0,
        &threadId);
    if (remoteThread == nullptr)
    {
        const DWORD err = GetLastError();
        CloseHandle(helperProcess);
        mod::Log(
            "Takeover: peer-quit broadcast failed reason='%s' "
            "CreateRemoteThread err=%lu start=0x%08lX",
            reason != nullptr ? reason : "",
            static_cast<unsigned long>(err),
            static_cast<unsigned long>(remoteStart));
        return false;
    }
    mod::Log(
        "Takeover: peer-quit broadcast remote thread created handle=%p tid=%lu reason='%s'",
        remoteThread,
        static_cast<unsigned long>(threadId),
        reason != nullptr ? reason : "");

    ClearPeerQuitDiagnostic();
    const DWORD waitResult = WaitForSingleObject(remoteThread, waitMs);
    DWORD exitCode = 0;
    const char* exitMeaning = "not_waited";
    if (waitResult == WAIT_OBJECT_0)
    {
        (void)GetExitCodeThread(remoteThread, &exitCode);
        const DWORD exitResult = DecodeInjectedPeerQuitBroadcastResultCode(exitCode);
        const DWORD exitDetail = DecodeInjectedPeerQuitBroadcastDetailCode(exitCode);
        exitMeaning = InjectedPeerQuitBroadcastResultToString(exitResult);
        if (exitResult != kInjectedPeerQuitBroadcastResultSuccess)
        {
            const char* detailMeaning = "";
            if (exitResult == kInjectedPeerQuitBroadcastResultManagerUnresolved)
            {
                detailMeaning = DecodeInjectedPeerManagerReason(exitDetail);
            }
            else if (exitResult == kInjectedPeerQuitBroadcastResultNativeCallCrashed)
            {
                detailMeaning = DecodeInjectedExceptionDetail(exitDetail);
            }
            mod::Log(
                "Takeover: peer-quit broadcast helper returned failure reason='%s' exit=%lu (%s) detail=%lu (%s) helperPid=%lu tid=%lu",
                reason != nullptr ? reason : "",
                static_cast<unsigned long>(exitResult),
                exitMeaning,
                static_cast<unsigned long>(exitDetail),
                detailMeaning,
                static_cast<unsigned long>(helperPid),
                static_cast<unsigned long>(threadId));
        }
        exitCode = exitResult;
    }
    else if (waitResult == WAIT_TIMEOUT)
    {
        exitMeaning = "timeout";
        mod::Log(
            "Takeover: peer-quit broadcast wait timed out reason='%s' waitMs=%lu tid=%lu",
            reason != nullptr ? reason : "",
            static_cast<unsigned long>(waitMs),
            static_cast<unsigned long>(threadId));
    }
    else
    {
        exitMeaning = "wait_failed";
        mod::Log(
            "Takeover: peer-quit broadcast wait failed reason='%s' wait=%lu err=%lu tid=%lu",
            reason != nullptr ? reason : "",
            static_cast<unsigned long>(waitResult),
            static_cast<unsigned long>(GetLastError()),
            static_cast<unsigned long>(threadId));
    }

            FlushInjectedPeerQuitDiagnosticToHostLog(reason, helperPid);
    CloseHandle(remoteThread);
    CloseHandle(helperProcess);

    mod::Log(
        "Takeover: peer-quit broadcast reason='%s' wait=%lu exit=%lu exitMeaning=%s tid=%lu start=0x%08lX helperPid=%lu",
        reason != nullptr ? reason : "",
        static_cast<unsigned long>(waitResult),
        static_cast<unsigned long>(exitCode),
        exitMeaning,
        static_cast<unsigned long>(threadId),
        static_cast<unsigned long>(remoteStart),
        static_cast<unsigned long>(helperPid));
    return waitResult == WAIT_OBJECT_0 && exitCode == kInjectedPeerQuitBroadcastResultSuccess;
}

bool RequestInjectedPeerQuitBroadcast(const char* reason, DWORD waitMs)
{
    HANDLE helperProcess = nullptr;
    HANDLE sourceHelperProcess = nullptr;
    uintptr_t remoteSelfBase = 0;
    DWORD helperPid = 0;
    DWORD duplicateErr = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        remoteSelfBase = g_remoteInjectedSelfBase;
        if (g_peerProcessOwnership
            == PeerProcessOwnership::ExternalLauncherParent)
        {
            remoteSelfBase = g_externalLauncherGuardRemoteBase;
        }
        helperPid = g_revivalProcessId;
        sourceHelperProcess = g_revivalProcess;
        if (g_revivalProcess != nullptr)
        {
            if (!DuplicateHandle(
                    GetCurrentProcess(),
                    g_revivalProcess,
                    GetCurrentProcess(),
                    &helperProcess,
                    0,
                    FALSE,
                    DUPLICATE_SAME_ACCESS))
            {
                duplicateErr = GetLastError();
            }
        }
    }

    mod::Log(
        "Takeover: peer-quit broadcast prepare reason='%s' sourceHelper=%p helperDup=%p helperPid=%lu remoteSelfBase=0x%08lX dupErr=%lu localRole=%d netRole=%d",
        reason != nullptr ? reason : "",
        sourceHelperProcess,
        helperProcess,
        static_cast<unsigned long>(helperPid),
        static_cast<unsigned long>(remoteSelfBase),
        static_cast<unsigned long>(duplicateErr),
        g_localRoleFlag,
        g_netplayRole);

    if (helperPid != 0)
    {
        const LONG trackedPid = InterlockedCompareExchange(
            &g_peerQuitBroadcastHelperPid, 0, 0);
        if (trackedPid != static_cast<LONG>(helperPid))
        {
            InterlockedExchange(&g_peerQuitBroadcastState, 0);
            InterlockedExchange(
                &g_peerQuitBroadcastHelperPid,
                static_cast<LONG>(helperPid));
        }

        LONG state = InterlockedCompareExchange(&g_peerQuitBroadcastState, 0, 0);
        if (state == 2)
        {
            if (helperProcess != nullptr)
            {
                CloseHandle(helperProcess);
            }
            mod::Log(
                "Takeover: peer-quit broadcast coalesced reason='%s' helperPid=%lu state=already_sent",
                reason != nullptr ? reason : "",
                static_cast<unsigned long>(helperPid));
            return true;
        }

        if (InterlockedCompareExchange(&g_peerQuitBroadcastState, 1, 0) != 0)
        {
            if (helperProcess != nullptr)
            {
                CloseHandle(helperProcess);
            }

            const DWORD waitStart = GetTickCount();
            do
            {
                state = InterlockedCompareExchange(&g_peerQuitBroadcastState, 0, 0);
                if (state != 1)
                {
                    break;
                }
                Sleep(1u);
            }
            while (GetTickCount() - waitStart < waitMs);

            const bool priorSucceeded =
                InterlockedCompareExchange(&g_peerQuitBroadcastState, 0, 0) == 2;
            mod::Log(
                "Takeover: peer-quit broadcast coalesced reason='%s' helperPid=%lu state=%s",
                reason != nullptr ? reason : "",
                static_cast<unsigned long>(helperPid),
                priorSucceeded ? "prior_success" : "prior_incomplete");
            return priorSucceeded;
        }
    }

    const bool sent = RequestInjectedPeerQuitBroadcastImpl(
        helperProcess,
        remoteSelfBase,
        helperPid,
        reason,
        waitMs);
    if (helperPid != 0)
    {
        InterlockedExchange(&g_peerQuitBroadcastState, sent ? 2 : 0);
    }
    return sent;
}

void ResetInjectedPeerQuitBroadcastState()
{
    InterlockedExchange(&g_peerQuitBroadcastState, 0);
    InterlockedExchange(&g_peerQuitBroadcastHelperPid, 0);
}

// ---------------------------------------------------------------------------
// Job object helpers
// ---------------------------------------------------------------------------

static HANDLE EnsureChildJobObject()
{
    if (g_childJobObject != nullptr)
        return g_childJobObject;

    g_childJobObject = CreateJobObjectA(nullptr, nullptr);
    if (g_childJobObject == nullptr)
    {
        mod::Log("Takeover: CreateJobObject failed err=%lu",
                 static_cast<unsigned long>(GetLastError()));
        return nullptr;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(g_childJobObject,
                                 JobObjectExtendedLimitInformation,
                                 &jeli, sizeof(jeli)))
    {
        mod::Log("Takeover: SetInformationJobObject failed err=%lu",
                 static_cast<unsigned long>(GetLastError()));
        CloseHandle(g_childJobObject);
        g_childJobObject = nullptr;
        return nullptr;
    }

    mod::Log("Takeover: created kill-on-close job object handle=%p",
             g_childJobObject);
    return g_childJobObject;
}

static void CloseChildJobObject()
{
    if (g_childJobObject != nullptr)
    {
        // Closing the handle triggers JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
        // terminating any surviving child processes.
        mod::Log("Takeover: closing child job object handle=%p",
                 g_childJobObject);
        CloseHandle(g_childJobObject);
        g_childJobObject = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Session lifecycle functions (public API from revival_takeover.h).
// ---------------------------------------------------------------------------

uint32_t BeginManagedSessionBoundary(const char* reason)
{
    // This authority owns only mod-side latches shared by direct sessions and
    // exact launcher-first adoption. Call it exactly once for a committed
    // session generation. Native object/input/INI work remains at the direct
    // StartSession call site.
    ResetForceLocalPlayInitCount();
    ResetGameModeValidation();
    ClearLocalProcessCloseForGameplayStall();
    netplay::bridge::recovery::ResetGameplayExitRecoveryCompletion();
    return netplay::bridge::session_lifecycle::BeginSessionBoundary(reason);
}

bool InitializeHost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    // Resolve launcher-first ownership before opening/truncating logEfz,
    // creating ordinary host IPC, or touching any saved host override.
    // Launcher-owned Tournament is attached without a second init; resolving
    // that ownership first prevents ordinary host setup from replacing role 3.
    if (!EnsureLocalRevivalLoaded())
    {
        mod::Log("Takeover: host local revival load failed");
        return false;
    }

    if (g_launchDisposition
        == revival_launch::LaunchDisposition::AttachExistingTournament)
    {
        // The launcher already owns the role-3 object and native navigation.
        // EnsureLocalRevivalLoaded performed the exact attach transaction;
        // ordinary host log/Protocol/IPC recovery has no role in Tournament
        // and would only add side effects before the parked UI commit.
        mod::Log(
            "Takeover: launcher-owned Tournament initialized without ordinary host control-plane startup");
        return true;
    }

    if (!RecoverTemporaryHostProtocolOverride(
            "mod startup"))
    {
        mod::Log(
            "Takeover: Host Protocol crash recovery remains pending; "
            "new Host attempts will stay blocked until it can be resolved");
    }
    CleanupNativeHostShadowLogDirectory("startup");
    PrimeManagedLogEfzHistory();
    (void)EnsureHostIpc();

    // DetectRevivalVersion() is now called inside EnsureLocalRevivalLoaded()
    // before any profile-dependent operations (frame hook, etc.).
    if (g_launchDisposition
            != revival_launch::LaunchDisposition::AdoptExternalOnline
        && g_launchDisposition
            != revival_launch::LaunchDisposition::AdoptExternalSpectator
        && g_launchDisposition
            != revival_launch::LaunchDisposition::AttachExistingTournament)
    {
        (void)SetLocalRoleFlag(kLocalRoleLocalPlay, "host_initialize");
    }

    mod::Log("Takeover: host initialized");
    return true;
}

void ShutdownHost()
{
    CloseMirrorLogFiles();
    StopManagedLogEfzWorker(true);
    std::lock_guard<std::mutex> lock(g_mutex);
    LogRevival102jDeepSnapshot("ShutdownHost.01.entry");
    RestoreTemporaryHostProtocolOverride(
        "Revival netplay session host shutdown");
    BOOL terminateOk = TRUE;
    if (g_revivalProcess != nullptr)
    {
        terminateOk = TerminatePeerProcessIfOwned(0, "shutdown_host");
    }
    const bool processReleased =
        ReleasePeerProcessAfterTerminationAttempt(
            terminateOk, nullptr, "shutdown_host");
    CloseChildJobObject();
    CloseHostIpc();
    if (processReleased)
    {
        CleanupExternalLauncherGuard();
    }
    CleanupNativeHostShadowLogDirectory("host_shutdown");
    g_localRoleFlag = -1;
    g_localInitAppliedForSession = false;
    g_spectatorPostInitAttemptedForSession = false;
    g_spectatorPostInitSucceededForSession = false;
    InterlockedExchange(&g_externalTournamentInitialTitleLeft, 0);
    InterlockedExchange(&g_deferredLifecycleWorkRequested, 0);
    InterlockedExchange(&g_deferredTitleSelection, -1);
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    g_lastLatePatchRetryTick = 0;
    g_observedTakeoverCreatePath = false;
    g_remoteInjectedSelfBase = 0;
    g_externalLauncherGuardRemoteBase = 0;
    g_remoteInjectedPatchMap.clear();
    mod::Log("Takeover: host shutdown");
    LogRevival102jDeepStep("ShutdownHost.99.complete");
}

void EmergencyShutdownHost()
{
    StopManagedLogEfzWorker(false);
    std::lock_guard<std::mutex> lock(g_mutex);
    LogRevival102jDeepSnapshot("EmergencyShutdownHost.01.entry");
    RestoreTemporaryHostProtocolOverride(
        "Revival netplay session emergency host shutdown");
    InterlockedExchange(&g_startAbortRequested, 1);
    BOOL terminateOk = TRUE;
    if (g_revivalProcess != nullptr)
    {
        terminateOk = TerminatePeerProcessIfOwned(
            0, "emergency_shutdown_host", false);
    }
    const bool processReleased =
        ReleasePeerProcessAfterTerminationAttempt(
            terminateOk,
            nullptr,
            "emergency_shutdown_host",
            false);
    CloseChildJobObject();
    CloseHostIpc();
    if (processReleased)
    {
        CleanupExternalLauncherGuard();
    }
    CleanupNativeHostShadowLogDirectory("host_emergency_shutdown");
    g_localRoleFlag = -1;
    g_localInitAppliedForSession = false;
    g_spectatorPostInitAttemptedForSession = false;
    g_spectatorPostInitSucceededForSession = false;
    InterlockedExchange(&g_externalTournamentInitialTitleLeft, 0);
    InterlockedExchange(&g_deferredLifecycleWorkRequested, 0);
    InterlockedExchange(&g_deferredTitleSelection, -1);
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    g_lastLatePatchRetryTick = 0;
    g_observedTakeoverCreatePath = false;
    g_remoteInjectedSelfBase = 0;
    g_externalLauncherGuardRemoteBase = 0;
    g_remoteInjectedPatchMap.clear();
}

void RequestAbortStart()
{
    InterlockedExchange(&g_startAbortRequested, 1);
}

void OnTitleSelectionConfirmed(int selection, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LogRevival102jDeepStep("TitleSelection.01.entry", ioStatus);

    if (g_launchDisposition
            == revival_launch::LaunchDisposition::AttachExistingTournament
        && g_localRoleFlag == kLocalRoleTournament)
    {
        // Every title confirmation during this exact launcher-owned role-3
        // generation belongs to Revival's native queued navigation. The
        // original title update has already handled it; the observer must not
        // defer lifecycle work, destroy the object, or invoke init again.
        mod::Log(
            "Takeover: preserved launcher-owned Tournament title confirmation selection=%d (initCalls=0)",
            selection);
        return;
    }

    if (IsInsideFrameTick())
    {
        InterlockedExchange(&g_deferredTitleSelection, selection);
        InterlockedExchange(&g_deferredLifecycleWorkRequested, 1);
        mod::Log(
            "Takeover: deferred title selection=%d until the active Revival "
            "frame tick returns",
            selection);
        LogRevival102jDeepSnapshot("TitleSelection.02.deferred", ioStatus);
        return;
    }

    if (!EnsureLocalRevivalLoaded())
    {
        return;
    }

    const bool sessionActive = ProcessAlive(ioStatus);
    if (sessionActive)
    {
        return;
    }

    // If we're currently in tournament mode, perform full cleanup before
    // doing anything else.  The DLL call-site patches make ExitProcess
    // unreachable, so the old ConsumeRevivalExitInterception path never
    // fires - we must clean up here instead.
    //
    // This also handles tournament re-entry (selection == 2 a second
    // time): without cleanup the EXE patches would be saved in their
    // already-patched state and the DLL patches would be skipped
    // because they're already 0xEB.  Resetting g_localRoleFlag to
    // kLocalRoleLocalPlay lets SetLocalRoleFlag call init(3,102) again.
    if (g_localRoleFlag == kLocalRoleTournament)
    {
        LogRevival102jDeepSnapshot("TitleSelection.10.tournament_cleanup_pre", ioStatus);
        if (!g_tournamentReturnCleanupPending)
        {
            ArmTournamentReturnCleanup("title_selection_while_tournament_active");
        }
        InterlockedExchange(&g_deferredLifecycleWorkRequested, 1);
        mod::Log(
            "Takeover: title selection=%d held until transactional Tournament cleanup completes",
            selection);
        LogRevival102jDeepSnapshot(
            "TitleSelection.11.tournament_cleanup_deferred", ioStatus);
        return;
    }

    if (selection == 2)
    {
        if (netplay::options::UseTournamentModeForOfflineVsHuman())
        {
            // VS Human: create a real tournament session via init(3,102).
            // This gives us win counters, player nicknames, match result
            // logging, and the EXE patches that tournament mode applies.
            // Save the original EXE bytes first so we can restore them when
            // the tournament exits, then neutralize the auto-navigation
            // input queue so the 22-entry button sequence doesn't cause
            // phantom inputs during character select.
            //
            // The DLL call-site patches must be applied BEFORE init(3,102)
            // returns, because the tournament session's tick function runs
            // on the very next frame and would call ExitProcess immediately
            // (mode is still 0 = title screen).
            const bool renderSavedBefore = SaveRenderContext();
            LogRevival102jDeepSnapshot("TitleSelection.20.tournament_init_pre", ioStatus);
            mod::Log(
                "Takeover: tournament pre-init SaveRenderContext=%d",
                renderSavedBefore ? 1 : 0);
            const bool exeJournalReady = SaveTournamentExePatches();
            const bool exitGuardsReady = exeJournalReady
                && SaveAndApplyDllExitProcessPatches();
            if (!exeJournalReady || !exitGuardsReady)
            {
                const bool exeRestoreOk = !exeJournalReady
                    || RestoreTournamentExePatches();
                const bool dllRestoreOk = !AreDllExitPatchesSaved()
                    || RestoreDllExitProcessPatches();
                mod::Log(
                    "Takeover: direct Tournament pre-init rejected exeJournal=%d guards=%d exeRestore=%d dllRestore=%d",
                    exeJournalReady ? 1 : 0,
                    exitGuardsReady ? 1 : 0,
                    exeRestoreOk ? 1 : 0,
                    dllRestoreOk ? 1 : 0);
                if (!exeRestoreOk || !dllRestoreOk)
                {
                    ArmTournamentReturnCleanup(
                        "direct_tournament_preinit_guard_failure");
                    InterlockedExchange(
                        &g_revivalExitMode,
                        static_cast<LONG>(kLocalRoleTournament));
                    InterlockedExchange(&g_revivalExitIntercepted, 1);
                    InterlockedExchange(&g_deferredLifecycleWorkRequested, 1);
                }
                return;
            }

            const bool roleSwitchReported = SetLocalRoleFlag(
                kLocalRoleTournament,
                "title_vs_human_tournament");
            const int nativeRole = ReadRoleFlagFromRevival();
            const uintptr_t tournamentSession =
                ReadSessionPointerFromRevival();
            uintptr_t tournamentVtable = 0;
            const bool exactTournamentObject =
                roleSwitchReported
                && nativeRole == kLocalRoleTournament
                && tournamentSession != 0
                && g_activeRevival != nullptr
                && g_activeRevival->tournamentSessionVtableRva != 0
                && SafeReadPtr(
                    reinterpret_cast<const void*>(tournamentSession),
                    &tournamentVtable)
                && tournamentVtable
                    == reinterpret_cast<uintptr_t>(g_localRevivalModule)
                        + g_activeRevival->tournamentSessionVtableRva;
            const bool queueNeutralized = exactTournamentObject
                && NeutralizeTournamentAutoNav();
            if (!exactTournamentObject || !queueNeutralized)
            {
                mod::Log(
                    "Takeover: direct Tournament commit rejected roleSwitch=%d nativeRole=%d session=0x%08lX vtable=0x%08lX queue=%d",
                    roleSwitchReported ? 1 : 0,
                    nativeRole,
                    static_cast<unsigned long>(tournamentSession),
                    static_cast<unsigned long>(tournamentVtable),
                    queueNeutralized ? 1 : 0);
                ArmTournamentReturnCleanup(
                    "direct_tournament_commit_validation_failure");
                InterlockedExchange(
                    &g_revivalExitMode,
                    static_cast<LONG>(kLocalRoleTournament));
                InterlockedExchange(&g_revivalExitIntercepted, 1);
                InterlockedExchange(&g_deferredLifecycleWorkRequested, 1);
                return;
            }
            if (!renderSavedBefore)
            {
                const bool renderSavedAfter = SaveRenderContext();
                mod::Log(
                    "Takeover: tournament post-init SaveRenderContext=%d",
                    renderSavedAfter ? 1 : 0);
            }
            if (IsActiveRevival102jProfile())
            {
                const bool textEnableOk = SetRevivalTextRenderingEnabled(
                    true,
                    "title_vs_human_tournament_102j_start");
                mod::Log(
                    "Takeover: 1.02j tournament start text rendering enable=%d",
                    textEnableOk ? 1 : 0);
            }
            mod::Log(
                "Takeover: direct Tournament committed role=3 session=0x%08lX queueNeutralized=1",
                static_cast<unsigned long>(tournamentSession));
            LogRevival102jDeepSnapshot("TitleSelection.21.tournament_init_post", ioStatus);
        }
        else
        {
            // Regular VS Human: keep Revival in its local-play role and let
            // the native EFZ title flow proceed without tournament-only hooks.
            (void)SetRoleFlagDirect(kLocalRoleLocalPlay, "title_vs_human_local");
            mod::Log("Takeover: offline VS Human configured for regular local VS");
        }
    }
    else
    {
        (void)SetRoleFlagDirect(kLocalRoleLocalPlay, "title_other");
    }
    // Normalize a stranded client input swap before entering any offline/local
    // mode.  A prior client (P2) netplay session that returned via the
    // frontend_return path leaves the persistent P1/P2 input blocks swapped;
    // these local-play title paths run neither StartSession nor (for regular VS
    // Human) ForceLocalPlayInit, so without this the offline match would start
    // with P1/P2 inputs swapped.  No-op when nothing is stranded; the tournament
    // branch already cleared the flag via ForceLocalPlayInit.  Safe here because
    // title selection implies no live netplay session.
    if (IsClientInputSwapApplied())
    {
        mod::Log(
            "TitleSelection: normalizing stranded client input swap before "
            "offline/local play");
        ReverseInputSwapIfClient();
    }
    RefreshRuntimeStatus(ioStatus);
    LogRevival102jDeepSnapshot("TitleSelection.99.complete", ioStatus);
}

namespace
{
class TemporaryHostProtocolFailureGuard
{
public:
    explicit TemporaryHostProtocolFailureGuard(bool armed)
        : armed_(armed)
    {
    }

    ~TemporaryHostProtocolFailureGuard()
    {
        if (armed_)
        {
            RestoreTemporaryHostProtocolOverride(
                "Revival netplay session helper startup failed");
        }
    }

    void Disarm()
    {
        armed_ = false;
    }

private:
    bool armed_ = false;
};
} // namespace

bool StartSession(
    NetbridgeRole role,
    uint16_t port,
    const char* address,
    const char* iniAddress,
    const char* nickname,
    bool writeNicknameToIni,
    network::NetworkFamily sessionFamily,
    bool writeHostProtocol,
    NetbridgeStatus* ioStatus,
    uint32_t* outConnectStartTick)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    // A launcher-first session can release the ordinary peer slot while its
    // exact parent guard remains protect-only. Reap it opportunistically once
    // the old parent has exited; this never re-enables retired signal delivery.
    (void)ReapExternalLauncherGuardIfParentExited();
    if (g_peerProcessOwnership
            == PeerProcessOwnership::ExternalLauncherParent
        && g_revivalProcess != nullptr)
    {
        if (ProcessAlive(ioStatus))
        {
            SetPhase(
                ioStatus,
                NetbridgePhase::Failed,
                "Previous external Revival launcher is still exiting; try again shortly.");
            return false;
        }
        // ProcessAlive observed the exact retained process object signaled,
        // normalized the launch disposition, and released the old peer slot.
    }

    mod::Log(
        "Takeover: StartSession role=%d port=%u address='%s' iniAddress='%s' nickname='%s' "
        "writeNicknameToIni=%d family=%s writeHostProtocol=%d",
        static_cast<int>(role),
        static_cast<unsigned>(port),
        (address != nullptr) ? address : "",
        (iniAddress != nullptr) ? iniAddress : "",
        (nickname != nullptr) ? nickname : "",
        writeNicknameToIni ? 1 : 0,
        network::FamilyName(sessionFamily),
        writeHostProtocol ? 1 : 0);
    LogRevival102jDeepStep("StartSession.01.entry", ioStatus);

    const bool temporaryHostProtocol =
        role == NetbridgeRole::Host && writeHostProtocol;
    if (temporaryHostProtocol
        && !PrepareTemporaryHostProtocolOverride(sessionFamily))
    {
        SetPhase(
            ioStatus,
            NetbridgePhase::Failed,
            "Revival netplay session Protocol setup failed.");
        return false;
    }
    TemporaryHostProtocolFailureGuard protocolFailureGuard(
        temporaryHostProtocol);

    // --- Session-start diagnostic dump (2nd-session crash investigation) ---
    const uint32_t sessionEpoch = BeginManagedSessionBoundary("StartSession");
    (void)sessionEpoch;
    // Normalize a stranded client input swap before the new session inits.  If
    // the previous session was a client (P2) whose swap was never reversed
    // (e.g. it returned to the netplay menu via frontend_return, which clears
    // the role without running ForceLocalPlayInit), the persistent P1/P2 input
    // blocks are still swapped and would corrupt this session's input ownership
    // (immediate desync).  This reverses+clears it iff the flag is still set;
    // a no-op otherwise.  Runs before the helper spawns and long before this
    // session's own StartInitPlayer applies its swap, so it cannot race.
    if (IsClientInputSwapApplied())
    {
        mod::Log(
            "StartSession: normalizing stranded client input swap from a prior "
            "session before new init");
        ReverseInputSwapIfClient();
    }
    if (g_tournamentReturnCleanupPending)
    {
        LogRevival102jDeepSnapshot("StartSession.02.stale_tournament_pre", ioStatus);
        mod::Log(
            "StartSession: rejected while transactional Tournament cleanup is still pending");
        SetPhase(
            ioStatus,
            NetbridgePhase::Failed,
            "Tournament cleanup is still in progress; try again after returning to title.");
        return false;
    }

    // --- Session boundary cleanup logging ---
    // Log EXE hook bytes at both hook sites for cross-session tracking.
    {
        uint8_t hookA[10] = {};
        uint8_t hookB[8] = {};
        memcpy(hookA, reinterpret_cast<const void*>(0x401582), 10);
        memcpy(hookB, reinterpret_cast<const void*>(0x401642), 8);
        mod::Log(
            "StartSession: EXE hooks pre-session "
            "0x401582=[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X] "
            "0x401642=[%02X %02X %02X %02X %02X %02X %02X %02X]",
            hookA[0], hookA[1], hookA[2], hookA[3], hookA[4],
            hookA[5], hookA[6], hookA[7], hookA[8], hookA[9],
            hookB[0], hookB[1], hookB[2], hookB[3], hookB[4],
            hookB[5], hookB[6], hookB[7]);
    }

    // Clear stale exit-interception flags from a previous session.
    // If g_revivalExitIntercepted leaked from session 1 (e.g. ExitProcess
    // raced with CancelSession), ConsumeRevivalExitInterception would fire
    // during session 2's setup - running the full reverse-init cleanup and
    // destroying the new session.
    if (InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0) != 0)
    {
        mod::Log("StartSession: clearing stale g_revivalExitIntercepted=%ld g_revivalExitMode=%ld",
                 static_cast<long>(g_revivalExitIntercepted),
                 static_cast<long>(g_revivalExitMode));
        InterlockedExchange(&g_revivalExitIntercepted, 0);
        InterlockedExchange(&g_revivalExitMode, -1);
    }

    LogSessionDiagnosticState("StartSession_entry");

    InterlockedExchange(&g_startAbortRequested, 0);

    if (!EnsureHostIpc())
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "IPC setup failed");
        return false;
    }

    if (ResetHostSharedBlockForSession(
            role == NetbridgeRole::Host,
            port,
            "StartSession"))
    {
        LogRevival102jDeepStep("StartSession.05.shared_block_reset", ioStatus);
    }

    if (!EnsureLocalRevivalLoaded(true))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "EfzRevival.dll unavailable");
        return false;
    }
    if (!ActiveRevivalProfileSupportsSessionStart())
    {
        const char* const tag =
            (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
                ? g_activeRevival->versionTag
                : "(null)";
        const uintptr_t roleOffset =
            (g_activeRevival != nullptr && g_activeRevival->roleFlagOffsetCount > 0)
                ? g_activeRevival->roleFlagOffsets[0]
                : 0;
        const uintptr_t sessionOffset =
            (g_activeRevival != nullptr && g_activeRevival->sessionPtrOffsetCount > 0)
                ? g_activeRevival->sessionPtrOffsets[0]
                : 0;
        mod::Log(
            "Takeover: StartSession rejected Revival profile version=%s roleCount=%u role0=0x%08lX "
            "sessionCount=%u session0=0x%08lX startInit=0x%08lX frameHook=0x%08lX tick=0x%08lX",
            tag,
            (g_activeRevival != nullptr) ? static_cast<unsigned>(g_activeRevival->roleFlagOffsetCount) : 0u,
            static_cast<unsigned long>(roleOffset),
            (g_activeRevival != nullptr) ? static_cast<unsigned>(g_activeRevival->sessionPtrOffsetCount) : 0u,
            static_cast<unsigned long>(sessionOffset),
            static_cast<unsigned long>((g_activeRevival != nullptr) ? g_activeRevival->startInitPlayerRva : 0),
            static_cast<unsigned long>((g_activeRevival != nullptr) ? g_activeRevival->frameHookRva : 0),
            static_cast<unsigned long>((g_activeRevival != nullptr) ? g_activeRevival->perFrameTickRva : 0));
        SetPhase(ioStatus, NetbridgePhase::Failed, "Unsupported EfzRevival.dll profile");
        return false;
    }

    LogRevival102jDeepSnapshot("StartSession.06.profile_and_ipc_ready", ioStatus);

    // Save the EfzRender* pointer now so that ClearRevivalText /
    // DisableRevivalTextRendering can restore it during CancelSession.
    // Tournament mode already does this in OnTitleSelectionConfirmed,
    // but online sessions (host/join/spectate) skipped it - causing
    // both clear and disable to silently fail on the cleanup path.
    (void)SaveRenderContext();

    if (ProcessAlive(ioStatus))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "Session already active");
        return false;
    }

    CloseProcessHandle(ioStatus);

    if (ioStatus != nullptr)
    {
        ioStatus->role = static_cast<int>(role);
        ioStatus->port = port;
        CopyString(ioStatus->address, sizeof(ioStatus->address), address);
        CopyString(ioStatus->nickname, sizeof(ioStatus->nickname), nickname);
    }
    SetPhase(ioStatus, NetbridgePhase::Connecting, nullptr);
    LogRevival102jDeepStep("StartSession.07.phase_connecting", ioStatus);
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    g_lastNativeDelayTimeoutTraceSerial = 0;
    g_lastNativeDelayTimeoutTraceTick = 0;
    if (outConnectStartTick != nullptr)
    {
        *outConnectStartTick = GetTickCount();
    }

    const std::string gameDir = GameDirectory();
    const std::wstring gameDirWide = GameDirectoryWide();
    if (!WriteIni(
            static_cast<int>(role),
            port,
            iniAddress,
            nickname,
            writeNicknameToIni,
            sessionFamily,
            writeHostProtocol))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "EfzRevival.ini write failed");
        return false;
    }
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::string exePath = gameDir;
    if (!exePath.empty())
    {
        exePath += "\\";
    }
    exePath += "EfzRevival.exe";

    std::wstring exePathWide = gameDirWide;
    if (!exePathWide.empty())
    {
        exePathWide += L"\\";
    }
    exePathWide += L"EfzRevival.exe";

    // ---- Wine/Proton path --------------------------------------------------
    // Both Wine and native paths use CREATE_SUSPENDED so that the child's
    // main thread is frozen before its entry point.  We inject our DLL via
    // CreateRemoteThread(LoadLibraryA), patch the IAT, then ResumeThread.
    // This ensures our ReadConsoleA hook is in place before main() runs.
    //
    // Under Wine we also set WINEDLLOVERRIDES as a belt-and-suspenders
    // measure (it does not force-load custom DLLs on current Wine, but
    // may help on future versions).
    //
    // SelfPatchIat() in DllMain(DLL_PROCESS_ATTACH) provides an additional
    // safety net under Wine, patching the EXE's IAT from within the
    // process before the loader lock is released.
    // --------------------------------------------------------------------
    const bool useWinePath = IsRunningUnderWine();

    if (useWinePath)
    {
        // Build an environment block that includes WINEDLLOVERRIDES so Wine
        // force-loads our DLL into the child process.
        //
        // Get our DLL's full path.  Wine's WINEDLLOVERRIDES expects the
        // module name (without extension), e.g. "efz_netplay_mod=n".
        // Using just the basename is the documented format for Wine.
        // We also set the DLL's directory on the search path so Wine can
        // find the native DLL file when the override triggers.
        const std::string selfPath = ModulePath(SelfModule());
        if (selfPath.empty())
        {
            SetPhase(ioStatus, NetbridgePhase::Failed, "[Wine] failed to get self module path");
            return false;
        }

        // Build the override string: "efz_netplay_mod=n"
        // n = native - tells Wine to load the DLL from the filesystem.
        std::string baseName = BaseLower(selfPath);
        {
            // Strip .dll extension if present.
            const size_t dot = baseName.rfind('.');
            if (dot != std::string::npos)
                baseName.erase(dot);
        }
        std::string overrideValue = baseName + "=n";

        // Merge with any existing WINEDLLOVERRIDES.
        char existingOverride[4096] = {};
        const DWORD existingLen = GetEnvironmentVariableA(
            "WINEDLLOVERRIDES", existingOverride, sizeof(existingOverride));
        if (existingLen > 0 && existingLen < sizeof(existingOverride))
        {
            overrideValue = std::string(existingOverride) + ";" + overrideValue;
        }

        // Build an environment block - a double-null-terminated sequence of
        // "KEY=VALUE\0" strings.  We inherit the current environment and
        // append/override WINEDLLOVERRIDES.
        std::vector<char> envBlock;
        {
            // Get the current environment.
            char* currentEnv = GetEnvironmentStringsA();
            if (currentEnv != nullptr)
            {
                // Walk the double-null-terminated block.
                const char* p = currentEnv;
                bool overrideWritten = false;
                while (*p != '\0')
                {
                    const size_t entryLen = std::strlen(p);
                    // Check if this is the WINEDLLOVERRIDES entry.
                    if (_strnicmp(p, "WINEDLLOVERRIDES=", 17) == 0)
                    {
                        // Replace with our merged value.
                        std::string merged = "WINEDLLOVERRIDES=" + overrideValue;
                        envBlock.insert(envBlock.end(), merged.begin(), merged.end());
                        envBlock.push_back('\0');
                        overrideWritten = true;
                    }
                    else
                    {
                        envBlock.insert(envBlock.end(), p, p + entryLen + 1);
                    }
                    p += entryLen + 1;
                }
                if (!overrideWritten)
                {
                    std::string entry = "WINEDLLOVERRIDES=" + overrideValue;
                    envBlock.insert(envBlock.end(), entry.begin(), entry.end());
                    envBlock.push_back('\0');
                }
                envBlock.push_back('\0'); // double-null terminator
                FreeEnvironmentStringsA(currentEnv);
            }
        }

        BOOL created = CreateProcessA(
            exePath.c_str(),
            nullptr,
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW,
            envBlock.empty() ? nullptr : envBlock.data(),
            gameDir.empty() ? nullptr : gameDir.c_str(),
            &si,
            &pi);

        if (!created)
        {
            mod::Log("Takeover [Wine]: CreateProcess failed err=%lu",
                     static_cast<unsigned long>(GetLastError()));
            SetPhase(ioStatus, NetbridgePhase::Failed, "[Wine] CreateProcess(EfzRevival.exe) failed");
            return false;
        }

        mod::Log("Takeover [Wine]: spawned EfzRevival suspended pid=%lu (DLL override active)",
                 static_cast<unsigned long>(pi.dwProcessId));
        LogRevival102jDeepStep("StartSession.10.helper_spawned_suspended_wine", ioStatus);
    }
    else
    {
        // ---- Native Windows path -------------------------------------------
        STARTUPINFOW siWide = {};
        siWide.cb = sizeof(siWide);
        BOOL created = CreateProcessW(
            exePathWide.c_str(),
            nullptr,
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW,
            nullptr,
            gameDirWide.empty() ? nullptr : gameDirWide.c_str(),
            &siWide,
            &pi);

        if (!created)
        {
            SetPhase(ioStatus, NetbridgePhase::Failed, "CreateProcess(EfzRevival.exe) failed");
            return false;
        }

        mod::Log("Takeover: spawned EfzRevival suspended pid=%lu", static_cast<unsigned long>(pi.dwProcessId));
        LogRevival102jDeepStep("StartSession.10.helper_spawned_suspended", ioStatus);
    }

    // Assign to kill-on-close job so that if the host process terminates
    // (crash, Alt+F4, etc.) without explicit cleanup, all child processes
    // (EfzRevival.exe, cmd.exe, conhost.exe, ...) are killed automatically.
    HANDLE job = EnsureChildJobObject();
    if (job != nullptr)
    {
        if (!AssignProcessToJobObject(job, pi.hProcess))
        {
            mod::Log("Takeover: AssignProcessToJobObject failed pid=%lu err=%lu",
                     static_cast<unsigned long>(pi.dwProcessId),
                     static_cast<unsigned long>(GetLastError()));
        }
        else
        {
            mod::Log("Takeover: assigned pid=%lu to kill-on-close job",
                     static_cast<unsigned long>(pi.dwProcessId));
        }
    }

    uintptr_t remoteBase = 0;

    // Inject our DLL into the child process.
    // - Under Wine: InjectSelf() first checks if WINEDLLOVERRIDES loaded the
    //   DLL, then falls back to CreateRemoteThread(LoadLibraryA).
    // - Native Windows: InjectSelf() uses CreateRemoteThread(LoadLibraryA).
    if (!InjectSelf(pi.hProcess, &remoteBase))
    {
        const char* reason = useWinePath
            ? "[Wine] DLL injection failed"
            : "Self injection failed";
        SetPhase(ioStatus, NetbridgePhase::Failed, reason);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }
    LogRevival102jDeepStep("StartSession.11.helper_injected", ioStatus);

    std::unordered_map<std::string, uint32_t> patches = BuildPatchMap(remoteBase);

    if (!PatchIat(pi.hProcess, pi.dwProcessId, patches, true))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "IAT patch failed");
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }
    LogRevival102jDeepStep("StartSession.12.helper_iat_patched", ioStatus);

    int localRoleMode = kLocalRoleOnline;
    if (role == NetbridgeRole::Spectate || role == NetbridgeRole::JoinSpectate)
    {
        localRoleMode = kLocalRoleSpectate;
    }

    g_hostBlock->initParams[0] = localRoleMode;
    g_hostBlock->initParams[1] = 102;
    ResetDebugCounters(g_hostBlock);
    g_localInitAppliedForSession = false;
    g_spectatorPostInitAttemptedForSession = false;
    g_spectatorPostInitSucceededForSession = false;
    g_sessionHistoryRepairHits = 0;
    InterlockedExchange(&g_injectedDelayPromptSerial, 0);
    InterlockedExchange(&g_injectedDelayPromptServedSerial, 0);
    InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, 0);
    InterlockedExchange(&g_injectedSpectateConfirmPromptSerial, 0);
    InterlockedExchange(&g_injectedSpectateConfirmPromptServedSerial, 0);
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_holePunchServerConfigLoaded = false;
    g_configuredHolePunchServer.clear();
    g_lastSpectateConsoleSnapshotTick = 0;
    InterlockedExchange(&g_injectedFingerprintReadHits, 0);
    {
        std::lock_guard<std::mutex> consoleLock(g_consoleLogMutex);
        g_consolePendingWriteFile.clear();
        g_consolePendingWriteFileDisk.clear();
        g_consolePendingWriteConsoleA.clear();
        g_consolePendingWriteConsoleW.clear();
        g_consolePendingWriteConsoleOutputCharacterA.clear();
        g_consolePendingWriteConsoleOutputCharacterW.clear();
        g_consolePendingOutputDebugStringA.clear();
        g_consolePendingOutputDebugStringW.clear();
    }
    CloseMirrorLogFiles();
    InterlockedIncrement(&g_hostBlock->initSerial);
    LogRevival102jDeepSnapshot("StartSession.13.ipc_payload_initialized", ioStatus);

    const console_handoff::InitialConsoleInputPlan consoleInputPlan =
        console_handoff::BuildInitialConsoleInputPlan(role);
    std::string primaryInput = consoleInputPlan.primaryInput;
    int menuChoice = consoleInputPlan.menuChoice;
    if (role == NetbridgeRole::Join)
    {
        if (address != nullptr && address[0] != '\0')
        {
            network::NetworkEndpoint endpoint = {};
            endpoint.family = sessionFamily;
            endpoint.host = address;
            endpoint.port = port;
            std::string clipboardAddress;
            if (network::FormatEndpoint(endpoint, &clipboardAddress))
            {
                if (TryWriteClipboardAscii(clipboardAddress.c_str()))
                {
                    mod::Log("Takeover: join clipboard seeded with address '%s'", clipboardAddress.c_str());
                }
                else
                {
                    mod::Log("Takeover: join clipboard write failed; Revival will use existing clipboard");
                }
            }
            else
            {
                mod::Log(
                    "Takeover: Revival netplay session join endpoint formatting failed "
                    "family=%s address='%s' port=%u",
                    network::FamilyName(sessionFamily),
                    address,
                    static_cast<unsigned>(port));
            }
        }
    }
    else if (role == NetbridgeRole::JoinSpectate)
    {
        // Join (choice 3). Prompt handling is done from the host-side UI based
        // on the detected prompt kind:
        // - "Host already playing, join as a spectator?" -> auto-answer Yes (1)
        // - "Host not yet playing, join as a player?"   -> show Join/Wait/Cancel
        //   in the in-game overlay and let the host-side UI decide whether to
        //   keep waiting, restart as a normal Join, or cancel cleanly.
        if (address != nullptr && address[0] != '\0')
        {
            network::NetworkEndpoint endpoint = {};
            endpoint.family = sessionFamily;
            endpoint.host = address;
            endpoint.port = port;
            std::string clipboardAddress;
            if (network::FormatEndpoint(endpoint, &clipboardAddress))
            {
                if (TryWriteClipboardAscii(clipboardAddress.c_str()))
                {
                    mod::Log("Takeover: join-spectate clipboard seeded with address '%s'", clipboardAddress.c_str());
                }
                else
                {
                    mod::Log("Takeover: join-spectate clipboard write failed; Revival will use existing clipboard");
                }
            }
            else
            {
                mod::Log(
                    "Takeover: Revival netplay session join-spectate endpoint formatting failed "
                    "family=%s address='%s' port=%u",
                    network::FamilyName(sessionFamily),
                    address,
                    static_cast<unsigned>(port));
            }
        }
    }
    else if (role == NetbridgeRole::Spectate)
    {
        if (address != nullptr && address[0] != '\0')
        {
            network::NetworkEndpoint endpoint = {};
            endpoint.family = sessionFamily;
            endpoint.host = address;
            endpoint.port = port;
            std::string clipboardAddress;
            if (!network::FormatEndpoint(endpoint, &clipboardAddress))
            {
                mod::Log(
                    "Takeover: Revival netplay session spectate endpoint formatting failed "
                    "family=%s address='%s' port=%u; using clipboard option as-is",
                    network::FamilyName(sessionFamily),
                    address,
                    static_cast<unsigned>(port));
            }
            else
            {
                if (!TryWriteClipboardAscii(clipboardAddress.c_str()))
                {
                    mod::Log("Takeover: spectate clipboard write failed; using option 4 with existing clipboard");
                }
                else
                {
                    mod::Log("Takeover: spectate clipboard seeded with address '%s'", clipboardAddress.c_str());
                }
            }
        }
    }

    if (primaryInput.empty())
    {
        primaryInput = "1\r\n";
        menuChoice = 1;
    }

    CopyString(g_hostBlock->consoleInput, sizeof(g_hostBlock->consoleInput), primaryInput.c_str());
    InterlockedIncrement(&g_hostBlock->consoleSerial);
    CopyString(
        g_hostBlock->consoleInputAux,
        sizeof(g_hostBlock->consoleInputAux),
        consoleInputPlan.auxiliaryInput);
    LONG auxSerialForSession = 0;
    if (g_hostBlock->consoleInputAux[0] != '\0')
    {
        auxSerialForSession =
            InterlockedIncrement(&g_hostBlock->consoleAuxSerial);
    }

    ResetEvent(g_hostInitEvent);
    ResetEvent(g_hostConsoleEvent);
    LogRevival102jDeepStep("StartSession.14.auto_reset_events_cleared", ioStatus);

    // --- Pre-launch ring buffer flush (spectate only) ----------------------
    // Revival's named shared-memory ring buffers ("InputP1", "InputP2", etc.)
    // may still contain stale data from a previous session if the kernel
    // objects haven't been destroyed.  Flush them NOW - before the child
    // process is resumed - so that only fresh data from the new session is
    // present when the DLL eventually starts consuming.
    //
    // Previously this flush lived inside the Tick() init-handshake block
    // (after the child signalled its init event).  That placement caused a
    // spectator desync: between the child's WriteProcessMemory IAT hook
    // signalling the init event and the next game-frame's Tick() detecting
    // it (~16 ms), the child had already started writing the host's
    // historical input stream into the ring buffers.  The flush then
    // discarded those early entries - the very beginning of the charselect
    // replay - leaving the DLL to start mid-stream against a freshly-
    // initialised charselect state.
    //
    // By flushing here (child still suspended), we clear only genuinely
    // stale data; once the child resumes and connects, every input frame
    // it writes is preserved for the DLL to consume.
    //
    // Extended to ALL roles (host/join/spectate): the named shared memory
    // ring buffers persist as kernel objects as long as any process holds a
    // handle.  The DLL in the host process survives across sessions, so
    // stale ring buffer mappings can contaminate session 2 for any role.
    // -------------------------------------------------------------------
    {
        struct RingMapping {
            const char* name;
            HANDLE      hMap;
            volatile DWORD* view;
        };
        RingMapping mappings[] = {
            {RevivalWireName("InputP1"),   nullptr, nullptr},
            {RevivalWireName("InputP2"),   nullptr, nullptr},
            {RevivalWireName("PaletteP1"), nullptr, nullptr},
            {RevivalWireName("PaletteP2"), nullptr, nullptr},
            {RevivalWireName("Sync"),      nullptr, nullptr},
            {RevivalWireName("Quit"),      nullptr, nullptr},
            {RevivalWireName("LoadMatch"), nullptr, nullptr},
            {RevivalWireName("Init"),      nullptr, nullptr},
            {RevivalWireName("Net"),       nullptr, nullptr},
        };
        constexpr int kMappingCount = 9;

        for (int mi = 0; mi < kMappingCount; ++mi)
        {
            mappings[mi].hMap = OpenFileMappingA(
                FILE_MAP_ALL_ACCESS, FALSE, mappings[mi].name);
            if (mappings[mi].hMap != nullptr)
            {
                mappings[mi].view = static_cast<volatile DWORD*>(
                    MapViewOfFile(mappings[mi].hMap,
                                 FILE_MAP_ALL_ACCESS, 0, 0, 8));
            }
        }
        for (int mi = 0; mi < kMappingCount; ++mi)
        {
            if (mappings[mi].view == nullptr)
                continue;
            const DWORD oldHead = mappings[mi].view[0];
            const DWORD oldTail = mappings[mi].view[1];
            if (oldHead != oldTail)
            {
                mappings[mi].view[0] = oldTail;
                mod::Log(
                    "Takeover: pre-launch flushed stale '%s' "
                    "head=%lu->%lu tail=%lu",
                    mappings[mi].name,
                    static_cast<unsigned long>(oldHead),
                    static_cast<unsigned long>(oldTail),
                    static_cast<unsigned long>(oldTail));
            }
        }
        // Alignment verification is unnecessary here - no producer is
        // running yet, so no interleaved writes can occur.
        for (int mi = 0; mi < kMappingCount; ++mi)
        {
            if (mappings[mi].view != nullptr)
                UnmapViewOfFile(const_cast<DWORD*>(mappings[mi].view));
            if (mappings[mi].hMap != nullptr)
                CloseHandle(mappings[mi].hMap);
        }
    }
    LogRevival102jDeepSnapshot("StartSession.15.prelaunch_rings_flushed", ioStatus);

    // The helper's DllMain bootstrap is asynchronous. Do not let its main
    // thread emit one-shot listener/prompt text until the helper-local raw
    // ingress worker has positively acknowledged readiness through IPC.
    const DWORD captureReadyStart = GetTickCount();
    while (InterlockedCompareExchange(
               &g_hostBlock->helperCaptureReady, 0, 0) == 0
        && GetTickCount() - captureReadyStart
            < kInjectedCaptureReadyTimeoutMs)
    {
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)
        {
            break;
        }
        Sleep(1);
    }
    if (InterlockedCompareExchange(
            &g_hostBlock->helperCaptureReady, 0, 0) == 0)
    {
        SetPhase(
            ioStatus,
            NetbridgePhase::Failed,
            "Helper console capture worker unavailable");
        (void)TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    // Both Wine and native: the process was created suspended, resume it
    // now that injection and IAT patching are complete.
    const DWORD resumeResult = ResumeThread(pi.hThread);
    mod::Log("Takeover: resumed main thread result=%lu%s",
             static_cast<unsigned long>(resumeResult),
             useWinePath ? " [Wine]" : "");
    if (resumeResult == static_cast<DWORD>(-1))
    {
        SetPhase(
            ioStatus,
            NetbridgePhase::Failed,
            "Failed to resume EfzRevival helper main thread");
        (void)TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }
    LogRevival102jDeepStep("StartSession.16.helper_resumed", ioStatus);

    g_revivalProcess = pi.hProcess;
    g_revivalProcessId = pi.dwProcessId;
    g_peerProcessOwnership = PeerProcessOwnership::SpawnedHelper;
    if (!StartPeerProcessExitWatch())
    {
        SetPhase(
            ioStatus,
            NetbridgePhase::Failed,
            "Helper process-exit watcher unavailable");
        const BOOL terminateOk =
            TerminatePeerProcessIfOwned(0, "start_watcher_failure");
        CloseHandle(pi.hThread);
        (void)ReleasePeerProcessAfterTerminationAttempt(
            terminateOk, ioStatus, "start_watcher_failure");
        return false;
    }
    g_remoteInjectedSelfBase = remoteBase;
    g_remoteInjectedPatchMap = std::move(patches);
    g_lastLatePatchRetryTick = GetTickCount();
    g_lastLatePatchRetryLogTick = 0;
    g_latePatchRetryAttempts = 0;
    g_latePatchRetrySuccesses = 0;
    g_lastLatePatchRetryResultValid = false;
    g_lastLatePatchRetryResult = false;
    g_observedTakeoverCreatePath = false;
    if (ioStatus != nullptr)
    {
        ioStatus->processId = g_revivalProcessId;
    }

    CloseHandle(pi.hThread);

    SetEvent(g_hostConsoleEvent);
    mod::Log(
        "Takeover: signaled console script menu=%d primaryLen=%u auxLen=%u auxSerial=%ld",
        menuChoice,
        static_cast<unsigned>(std::strlen(g_hostBlock->consoleInput)),
        static_cast<unsigned>(std::strlen(g_hostBlock->consoleInputAux)),
        static_cast<long>(auxSerialForSession));
    SetPhase(ioStatus, NetbridgePhase::Connecting, "awaiting handshake");
    RefreshRuntimeStatus(ioStatus);
    LogRevival102jDeepSnapshot("StartSession.99.armed", ioStatus);
    mod::Log("Takeover: start session armed (asynchronous handshake via Tick)");
    protocolFailureGuard.Disarm();
    return true;
}

bool ApplyInputDelay(int delayFrames, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LogRevival102jDeepStep("ApplyInputDelay.01.entry", ioStatus);

    // Input delay is only applicable to online sessions (mode 0).
    // Spectator sessions have a different layout and the delay offset
    // would write into unrelated spectator fields.
    // During the connecting phase g_localRoleFlag is still kLocalRoleLocalPlay
    // because the init handshake hasn't completed yet.  Check the *intended*
    // role stored in the shared block as well so the delay prompt works.
    const bool currentRoleOnline = (g_localRoleFlag == kLocalRoleOnline);
    const bool intendedRoleOnline = (g_hostBlock != nullptr
        && g_hostBlock->initParams[0] == kLocalRoleOnline);
    if (!currentRoleOnline && !intendedRoleOnline)
    {
        mod::Log("Takeover: ApplyInputDelay rejected (role=%d, intended=%d, online-only)",
                 g_localRoleFlag,
                 g_hostBlock ? g_hostBlock->initParams[0] : -1);
        return false;
    }

    if (delayFrames < 0 || delayFrames > 20)
    {
        mod::Log("Takeover: ApplyInputDelay rejected out-of-range value=%d", delayFrames);
        return false;
    }

    LONG delayPromptSerial = 0;
    LONG delayPromptServedSerial = 0;
    ReadDelayPromptSignal(&delayPromptSerial, &delayPromptServedSerial);
    const bool promptPending = delayPromptSerial > 0 && delayPromptServedSerial < delayPromptSerial;

    auto queuePromptInput = [&](int value) -> bool {
        if (g_hostBlock == nullptr)
        {
            return false;
        }

        g_hostBlock->delayInputValue = value;
        const LONG inputSerial = InterlockedIncrement(&g_hostBlock->delayInputSerial);
        if (g_hostConsoleEvent != nullptr)
        {
            SetEvent(g_hostConsoleEvent);
        }
        mod::Log(
            "Takeover: ApplyInputDelay queued prompt input value=%d promptSerial=%ld inputSerial=%ld",
            value,
            static_cast<long>(delayPromptSerial),
            static_cast<long>(inputSerial));
        MOD_LIFECYCLE_TRACE(
            "CONSOLE_DELAY_INPUT_QUEUE promptSerial=%ld promptServed=%ld "
            "inputSerial=%ld inputServed=%ld value=%d controlWake=%ld/%ld",
            static_cast<long>(delayPromptSerial),
            static_cast<long>(delayPromptServedSerial),
            static_cast<long>(inputSerial),
            static_cast<long>(
                InterlockedCompareExchange(
                    &g_hostBlock->delayInputServedSerial,
                    0,
                    0)),
            value,
            static_cast<long>(
                InterlockedCompareExchange(
                    &g_hostBlock->consoleControlWakeRequestSerial,
                    0,
                    0)),
            static_cast<long>(
                InterlockedCompareExchange(
                    &g_hostBlock->consoleControlWakeServedSerial,
                    0,
                    0)));
        RefreshRuntimeStatus(ioStatus);
        if (ioStatus != nullptr)
        {
            ioStatus->rollbackFrames = value;
        }
        return true;
    };

    if (promptPending && queuePromptInput(delayFrames))
    {
        LogRevival102jDeepSnapshot("ApplyInputDelay.02.prompt_value_queued", ioStatus);
        return true;
    }

    const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
    if (sessionPtr == 0)
    {
        mod::Log("Takeover: ApplyInputDelay failed (session pointer unavailable)");
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    void* const delayAddress = reinterpret_cast<void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay);
    if (!IsWritableRange(delayAddress, sizeof(int)))
    {
        mod::Log(
            "Takeover: ApplyInputDelay failed (delay field not writable addr=0x%p)",
            delayAddress);
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    *reinterpret_cast<int*>(delayAddress) = delayFrames;

    // Also write the saved target prediction window (inputDelay + 8, i.e.
    // +696 in 1.02e).  Between matches, BuildMatchInfoAndHUD restores
    // the active inputDelay (+688) FROM the target window (+696).  If we
    // only update +688 here, the target stays at 0 and all subsequent
    // matches start with inputDelay=0, causing 2-3 iterations per frame
    // (the ~1.5x speed-up bug).
    void* const targetWindowAddress = reinterpret_cast<void*>(
        sessionPtr + g_activeRevival->sessionOffsetInputDelay + 8);
    if (IsWritableRange(targetWindowAddress, sizeof(int)))
    {
        *reinterpret_cast<int*>(targetWindowAddress) = delayFrames;
    }

    mod::Log(
        "Takeover: ApplyInputDelay applied value=%d session=0x%08lX (+696=%d)",
        delayFrames,
        static_cast<unsigned long>(sessionPtr),
        delayFrames);
    RefreshRuntimeStatus(ioStatus);
    if (ioStatus != nullptr)
    {
        ioStatus->rollbackFrames = delayFrames;
    }
    LogRevival102jDeepSnapshot("ApplyInputDelay.99.direct_write_complete", ioStatus);
    return true;
}

bool AnswerSpectatePromptChoice(int choice, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LogRevival102jDeepStep("SpectatePrompt.01.answer_entry", ioStatus);

    LONG promptSerial = 0;
    LONG promptServedSerial = 0;
    int promptKind = static_cast<int>(NetbridgeSpectatePromptKind::None);
    ReadSpectateConfirmPromptSignal(&promptSerial, &promptServedSerial, &promptKind);
    const bool promptPending = promptSerial > 0 && promptServedSerial < promptSerial;

    if (!promptPending)
    {
        mod::Log(
            "Takeover: AnswerSpectatePromptChoice rejected (no pending prompt serial=%ld served=%ld)",
            static_cast<long>(promptSerial),
            static_cast<long>(promptServedSerial));
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    if (g_hostBlock == nullptr)
    {
        mod::Log("Takeover: AnswerSpectatePromptChoice rejected (no shared block)");
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    int maxChoice = 2;
    if (promptKind == static_cast<int>(NetbridgeSpectatePromptKind::HostNotYetPlaying))
    {
        maxChoice = 3;
    }
    if (choice < 1 || choice > maxChoice)
    {
        mod::Log(
            "Takeover: AnswerSpectatePromptChoice rejected (invalid choice=%d kind=%d maxChoice=%d)",
            choice,
            promptKind,
            maxChoice);
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    g_hostBlock->spectateConfirmInputValue = choice;
    const LONG inputSerial = InterlockedIncrement(&g_hostBlock->spectateConfirmInputSerial);
    if (g_hostConsoleEvent != nullptr)
    {
        SetEvent(g_hostConsoleEvent);
    }
    mod::Log(
        "Takeover: AnswerSpectatePromptChoice queued choice=%d kind=%d promptSerial=%ld inputSerial=%ld",
        choice,
        promptKind,
        static_cast<long>(promptSerial),
        static_cast<long>(inputSerial));
    RefreshRuntimeStatus(ioStatus);
    LogRevival102jDeepSnapshot("SpectatePrompt.99.answer_queued", ioStatus);
    return true;
}

bool PrepareVsHumanHandoff(NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LogRevival102jDeepStep("Handoff.01.prepare_entry", ioStatus);

    if (!EnsureLocalRevivalLoaded())
    {
        mod::Log("Takeover: PrepareVsHumanHandoff failed (EfzRevival.dll unavailable)");
        return false;
    }

    if (ioStatus == nullptr || ioStatus->localInitApplied == 0)
    {
        static DWORD s_lastNotReadyLogTick = 0;
        const DWORD now = GetTickCount();
        if (s_lastNotReadyLogTick == 0 || now - s_lastNotReadyLogTick >= 2000)
        {
            s_lastNotReadyLogTick = now;
            mod::Log(
                "Takeover: PrepareVsHumanHandoff deferred localInitApplied=%d phase=%s",
                ioStatus != nullptr ? ioStatus->localInitApplied : 0,
                ioStatus != nullptr ? netplay::bridge::PhaseToString(static_cast<NetbridgePhase>(ioStatus->phase)) : "unknown");
        }
        return false;
    }

    const bool roleSet = (g_localRoleFlag >= 0);
    RefreshRuntimeStatus(ioStatus);

    const bool syncReady = IsSyncReadyForVsHuman(ioStatus);
    const bool strictNativeSync = RequiresNativeVsHumanSyncForHandoff(ioStatus);
    const int mode = ioStatus != nullptr ? ioStatus->syncGameMode : -1;
    const int flag1084 = ioStatus != nullptr ? ioStatus->syncMode0Flag1084 : -1;
    const int sessionByte = ioStatus != nullptr ? ioStatus->syncSessionByte : -1;
    const int flag4964 = ioStatus != nullptr ? ioStatus->syncGlobalFlag4964 : -1;
    const int flag4965 = ioStatus != nullptr ? ioStatus->syncGlobalFlag4965 : -1;
    const int roleFlag = ioStatus != nullptr ? ioStatus->roleFlag : -1;

    static DWORD s_lastLogTick = 0;
    static int s_lastRoleSet = -1;
    static int s_lastSyncReady = -1;
    static int s_lastMode = std::numeric_limits<int>::min();
    static int s_lastFlag1084 = std::numeric_limits<int>::min();
    static int s_lastSessionByte = std::numeric_limits<int>::min();
    static int s_lastFlag4964 = std::numeric_limits<int>::min();
    static int s_lastFlag4965 = std::numeric_limits<int>::min();
    static int s_lastRoleFlag = std::numeric_limits<int>::min();
    static int s_lastStrictNativeSync = -1;
    const DWORD now = GetTickCount();
    const bool changed =
        s_lastRoleSet != (roleSet ? 1 : 0)
        || s_lastSyncReady != (syncReady ? 1 : 0)
        || s_lastStrictNativeSync != (strictNativeSync ? 1 : 0)
        || s_lastMode != mode
        || s_lastFlag1084 != flag1084
        || s_lastSessionByte != sessionByte
        || s_lastFlag4964 != flag4964
        || s_lastFlag4965 != flag4965
        || s_lastRoleFlag != roleFlag;
    if (changed || s_lastLogTick == 0 || now - s_lastLogTick >= 3000)
    {
        s_lastRoleSet = roleSet ? 1 : 0;
        s_lastSyncReady = syncReady ? 1 : 0;
        s_lastStrictNativeSync = strictNativeSync ? 1 : 0;
        s_lastMode = mode;
        s_lastFlag1084 = flag1084;
        s_lastSessionByte = sessionByte;
        s_lastFlag4964 = flag4964;
        s_lastFlag4965 = flag4965;
        s_lastRoleFlag = roleFlag;
        s_lastLogTick = now;
        mod::Log(
            "Takeover: PrepareVsHumanHandoff roleSet=%d syncReady=%d strictNativeSync=%d mode=%d flag1084=%d sessionByte=%d flags=%d/%d roleFlag=%d",
            roleSet ? 1 : 0,
            syncReady ? 1 : 0,
            strictNativeSync ? 1 : 0,
            mode,
            flag1084,
            sessionByte,
            flag4964,
            flag4965,
            roleFlag);
        LogRevival102jDeepStep("Handoff.02.readiness_changed_or_periodic", ioStatus);
    }
    const bool ready = syncReady || (roleSet && !strictNativeSync);
    if (ready)
    {
        LogRevival102jDeepSnapshot("Handoff.99.ready", ioStatus);
    }
    return ready;
}

void Tick(NetbridgeStatus* ioStatus, uint32_t* ioConnectStartTick)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ioStatus == nullptr)
    {
        return;
    }

    HandleTemporaryHostProtocolListenerAck();
    const NetbridgePhase phase = static_cast<NetbridgePhase>(ioStatus->phase);
    if (phase != NetbridgePhase::Connecting
        && phase != NetbridgePhase::DelaySetup
        && phase != NetbridgePhase::Connected)
    {
        RestoreTemporaryHostProtocolOverride(
            "Revival netplay session reached a terminal phase before listener acknowledgement");
        RefreshRuntimeStatus(ioStatus);
        return;
    }

    const bool helperAlive = ProcessAlive(ioStatus);
    if (g_hostBlock != nullptr
        && g_hostBlock->magic == kIpcMagic
        && g_hostBlock->version == kIpcVersion
        && g_hostBlock->hostPid != 0)
    {
        const LONG nativeTimeoutSerial =
            InterlockedCompareExchange(
                &g_hostBlock->nativeDelayTimeoutSerial,
                0,
                0);
        const LONG nativeTimeoutHandled =
            InterlockedCompareExchange(
                &g_hostBlock->nativeDelayTimeoutHandledSerial,
                0,
                0);
        if (nativeTimeoutSerial > 0
            && nativeTimeoutSerial > nativeTimeoutHandled)
        {
            const LONG requiredControlWakeSerial =
                InterlockedCompareExchange(
                    &g_hostBlock->nativeDelayTimeoutRequiredWakeSerial,
                    0,
                    0);
            const LONG controlWakeRequest =
                InterlockedCompareExchange(
                    &g_hostBlock->consoleControlWakeRequestSerial,
                    0,
                    0);
            const LONG controlWakeServed =
                InterlockedCompareExchange(
                    &g_hostBlock->consoleControlWakeServedSerial,
                    0,
                    0);
            const bool cleanupReady =
                console_handoff::IsNativeTimeoutCleanupReady(
                    helperAlive,
                    controlWakeServed,
                    requiredControlWakeSerial);
            const DWORD now = GetTickCount();
            if (nativeTimeoutSerial
                    != g_lastNativeDelayTimeoutTraceSerial
                || g_lastNativeDelayTimeoutTraceTick == 0
                || now - g_lastNativeDelayTimeoutTraceTick >= 1000)
            {
                g_lastNativeDelayTimeoutTraceSerial =
                    nativeTimeoutSerial;
                g_lastNativeDelayTimeoutTraceTick = now;
                MOD_LIFECYCLE_TRACE(
                    "NATIVE_DELAY_TIMEOUT_GATE serial=%ld handled=%ld "
                    "helperAlive=%d controlWake=%ld/%ld required=%ld "
                    "cleanupReady=%d",
                    static_cast<long>(nativeTimeoutSerial),
                    static_cast<long>(nativeTimeoutHandled),
                    helperAlive ? 1 : 0,
                    static_cast<long>(controlWakeRequest),
                    static_cast<long>(controlWakeServed),
                    static_cast<long>(requiredControlWakeSerial),
                    cleanupReady ? 1 : 0);
            }

            if (!cleanupReady)
            {
                RefreshRuntimeStatus(ioStatus);
                return;
            }

            MOD_LIFECYCLE_TRACE(
                "NATIVE_DELAY_TIMEOUT_RELEASE serial=%ld evidence=%s "
                "controlWake=%ld/%ld required=%ld",
                static_cast<long>(nativeTimeoutSerial),
                helperAlive ? "bel_served" : "helper_exit",
                static_cast<long>(controlWakeRequest),
                static_cast<long>(controlWakeServed),
                static_cast<long>(requiredControlWakeSerial));
            mod::Log(
                "Takeover: native held-delay timeout cleanup complete "
                "serial=%ld evidence=%s wake=%ld/%ld required=%ld",
                static_cast<long>(nativeTimeoutSerial),
                helperAlive ? "BEL served" : "helper exited",
                static_cast<long>(controlWakeRequest),
                static_cast<long>(controlWakeServed),
                static_cast<long>(requiredControlWakeSerial));
            PublishConsoleError("Opponent timed out.");
            InterlockedExchange(
                &g_hostBlock->nativeDelayTimeoutHandledSerial,
                nativeTimeoutSerial);
            RefreshRuntimeStatus(ioStatus);
        }
    }

    if (!helperAlive)
    {
        RestoreTemporaryHostProtocolOverride(
            "Revival netplay session helper ended before listener acknowledgement");
        LogRevival102jDeepSnapshot("Tick.10.helper_not_alive_pre_recovery", ioStatus);
        RefreshRuntimeStatus(ioStatus);
        const bool runtimeReady = HasRuntimeReadySignal(ioStatus);
        if (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
        {
            if (g_localInitAppliedForSession)
            {
                const bool spectatorStarting =
                    g_localRoleFlag == kLocalRoleSpectate
                    || (g_hostBlock != nullptr
                        && g_hostBlock->initParams[0] == kLocalRoleSpectate);
                if (spectatorStarting)
                {
                    SetPhase(
                        ioStatus,
                        NetbridgePhase::Failed,
                        "EfzRevival spectator process ended during initialization");
                    g_localInitAppliedForSession = false;
                    const bool localInitOk = ForceLocalPlayInit();
                    mod::Log(
                        "Takeover: spectator helper exited before handoff; "
                        "local recovery result=%d postInitAttempted=%d postInitOk=%d",
                        localInitOk ? 1 : 0,
                        g_spectatorPostInitAttemptedForSession ? 1 : 0,
                        g_spectatorPostInitSucceededForSession ? 1 : 0);
                    LogRevival102jDeepSnapshot(
                        "Tick.11.spectator_helper_exit_recovered",
                        ioStatus);
                    return;
                }

                if (runtimeReady)
                {
                    SetPhase(ioStatus, NetbridgePhase::Connected, nullptr);
                    if (ioConnectStartTick != nullptr)
                    {
                        *ioConnectStartTick = GetTickCount();
                    }
                }
                else if (ioStatus->delaySetupReady != 0)
                {
                    SetPhase(ioStatus, NetbridgePhase::DelaySetup, nullptr);
                    mod::Log("Takeover: helper process exited after init; keeping delay setup stage active");
                }
                else
                {
                    SetPhase(ioStatus, NetbridgePhase::Connecting, "awaiting runtime sync");
                    mod::Log("Takeover: helper process exited after init; still waiting for runtime sync");
                }
            }
            else
            {
                SetPhase(ioStatus, NetbridgePhase::Failed, "EfzRevival process ended during connect");
                ReinitLocalPlay();
            }
        }
        else
        {
            // Connected phase: peer process has exited.  Regardless of
            // whether the rollback runtime still reports ready, terminate
            // the session.  Keeping it alive leads to a stuck state where
            // no further progress is possible.
            SetPhase(ioStatus, NetbridgePhase::SessionEnded,
                     runtimeReady ? "peer disconnected" : nullptr);
            ReinitLocalPlay();
            if (runtimeReady)
            {
                mod::Log("Takeover: helper process exited during Connected phase - forcing SessionEnded (runtime was still ready)");
            }
        }
        LogRevival102jDeepSnapshot("Tick.12.helper_exit_branch_complete", ioStatus);
        return;
    }

    if (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
    {
        const DWORD now = GetTickCount();
        const LONG cpHits = static_cast<LONG>(g_hostBlock != nullptr ? g_hostBlock->dbgCreateProcessHits : 0);
        const LONG wpmHits = static_cast<LONG>(g_hostBlock != nullptr ? g_hostBlock->dbgWriteProcessHits : 0);
        const LONG crtHits = static_cast<LONG>(g_hostBlock != nullptr ? g_hostBlock->dbgCreateRemoteThreadHits : 0);
        if (!g_observedTakeoverCreatePath && (cpHits > 0 || wpmHits > 0 || crtHits > 0))
        {
            g_observedTakeoverCreatePath = true;
            mod::Log(
                "Takeover: observed takeover create path hits cp=%ld wpm=%ld crt=%ld",
                static_cast<long>(cpHits),
                static_cast<long>(wpmHits),
                static_cast<long>(crtHits));
            LogRevival102jDeepStep("Tick.20.takeover_create_path_observed", ioStatus);
        }

        if (g_peerProcessOwnership == PeerProcessOwnership::SpawnedHelper
            && !g_observedTakeoverCreatePath
            && g_revivalProcess != nullptr
            && g_revivalProcessId != 0
            && g_remoteInjectedSelfBase != 0
            && (g_lastLatePatchRetryTick == 0 || now - g_lastLatePatchRetryTick >= 500))
        {
            if (g_remoteInjectedPatchMap.empty())
            {
                g_remoteInjectedPatchMap =
                    BuildPatchMap(g_remoteInjectedSelfBase);
            }
            const bool patchedLate = PatchIat(
                g_revivalProcess,
                g_revivalProcessId,
                g_remoteInjectedPatchMap,
                false);
            ++g_latePatchRetryAttempts;
            if (patchedLate)
            {
                ++g_latePatchRetrySuccesses;
            }
            const bool resultChanged = !g_lastLatePatchRetryResultValid || g_lastLatePatchRetryResult != patchedLate;
            if (resultChanged
                || g_lastLatePatchRetryLogTick == 0
                || now - g_lastLatePatchRetryLogTick >= kLatePatchRetryLogIntervalMs)
            {
                mod::Log(
                    "Takeover: late PatchIat retry result=%d attempts=%lu success=%lu elapsed=%lums",
                    patchedLate ? 1 : 0,
                    static_cast<unsigned long>(g_latePatchRetryAttempts),
                    static_cast<unsigned long>(g_latePatchRetrySuccesses),
                    static_cast<unsigned long>(ioStatus->phaseTick != 0 ? (now - ioStatus->phaseTick) : 0));
                g_lastLatePatchRetryLogTick = now;
            }
            g_lastLatePatchRetryResult = patchedLate;
            g_lastLatePatchRetryResultValid = true;
            g_lastLatePatchRetryTick = now;
        }

        if (g_lastConnectingDiagnosticTick == 0 || now - g_lastConnectingDiagnosticTick >= 2000)
        {
            g_lastConnectingDiagnosticTick = now;
            const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
            const uintptr_t looseSessionPtr = ReadSessionPointerFromRevivalLoose();
            int pingMs = -1;
            int delayFrames = -1;
            const int roleFromDll = ReadRoleFlagFromRevival();
            RevivalSyncFlags syncFlags = {};
            (void)ReadRevivalSyncFlags(&syncFlags);
            if (sessionPtr != 0)
            {
                (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetPingMs), &pingMs);
                (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay), &delayFrames);
            }
            if (sessionPtr != 0
                && looseSessionPtr != 0
                && looseSessionPtr != sessionPtr
                && (g_lastSessionPointerMismatchTick == 0 || now - g_lastSessionPointerMismatchTick >= 2000))
            {
                g_lastSessionPointerMismatchTick = now;
                mod::Log(
                    "Takeover: session pointer mismatch strict=0x%08lX loose=0x%08lX offset=0x%04lX",
                    static_cast<unsigned long>(sessionPtr),
                    static_cast<unsigned long>(looseSessionPtr),
                    static_cast<unsigned long>(g_lastSessionPtrOffset));
            }
            const int pingDiag = (pingMs >= 0 && pingMs < 60000) ? pingMs : -1;
            const int delayDiag = (delayFrames >= 0 && delayFrames < 128) ? delayFrames : -1;
            const int exeSyncDiag =
                (syncFlags.gameMode >= 0 || syncFlags.mode0Flag1084 >= 0)
                    ? 1
                    : 0;
            const int dllSyncDiag =
                (syncFlags.sessionByte >= 0
                 || syncFlags.globalFlag4964 >= 0
                 || syncFlags.globalFlag4965 >= 0)
                    ? 1
                    : 0;
            const int roleFromDllDiag = (roleFromDll >= 0) ? 1 : 0;
            const int sessionFromDllDiag = (sessionPtr != 0) ? 1 : 0;
            mod::Log(
                "Takeover: connecting diag session=0x%08lX sessionOff=0x%04lX roleFlag=%d ping=%d delay=%d sync(mode=%d flag1084=%d session=%d flags=%d/%d ready=%d) src(exe=%d dll=%d roleDll=%d sessDll=%d) hits(rc=%ld auto=%ld cp=%ld wpm=%ld crt=%ld)",
                static_cast<unsigned long>(sessionPtr),
                static_cast<unsigned long>(g_lastSessionPtrOffset),
                ioStatus->roleFlag,
                pingDiag,
                delayDiag,
                syncFlags.gameMode,
                syncFlags.mode0Flag1084,
                syncFlags.sessionByte,
                syncFlags.globalFlag4964,
                syncFlags.globalFlag4965,
                syncFlags.inRollbackSyncState ? 1 : 0,
                exeSyncDiag,
                dllSyncDiag,
                roleFromDllDiag,
                sessionFromDllDiag,
                static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgReadConsoleHits : 0),
                static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgReadConsoleAutoHits : 0),
                static_cast<long>(cpHits),
                static_cast<long>(wpmHits),
                static_cast<long>(crtHits));
            LogRevival102jDeepStep("Tick.21.connecting_periodic", ioStatus);
        }

        const bool spectateRoleActive =
            g_hostBlock != nullptr && g_hostBlock->initParams[0] == kLocalRoleSpectate;
        if (spectateRoleActive
            && (g_lastSpectateConsoleSnapshotTick == 0
                || now - g_lastSpectateConsoleSnapshotTick >= 4000))
        {
            g_lastSpectateConsoleSnapshotTick = now;
            LogPendingSpectateConsoleSnapshot();
        }
    }

    const bool initHandshakeEligible =
        (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
        && !g_localInitAppliedForSession
        && g_hostInitEvent != nullptr
        && g_hostBlock != nullptr
        && g_localInitFn != nullptr;

    if (initHandshakeEligible
        && IsInsideFrameTick()
        && WaitForSingleObject(g_hostInitEvent, 0) == WAIT_OBJECT_0)
    {
        // g_hostInitEvent is an auto-reset event.  The zero-timeout wait above
        // consumes its one signal, so put the signal back for the post-tick
        // takeover::Tick() call that is allowed to run the destructor/init.
        // Without this, every session remains stuck in Connecting because the
        // deferred call can no longer observe the helper's init handshake.
        if (!SetEvent(g_hostInitEvent))
        {
            mod::Log(
                "Takeover: failed to preserve deferred init handshake event: %s",
                ErrorString(GetLastError()).c_str());
        }
        LogRevival102jDeepStep("Tick.30.init_event_detected_and_rearmed", ioStatus);
        const LONG wasPending =
            InterlockedExchange(&g_deferredLifecycleWorkRequested, 1);
        if (wasPending == 0)
        {
            mod::Log(
                "Takeover: init handshake ready inside active Revival tick; "
                "deferring destructor/init until post-tick");
        }
    }

    if (initHandshakeEligible && !IsInsideFrameTick())
    {
        const DWORD initReady = WaitForSingleObject(g_hostInitEvent, 0);
        if (initReady == WAIT_OBJECT_0)
        {
            LogRevival102jDeepSnapshot("Tick.31.init_event_post_tick_consumed", ioStatus);
            // --- Diagnostic dump before init handshake (2nd-session crash investigation) ---
            LogSessionDiagnosticState("Tick_init_handshake_pre");

            int initParams[2] = {g_hostBlock->initParams[0], g_hostBlock->initParams[1]};
            if (initParams[1] == 0)
            {
                initParams[1] = 102;
            }

            mod::Log(
                "Takeover: init handshake observed (deferred) mode=%d magic=%d serial=%ld",
                initParams[0],
                initParams[1],
                static_cast<long>(g_hostBlock->initSerial));

            // --- Full init write snapshot BEFORE ---
            LogInitWriteSnapshot("Tick_init_pre");

            // 1.02j installs 0x401642 only once, outside exported init().
            // Capture it before invoking the verified session destructor.
            const uintptr_t oldSessionPtr = ReadSessionPointerFromRevival();
            if (IsActiveRevival102jProfile())
            {
                SaveExeDispatchHookBytes();
            }
            DestroyCurrentSession("Tick_init_handshake");
            LogRevival102jDeepStep("Tick.32.old_session_destroyed", ioStatus);

            // Prevent init() from chaining another trampoline at 0x401582.
            mod::Log(
                "Tick_init_handshake: about to save EXE hook bytes before "
                "init() mode=%d oldSession=0x%08lX",
                initParams[0],
                static_cast<unsigned long>(oldSessionPtr));
            SaveExeFrameHookBytes();
            if (!IsActiveRevival102jProfile())
            {
                // Legacy builds may replace this hook from exported init().
                SaveExeDispatchHookBytes();
            }
            // Restore original (pre-hook) bytes at mode-ctor hook sites
            // BEFORE init() so the new trampoline copies clean EXE bytes
            // instead of stale hooks from a previous session's mode.
            RestoreModeCtorOriginalBytes();
            ResetModeConstructorTrampolineCache();
            LogRevival102jDeepSnapshot("Tick.33.exported_init_pre", ioStatus);

            // Dump the 10 bytes at 0x401582 right before init().
            {
                uint8_t pre[10] = {};
                memcpy(pre, reinterpret_cast<const void*>(0x401582), 10);
                mod::Log(
                    "Tick_init_handshake: 0x401582 pre-init  "
                    "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
                    pre[0], pre[1], pre[2], pre[3], pre[4],
                    pre[5], pre[6], pre[7], pre[8], pre[9]);
            }

            mod::Log("Tick_init_handshake: calling init(mode=%d, magic=%d)",
                     initParams[0], initParams[1]);
            CloseMirrorLogFiles();
            const int initResult = g_localInitFn(initParams);
            LogRevival102jDeepSnapshot("Tick.34.exported_init_returned_unrestored", ioStatus);

            // Dump the 10 bytes AFTER init() to see what sub_1006F160 wrote.
            {
                uint8_t post[10] = {};
                memcpy(post, reinterpret_cast<const void*>(0x401582), 10);
                mod::Log(
                    "Tick_init_handshake: 0x401582 post-init "
                    "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
                    post[0], post[1], post[2], post[3], post[4],
                    post[5], post[6], post[7], post[8], post[9]);
            }

            // Undo the EXE frame-hook chain growth at 0x401582.
            // Mode-ctor originals were already restored before init();
            // for non-local modes init() installs fresh hooks that don't
            // chain through stale trampolines.
            mod::Log("Tick_init_handshake: restoring saved EXE hook bytes (mode=%d)", initParams[0]);
            RestoreExeFrameHookBytes();

            // Verify the restore worked.
            {
                uint8_t verify[10] = {};
                memcpy(verify, reinterpret_cast<const void*>(0x401582), 10);
                mod::Log(
                    "Tick_init_handshake: 0x401582 restored  "
                    "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
                    verify[0], verify[1], verify[2], verify[3], verify[4],
                    verify[5], verify[6], verify[7], verify[8], verify[9]);
            }

            // Restore the saved 0x401642 state. On 1.02j this must remain the
            // one-time persistent dispatcher because init() never recreates it.
            RestoreExeDispatchHookBytesAfterSessionInit(initParams[0]);

            // Fix up relative instructions in mode-constructor trampolines.
            FixupModeConstructorTrampolines("Tick_init_handshake");
            LogRevival102jDeepSnapshot("Tick.35.hooks_restored_and_fixed", ioStatus);

            // --- Dump bytes at 0x401642 (double-speed investigation) --------
            // After init(), check that the REPLACEMENT hook at 0x401642 is
            // still intact (first byte should be 0xE9 = JMP near).  If the
            // hook was undone, the main loop's vtable[1] call AND Revival's
            // sub_1006D0B0 both run the screen update → doubled game speed.
            {
                uint8_t hb[16] = {};
                for (int i = 0; i < 16; ++i)
                {
                    if (!SafeReadByte(reinterpret_cast<const void*>(0x401642 + i), &hb[i]))
                        hb[i] = 0xCC;
                }
                mod::Log(
                    "Tick_init_handshake: 0x401642 post-init "
                    "[%02X %02X %02X %02X %02X %02X %02X %02X "
                    " %02X %02X %02X %02X %02X %02X %02X %02X]",
                    hb[0], hb[1], hb[2], hb[3], hb[4], hb[5], hb[6], hb[7],
                    hb[8], hb[9], hb[10], hb[11], hb[12], hb[13], hb[14], hb[15]);
            }

            g_localRoleFlag = initParams[0];
            g_localInitAppliedForSession = true;

            const uintptr_t newSessionPtr = ReadSessionPointerFromRevival();

            // --- Full init write snapshot AFTER init() ---
            LogInitWriteSnapshot("Tick_init_post");
            LogRevival102jDeepSnapshot("Tick.36.local_init_applied", ioStatus);

            mod::Log(
                "Takeover: local init(mode=%d magic=%d) result=%d [deferred] oldSession=0x%08lX newSession=0x%08lX",
                initParams[0],
                initParams[1],
                initResult,
                static_cast<unsigned long>(oldSessionPtr),
                static_cast<unsigned long>(newSessionPtr));

            // Zero out the initComplete field on the new session BEFORE
            // calling InvokeStartInitPlayer.  When the heap reuses the same
            // address as a previous session, initComplete may still be 1
            // (stale), which would cause InvokeStartInitPlayer to skip the
            // critical sub_10072880 call - leaving the session in an
            // uninitialized state and freezing the game.
            //
            // Only meaningful for online sessions (mode 0) where
            // InvokeStartInitPlayer runs.  For spectator/local/tournament
            // sessions, the initComplete offset overlaps with different
            // fields in the smaller session object.
            if (initParams[0] == kLocalRoleOnline
                && newSessionPtr != 0 && newSessionPtr >= 0x00100000u
                && g_activeRevival->sessionOffsetInitComplete != 0)
            {
                int prevInitComplete = -1;
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(newSessionPtr + g_activeRevival->sessionOffsetInitComplete),
                    &prevInitComplete);
                if (prevInitComplete == 1)
                {
                    DWORD oldProt = 0;
                    void* initCompAddr = reinterpret_cast<void*>(
                        newSessionPtr + g_activeRevival->sessionOffsetInitComplete);
                    if (VirtualProtect(initCompAddr, sizeof(int), PAGE_READWRITE, &oldProt))
                    {
                        int zero = 0;
                        memcpy(initCompAddr, &zero, sizeof(int));
                        VirtualProtect(initCompAddr, sizeof(int), oldProt, &oldProt);
                        mod::Log(
                            "Takeover: cleared stale initComplete=1 on reused session 0x%08lX before StartInitPlayer",
                            static_cast<unsigned long>(newSessionPtr));
                    }
                    else
                    {
                        mod::Log(
                            "Takeover: FAILED to clear stale initComplete=1 on session 0x%08lX - "
                            "VirtualProtect err=%lu (StartInitPlayer will likely skip sub_10072880!)",
                            static_cast<unsigned long>(newSessionPtr),
                            static_cast<unsigned long>(GetLastError()));
                    }
                }
            }

            const bool startInitOk = InvokeStartInitPlayer(initParams[0]);

            // --- Detect netplay role (host / client / spectator) ---
            if (initParams[0] == kLocalRoleSpectate)
            {
                g_netplayRole = kNetplayRoleSpectator;
                mod::Log("Takeover: netplay role = Spectator");
            }
            else if (initParams[0] == kLocalRoleOnline && startInitOk)
            {
                // Read the activePlayer field that StartInitPlayer just wrote.
                // 0 = host (P1), 1 = joiner/client (P2 - inputs were swapped).
                const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
                int activePlayer = -1;
                if (sessionPtr != 0)
                {
                    (void)SafeReadInt(
                        reinterpret_cast<const void*>(
                            sessionPtr + g_activeRevival->sessionOffsetActivePlayer),
                        &activePlayer);
                }
                if (activePlayer == 1)
                {
                    g_netplayRole = kNetplayRoleClient;
                    // Revival's StartInitPlayer just swapped the P1/P2 input
                    // blocks for the P2/client side.  Record it durably so the
                    // swap is reversed even if this session ends via a path
                    // that clears the role without running ForceLocalPlayInit.
                    SetClientInputSwapApplied(true);
                    mod::Log("Takeover: netplay role = Client (P2, inputs swapped)");
                }
                else
                {
                    g_netplayRole = kNetplayRoleHost;
                    mod::Log("Takeover: netplay role = Host (P1, activePlayer=%d)",
                             activePlayer);
                }
            }

            // --- Snapshot after StartInitPlayer (writes session fields) ---
            LogInitWriteSnapshot("Tick_startInitPlayer_post");
            LogRevival102jDeepSnapshot("Tick.37.start_init_player_returned", ioStatus);

            mod::Log(
                "Takeover: InvokeStartInitPlayer result=%d [deferred]",
                startInitOk ? 1 : 0);

            // init(1,102) only constructs the spectator object. Legacy builds
            // use vtable[1] for the required match init; 1.02j moved it to
            // verified vtable[2] (RVA 0x5E640), while vtable[1] became its
            // MinGW deleting destructor.
            if (initParams[0] == kLocalRoleSpectate)
            {
                if (IsActiveRevival102jProfile())
                {
                    mod::Log(
                        "Takeover: 1.02j spectator object constructed; "
                        "waiting for native PID/wire readiness before vtable[2]");
                }
                else
                {
                    const bool vtableInitOk = InvokeSessionVtableInit("Tick_spectate");
                    g_spectatorPostInitAttemptedForSession = true;
                    g_spectatorPostInitSucceededForSession = vtableInitOk;
                    mod::Log(
                        "Takeover: spectator vtable init result=%d [deferred]",
                        vtableInitOk ? 1 : 0);
                    LogInitWriteSnapshot("Tick_spectateVtable1_post");
                }
            }

            // Init-snapshot: record the DLL ExitProcess Jcc call-site bytes
            // before patching them.  RestoreDllExitProcessPatches() (called in
            // CancelSessionUnlocked on exit) will undo exactly these changes.
            // For online/spectate the Jcc sites only cover the tournament tick
            // path; the online ExitProcess path is handled by the IAT hook +
            // OurFrameDispatch longjmp.  Logging here lets us correlate the
            // init snapshot with the corresponding restore in the log file.
            mod::Log(
                "Takeover: init snapshot step SaveAndApplyDllExitProcessPatches "
                "for mode=%d (to be reversed by RestoreDllExitProcessPatches on exit)",
                initParams[0]);
            const bool exitGuardsApplied = SaveAndApplyDllExitProcessPatches();
            if (exitGuardsApplied
                && (initParams[0] == kLocalRoleOnline
                    || initParams[0] == kLocalRoleSpectate))
            {
                PrimeGracefulQuitRingForSession();
            }
            LogRevival102jDeepSnapshot("Tick.38.exit_guards_applied", ioStatus);
            mod::Log(
                "Takeover: init snapshot complete for mode=%d",
                initParams[0]);

            StabilizeOnlineSessionBindingAfterInit(initParams[0]);

            if (initParams[0] == kLocalRoleOnline
                || initParams[0] == kLocalRoleSpectate
                || initParams[0] == kLocalRoleTournament)
            {
                const bool textEnableOk = SetRevivalTextRenderingEnabled(
                    true,
                    "Tick_initSequence_session_start");
                mod::Log(
                    "Takeover: session text rendering enable mode=%d result=%d",
                    initParams[0],
                    textEnableOk ? 1 : 0);
            }

            // NOTE: The spectate ring-buffer flush that used to live here
            // has been moved to StartSession (before ResumeThread).  Flushing
            // here - after the child process has already been running for up
            // to a game frame - discarded the beginning of the host's input
            // replay stream, causing spectator desync at charselect.
            // See the "Pre-launch ring buffer flush" block in StartSession.

            // --- Final snapshot after all init steps complete ---
            LogInitWriteSnapshot("Tick_initSequence_complete");
            MarkRevivalSyncDiagnosticsSessionStart("Tick_initSequence_complete");
            LogSessionDiagnosticState("Tick_init_handshake_post");
            LogRevival102jDeepSnapshot("Tick.39.init_sequence_complete", ioStatus);
        }
    }

    if (g_localInitAppliedForSession)
    {
        RepairRollbackHistoryBindingsIfNeeded();
    }

    // 1.02j spectator post-init opens the peer process from the PID copied
    // through Init_Spec.  The injected init event is emitted earlier, when
    // the helper merely attempts the remote init call, so invoking vtable[2]
    // in that event handler can race the native wire/PID publication.  Wait
    // until those prerequisites are observable, invoke exactly once, and do
    // not allow phase promotion unless all postconditions validate.
    if (g_localInitAppliedForSession
        && g_localRoleFlag == kLocalRoleSpectate
        && IsActiveRevival102jProfile()
        && !g_spectatorPostInitAttemptedForSession
        && IsRevival102jSpectatorPostInitReady("Tick_spectate_ready"))
    {
        LogRevival102jDeepSnapshot("Tick.50.spectator_post_init_ready", ioStatus);
        g_spectatorPostInitAttemptedForSession = true;
        g_spectatorPostInitSucceededForSession =
            InvokeRevival102jSpectatorPostInit("Tick_spectate");
        mod::Log(
            "Takeover: 1.02j spectator vtable[2] one-shot result=%d",
            g_spectatorPostInitSucceededForSession ? 1 : 0);
        LogInitWriteSnapshot(
            g_spectatorPostInitSucceededForSession
                ? "Tick_spectateVtable2_post_102j"
                : "Tick_spectateVtable2_failed_102j");
        LogRevival102jDeepSnapshot("Tick.51.spectator_post_init_returned", ioStatus);

        if (!g_spectatorPostInitSucceededForSession)
        {
            SetPhase(
                ioStatus,
                NetbridgePhase::Failed,
                "EfzRevival spectator initialization failed");
            g_localInitAppliedForSession = false;
            const bool localInitOk = ForceLocalPlayInit();
            mod::Log(
                "Takeover: spectator post-init failure local recovery result=%d",
                localInitOk ? 1 : 0);
            LogRevival102jDeepSnapshot("Tick.52.spectator_post_init_recovered", ioStatus);
            return;
        }
    }

    if (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
    {
        LONG delayPromptSerial = 0;
        LONG delayPromptServedSerial = 0;
        ReadDelayPromptSignal(&delayPromptSerial, &delayPromptServedSerial);
        const LONG observedSerial = InterlockedCompareExchange(&g_injectedConnectedFromDelayPromptSerial, 0, 0);
        if (delayPromptSerial > 0 && delayPromptSerial != observedSerial)
        {
            InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, delayPromptSerial);
            mod::Log(
                "Takeover: delay prompt observed promptSerial=%ld servedSerial=%ld (waiting for init handshake/runtime sync before connected)",
                static_cast<long>(delayPromptSerial),
                static_cast<long>(delayPromptServedSerial));
        }
    }

    RefreshRuntimeStatus(ioStatus);

    // --- Console error detection ---
    // If the helper published a connection error (e.g., "Connection timed out",
    // "Source quit or timed out", "Host timed out", "Remote timed out",
    // "Socket error"), act immediately. Active-opponent quit packets are
    // promoted into this path earlier by console capture; raw "<endpoint> died"
    // lines are no longer treated as fatal by themselves.
    if (ioStatus->consoleErrorSerial > 0)
    {
        const bool desyncDetected =
            (phase == NetbridgePhase::Connecting
                || phase == NetbridgePhase::DelaySetup
                || phase == NetbridgePhase::Connected)
            && ConsoleErrorIndicatesDesync(ioStatus->consoleErrorText);
        if (desyncDetected)
        {
            mod::Log(
                "Takeover: desync console error detected serial=%d text='%s' phase=%d - emitting forced diagnostic report",
                ioStatus->consoleErrorSerial,
                ioStatus->consoleErrorText,
                static_cast<int>(phase));
            LogSessionDiagnosticStateForced("console_desync_detected");
        }

        if (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
        {
            mod::Log(
                "Takeover: console error detected serial=%d text='%s' phase=%d - transitioning to Failed",
                ioStatus->consoleErrorSerial,
                ioStatus->consoleErrorText,
                static_cast<int>(phase));
            SetPhase(ioStatus, NetbridgePhase::Failed, ioStatus->consoleErrorText);
            LogRevival102jDeepSnapshot("Tick.60.console_error_connecting", ioStatus);
            ReinitLocalPlay();
            return;
        }
        if (phase == NetbridgePhase::Connected)
        {
            // During Connected phase (charselect/gameplay after handoff),
            // the helper process detected a network disconnect.  The per-
            // frame tick hook (OurPerFrameTickHook) also polls this flag
            // and performs full recovery.  Here we transition the bridge
            // phase so any code that checks GetStatus() sees the session
            // has ended.  The heavy recovery (ForceLocalPlayInit, restore
            // patches, ForceGameModeToTitle) is handled by the tick hook.
            mod::Log(
                "Takeover: console error detected serial=%d text='%s' phase=Connected - transitioning to SessionEnded",
                ioStatus->consoleErrorSerial,
                ioStatus->consoleErrorText);
            SetPhase(ioStatus, NetbridgePhase::SessionEnded, ioStatus->consoleErrorText);
            LogRevival102jDeepSnapshot("Tick.61.console_error_connected", ioStatus);
            return;
        }
    }

    NetbridgePhase currentPhase = static_cast<NetbridgePhase>(ioStatus->phase);
    RuntimeReadyProbe runtimeProbe = {};
    if (currentPhase == NetbridgePhase::Connecting || currentPhase == NetbridgePhase::DelaySetup)
    {
        runtimeProbe = EvaluateRuntimeReadyProbe(ioStatus);
        const uint32_t probeMask = BuildRuntimeReadyProbeMask(runtimeProbe);
        const DWORD now = GetTickCount();
        const bool changed = !g_lastRuntimeReadyProbeMaskValid || g_lastRuntimeReadyProbeMask != probeMask;
        if (changed || g_lastRuntimeReadyProbeLogTick == 0 || now - g_lastRuntimeReadyProbeLogTick >= 3000)
        {
            g_lastRuntimeReadyProbeMask = probeMask;
            g_lastRuntimeReadyProbeMaskValid = true;
            g_lastRuntimeReadyProbeLogTick = now;
            mod::Log(
                "Takeover: runtime-ready probe ready=%d source=%s localInit=%d prompt=%d input=%d session=%d bind(pid=%d handle=%d) syncReady=%d role=%d",
                runtimeProbe.ready ? 1 : 0,
                runtimeProbe.source != nullptr ? runtimeProbe.source : "none",
                runtimeProbe.localInitApplied ? 1 : 0,
                runtimeProbe.delayPromptSeen ? 1 : 0,
                runtimeProbe.delayInputApplied ? 1 : 0,
                runtimeProbe.sessionPointerValid ? 1 : 0,
                runtimeProbe.helperPidMatches ? 1 : 0,
                runtimeProbe.helperHandleMatches ? 1 : 0,
                runtimeProbe.nativeSyncReady ? 1 : 0,
                ioStatus->roleFlag);
            LogRevival102jDeepStep("Tick.70.runtime_probe_changed_or_periodic", ioStatus);
        }
    }

    if ((currentPhase == NetbridgePhase::Connecting || currentPhase == NetbridgePhase::DelaySetup)
        && g_localInitAppliedForSession
        && ioStatus->delaySetupReady != 0)
    {
        if (currentPhase != NetbridgePhase::DelaySetup)
        {
            SetPhase(ioStatus, NetbridgePhase::DelaySetup, nullptr);
            currentPhase = NetbridgePhase::DelaySetup;
            mod::Log(
                "Takeover: promoted to delay setup prompt=%d/%d ping=%d delay=%d",
                ioStatus->delayPromptSerial,
                ioStatus->delayPromptServedSerial,
                ioStatus->pingMs,
                ioStatus->rollbackFrames);
            LogRevival102jDeepSnapshot("Tick.71.promoted_delay_setup", ioStatus);
        }
    }

    if ((currentPhase == NetbridgePhase::Connecting || currentPhase == NetbridgePhase::DelaySetup)
        && g_localInitAppliedForSession
        && runtimeProbe.ready)
    {
        SetPhase(ioStatus, NetbridgePhase::Connected, nullptr);
        if (ioConnectStartTick != nullptr)
        {
            *ioConnectStartTick = GetTickCount();
        }
        mod::Log(
            "Takeover: promoted to connected after runtime ready source=%s sync=%d/%d/%d ping=%d delay=%d",
            runtimeProbe.source != nullptr ? runtimeProbe.source : "none",
            ioStatus->syncGameMode,
            ioStatus->syncMode0Flag1084,
            ioStatus->syncSessionByte,
            ioStatus->pingMs,
            ioStatus->rollbackFrames);
        LogRevival102jDeepSnapshot("Tick.72.promoted_connected", ioStatus);
    }

    // Spectator sessions don't go through the rollback sync handshake, so
    // the runtime-ready probe never fires.  Promote to Connected as soon as
    // init is applied and the DLL exit-process patches are saved (the last
    // step of the spectator init sequence).  This allows phase-gated code
    // (name reading, disconnect handling) to work correctly for spectators.
    if ((currentPhase == NetbridgePhase::Connecting || currentPhase == NetbridgePhase::DelaySetup)
        && g_localInitAppliedForSession
        && g_localRoleFlag == kLocalRoleSpectate
        && g_spectatorPostInitSucceededForSession
        && AreDllExitPatchesSaved())
    {
        SetPhase(ioStatus, NetbridgePhase::Connected, nullptr);
        if (ioConnectStartTick != nullptr)
        {
            *ioConnectStartTick = GetTickCount();
        }
        mod::Log(
            "Takeover: spectator promoted to connected "
            "(post-init/liveness validated, exit patches saved)");
        LogRevival102jDeepSnapshot("Tick.73.spectator_promoted_connected", ioStatus);
    }
}

// ---------------------------------------------------------------------------
// CancelSessionUnlocked - shared body for CancelSession and exit interception.
// Caller MUST hold g_mutex.
// ---------------------------------------------------------------------------
static void CancelSessionUnlocked(const char* reason, NetbridgeStatus* ioStatus)
{
    RestoreTemporaryHostProtocolOverride(
        reason != nullptr
            ? reason
            : "Revival netplay session cancel");

    // Idempotency guard for the normal match-end double-cancel: the flow fires
    // CancelSession twice per boundary - "match_ended" (heavy teardown, parks
    // phase at SessionEnded) then "no_overlay_session_ended" (drives phase to
    // Idle).  Every operation in the body is individually idempotent EXCEPT
    // ForceLocalPlayInit, which unconditionally destroys+rebuilds the session
    // and re-runs Revival's init() - running it a second time per boundary is a
    // prime suspect for the 2nd/3rd-session desync.  The session epoch (bumped
    // once per session at StartSession) lets a repeat teardown within one
    // boundary be recognized and skip that non-idempotent step.  The peer-death
    // path (where "no_overlay_session_ended" is the FIRST cancel) has no prior
    // teardown for this epoch, so it runs the full teardown normally.
    const uint32_t boundaryEpoch =
        netplay::bridge::session_lifecycle::CurrentSessionEpoch();
    const uint32_t cleanupInvocation =
        netplay::bridge::session_lifecycle::NextCleanupInvocation();
    (void)cleanupInvocation;  // read only by the (release-compiled-out) trace
    const bool repeatTeardown =
        boundaryEpoch != 0
        && netplay::bridge::session_lifecycle::LastTornDownEpoch() == boundaryEpoch;
    MOD_LIFECYCLE_TRACE(
        "LIFECYCLE ep=%u cleanup=%u stage=cancel_session action=%s reason=%s role=%d",
        boundaryEpoch, cleanupInvocation,
        repeatTeardown ? "repeat" : "begin",
        (reason != nullptr) ? reason : "", g_localRoleFlag);

    LogRevival102jDeepSnapshot("CancelSession.01.entry", ioStatus);
    // --- Diagnostic dump before teardown (2nd-session crash investigation) ---
    LogSessionDiagnosticState("CancelSession_entry");
    mod::Log(
        "DIAG[CancelSession]: reason='%s' forceLocalPlayInitCount=%d",
        (reason != nullptr) ? reason : "",
        GetForceLocalPlayInitCount());

    // --- Session boundary cleanup logging ---
    {
        uint8_t hookA[10] = {};
        uint8_t hookB[8] = {};
        memcpy(hookA, reinterpret_cast<const void*>(0x401582), 10);
        memcpy(hookB, reinterpret_cast<const void*>(0x401642), 8);
        mod::Log(
            "CancelSession: EXE hooks at teardown "
            "0x401582=[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X] "
            "0x401642=[%02X %02X %02X %02X %02X %02X %02X %02X]",
            hookA[0], hookA[1], hookA[2], hookA[3], hookA[4],
            hookA[5], hookA[6], hookA[7], hookA[8], hookA[9],
            hookB[0], hookB[1], hookB[2], hookB[3], hookB[4],
            hookB[5], hookB[6], hookB[7]);
    }
    // Log init-once guard state at teardown.
    if (g_activeRevival != nullptr)
    {
        HMODULE revival = GetModuleHandleA("EfzRevival.dll");
        if (revival != nullptr)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
            const uint16_t guardVal = *reinterpret_cast<const volatile uint16_t*>(
                base + g_activeRevival->initOnceGuardOffset);
            const uintptr_t timerPtr = *reinterpret_cast<const volatile uintptr_t*>(
                base + g_activeRevival->timerPtrOffset);
            const uintptr_t renderCtx = *reinterpret_cast<const volatile uintptr_t*>(
                base + g_activeRevival->renderContextGlobalOffset);
            mod::Log(
                "CancelSession: DLL globals guard=0x%04X timer=0x%08lX renderCtx=0x%08lX",
                static_cast<unsigned>(guardVal),
                static_cast<unsigned long>(timerPtr),
                static_cast<unsigned long>(renderCtx));
        }
    }

    InterlockedExchange(&g_startAbortRequested, 1);
    FlushPendingConsoleOutput("cancel");

    const bool suppressSharedRecoveryTeardown =
        netplay::bridge::recovery::ShouldSuppressLegacyGameplayExitCleanup();
    if (suppressSharedRecoveryTeardown)
    {
        mod::Log(
            "GAMEPLAY_EXIT_RECOVERY_SUPPRESS_OLD_TEARDOWN reason=cancel_session_unlocked inProgress=%d pendingMenu=%d completed=%d origin=%s state=%s",
            netplay::bridge::recovery::IsGameplayExitRecoveryInProgress() ? 1 : 0,
            netplay::bridge::recovery::HasPendingGameplayExitMenuEntry() ? 1 : 0,
            netplay::bridge::recovery::WasGameplayExitRecoveryCompleted() ? 1 : 0,
            netplay::bridge::recovery::CurrentGameplayExitRecoveryOrigin(),
            netplay::bridge::recovery::CurrentGameplayExitRecoveryStateName());
        (void)SuppressDeferredCancelCleanupAfterGameplayRecovery(
            netplay::bridge::recovery::CurrentGameplayExitRecoveryOrigin());
    }

    // Pre-teardown peer-quit broadcast (rank 4).  The natural match-end cancel
    // ("match_ended") and most other cancel reasons historically said nothing
    // to the peer - the old ConsumeOnlineMatchEscGracefulQuit gate here was
    // never armed (dead code), so a fast peer that reached title first would
    // TerminateProcess its helper and leave the slow peer stalled waiting for
    // frames until a network timeout.  Broadcast a native MessageQuit to the
    // peer BEFORE tearing down, unconditionally for online/spectate cancels
    // while the helper is still alive.  This is the same proven mechanism the
    // disconnect-recovery path already uses (gameplay_exit_recovery.cpp:142).
    // It is idempotent on both ends: the receiver re-entering its terminal quit
    // state is a no-op, and the sender's peer-manager lookup no-ops if the
    // session already tore down.  MUST use the ...Impl variant - the public
    // wrapper re-locks the takeover mutex we already hold (self-deadlock).
    // Skip on shutdown/emergency (loader lock) and on repeat teardown.
    const bool broadcastReasonOk =
        reason == nullptr
        || (std::strcmp(reason, "shutdown") != 0
            && std::strcmp(reason, "emergency") != 0);
    const bool shouldBroadcastPeerQuit =
        !repeatTeardown
        && broadcastReasonOk
        // The shared gameplay-exit recovery path broadcasts its own MessageQuit
        // (gameplay_exit_recovery.cpp) before it tears down; don't double-fire.
        && !suppressSharedRecoveryTeardown
        // The broadcast blocks up to 300ms on WaitForSingleObject.  Never run
        // that on the rollback/per-frame tick thread (an in-tick cancel would
        // stall the simulation and itself risk a desync); the normal match-end
        // cancel that needs the broadcast fires from the title hook, not in-tick.
        && !IsInsideFrameTick()
        // Online only.  Spectator MessageQuit via sendQuitAll on the spectator's
        // peer manager is unverified against the players' match and could end a
        // real 2-player game, so keep the historical Online-only scope.
        && g_localRoleFlag == kLocalRoleOnline
        && g_revivalProcess != nullptr;
    if (shouldBroadcastPeerQuit)
    {
        HANDLE helperProcess = nullptr;
        HANDLE sourceHelperProcess = g_revivalProcess;
        DWORD duplicateErr = 0;
        if (!DuplicateHandle(
                GetCurrentProcess(),
                g_revivalProcess,
                GetCurrentProcess(),
                &helperProcess,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS))
        {
            duplicateErr = GetLastError();
        }

        const uintptr_t peerQuitRemoteBase =
            g_peerProcessOwnership
                    == PeerProcessOwnership::ExternalLauncherParent
                ? g_externalLauncherGuardRemoteBase
                : g_remoteInjectedSelfBase;
        mod::Log(
            "Takeover: cancel session peer-quit prepare reason='%s' sourceHelper=%p helperDup=%p helperPid=%lu remoteSelfBase=0x%08lX dupErr=%lu",
            reason != nullptr ? reason : "",
            sourceHelperProcess,
            helperProcess,
            static_cast<unsigned long>(g_revivalProcessId),
            static_cast<unsigned long>(peerQuitRemoteBase),
            static_cast<unsigned long>(duplicateErr));

        const bool peerQuitSent = RequestInjectedPeerQuitBroadcastImpl(
            helperProcess,
            peerQuitRemoteBase,
            g_revivalProcessId,
            "cancel_session_peer_quit",
            300u);
        mod::Log(
            "Takeover: cancel session peer-quit broadcast=%d "
            "reason='%s' helperPid=%lu",
            peerQuitSent ? 1 : 0,
            reason != nullptr ? reason : "",
            static_cast<unsigned long>(g_revivalProcessId));
        if (!peerQuitSent)
        {
            mod::Log(
                "Takeover: cancel session peer-quit broadcast failed; "
                "continuing with local teardown");
        }
    }

    const bool hadProcess = ProcessAlive(ioStatus);
    LogRevival102jDeepStep("CancelSession.10.helper_teardown_begin", ioStatus);
    BOOL terminateOk = TRUE;
    if (!suppressSharedRecoveryTeardown && hadProcess && g_revivalProcess != nullptr)
    {
        terminateOk = TerminatePeerProcessIfOwned(0, "cancel_session");
    }
    if (!suppressSharedRecoveryTeardown)
    {
        (void)ReleasePeerProcessAfterTerminationAttempt(
            terminateOk, ioStatus, "cancel_session");
        // Close the job object so any grandchild processes (cmd.exe, conhost.exe)
        // spawned by EfzRevival.exe are also terminated.  A fresh job will be
        // created for the next StartSession call.
        CloseChildJobObject();
        RestoreDllExitProcessPatches();
        ReinitLocalPlay();
    }
    LogRevival102jDeepSnapshot("CancelSession.11.helper_teardown_complete", ioStatus);

    // ---- Additional cleanup (Issues 1, 4, 5 in CONNECTION_INTERRUPTION doc) ----
    // During normal operation (not process shutdown), reinitialise the DLL
    // session to local-play immediately.  This replaces the dead/neutralised
    // online session object with a live one so the game loop always has a
    // valid vtable for frame dispatch.  During DLL_PROCESS_DETACH we skip
    // this because calling back into EfzRevival.dll under the loader lock
    // can deadlock or crash.
    const bool isShutdown = (reason != nullptr &&
        (std::strcmp(reason, "shutdown") == 0 ||
         std::strcmp(reason, "emergency") == 0));
    const bool isExitInterception =
        reason != nullptr && std::strcmp(reason, "exit_intercepted") == 0;
    const bool alreadyRecoveredLocal102j =
        IsActiveRevival102jProfile()
        && isExitInterception
        && g_localRoleFlag == kLocalRoleLocalPlay
        && GetForceLocalPlayInitCount() > 0
        && ReadSessionPointerFromRevival() != 0;

    // True only when this call actually ran (or deferred) the non-idempotent
    // ForceLocalPlayInit teardown.  Used to gate NoteTeardownComplete so that a
    // cancel SKIPPED for a transient reason (recovery-suppressed, shutdown,
    // already-recovered) does NOT mark the epoch torn down - otherwise a later
    // cancel of the same boundary would falsely see a repeat and skip the real
    // teardown that never happened.
    bool ranTeardown = false;
    if (!isShutdown && !suppressSharedRecoveryTeardown && !alreadyRecoveredLocal102j
        && !repeatTeardown)
    {
        ranTeardown = true;
        // If we're inside the per-frame tick (sub_1006E570 -> vtable[2] ->
        // RollbackLoopTick), ForceLocalPlayInit MUST NOT run now because it
        // would destroy the session that RollbackLoopTick is actively using
        // as 'this' (use-after-free -> crash in SetEvent(this[2])).
        // Defer the cleanup to OurPerFrameTickHook which will execute it
        // after the original sub_1006E570 returns safely.
        if (IsInsideFrameTick())
        {
            mod::Log(
                "Takeover: cancel cleanup - DEFERRED (inside frame tick, "
                "ForceLocalPlayInit would destroy active session)");
            RequestDeferredCancelCleanup(reason);
            LogRevival102jDeepSnapshot("CancelSession.20.local_reinit_deferred", ioStatus);
        }
        else
        {
            LogRevival102jDeepSnapshot("CancelSession.21.local_reinit_pre", ioStatus);
            const bool initOk = ForceLocalPlayInit();
            mod::Log(
                "Takeover: cancel cleanup - ForceLocalPlayInit result=%d",
                initOk ? 1 : 0);

            const bool clearOk = ClearRevivalText();
            mod::Log(
                "Takeover: cancel cleanup - ClearRevivalText result=%d",
                clearOk ? 1 : 0);

            const bool textOk = ResetRevivalTextRenderingAfterCleanup(
                "cancel_cleanup");
            mod::Log(
                "Takeover: cancel cleanup - ResetRevivalTextRenderingAfterCleanup result=%d",
                textOk ? 1 : 0);

            mod::ResetCrashRecoveryState();
            mod::Log("Takeover: cancel cleanup - crash recovery state reset");

            ResetGameModeValidation();
            mod::Log("Takeover: cancel cleanup - game mode validation reset");
            LogRevival102jDeepSnapshot("CancelSession.22.local_reinit_post", ioStatus);
        }
    }
    else
    {
        mod::Log(
            "Takeover: cancel cleanup - skipped DLL re-init "
            "(reason='%s' shutdown=%d suppressShared=%d alreadyRecoveredLocal102j=%d repeat=%d)",
            reason != nullptr ? reason : "",
            isShutdown ? 1 : 0,
            suppressSharedRecoveryTeardown ? 1 : 0,
            alreadyRecoveredLocal102j ? 1 : 0,
            repeatTeardown ? 1 : 0);
        // Still reset the netplay role even on shutdown so stale state
        // doesn't leak to a future session (belt-and-suspenders).
        g_netplayRole = kNetplayRoleNone;
    }
    // ---- End additional cleanup ------------------------------------------------

    // Mark this session boundary's teardown as run so a repeat cancel within
    // the same boundary (the normal match_ended -> no_overlay_session_ended
    // double-cancel) is recognized above and skips the non-idempotent
    // ForceLocalPlayInit.  Only mark when this call actually ran/deferred the
    // teardown (ranTeardown) - a transiently-skipped cancel must not claim the
    // boundary.  Idempotent; the terminal phase block below still runs on every
    // call so the SessionEnded -> Idle transition is unaffected.
    if (boundaryEpoch != 0 && ranTeardown)
    {
        netplay::bridge::session_lifecycle::NoteTeardownComplete(boundaryEpoch);
    }
    MOD_LIFECYCLE_TRACE(
        "LIFECYCLE ep=%u cleanup=%u stage=cancel_session action=%s reason=%s",
        boundaryEpoch, cleanupInvocation,
        repeatTeardown ? "repeat_complete" : "complete",
        (reason != nullptr) ? reason : "");

    // Clear per-session IAT hook tracking vectors to prevent unbounded
    // growth across sessions (GAP 4 in cleanup audit).
    {
        std::lock_guard<std::mutex> ftLock(g_fakeThreadMutex);
        const size_t oldFakeCount = g_fakeThreads.size();
        // Unlock before calling ClearFakeThreads which takes the same lock,
        // so log the count first then clear outside the lock.
        mod::Log("Takeover: cancel cleanup - clearing g_fakeThreads (count=%zu)",
                 oldFakeCount);
    }
    ClearFakeThreads();
    {
        std::lock_guard<std::mutex> raLock(g_redirectAllocMutex);
        mod::Log("Takeover: cancel cleanup - clearing g_redirectAllocations (count=%zu)",
                 g_redirectAllocations.size());
    }
    ClearRedirectAllocations();

    g_localInitAppliedForSession = false;
    g_spectatorPostInitAttemptedForSession = false;
    g_spectatorPostInitSucceededForSession = false;
    InterlockedExchange(&g_deferredLifecycleWorkRequested, 0);
    InterlockedExchange(&g_deferredTitleSelection, -1);
    InterlockedExchange(&g_injectedDelayPromptSerial, 0);
    InterlockedExchange(&g_injectedDelayPromptServedSerial, 0);
    InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, 0);
    InterlockedExchange(&g_injectedSpectateConfirmPromptSerial, 0);
    InterlockedExchange(&g_injectedSpectateConfirmPromptServedSerial, 0);
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
    g_delayPromptMetrics = {};

    // Clear stale serials in the shared memory block so that
    // ReadDelayPromptSignal / ReadSpectateConfirmPromptSignal won't
    // re-read the old session's values after the next RefreshRuntimeStatus.
    if (g_hostBlock != nullptr)
    {
        InterlockedExchange(&g_hostBlock->delayPromptSerial, 0);
        InterlockedExchange(&g_hostBlock->delayPromptServedSerial, 0);
        InterlockedExchange(&g_hostBlock->delayMetricsSerial, 0);
        InterlockedExchange(&g_hostBlock->delayInputSerial, 0);
        InterlockedExchange(&g_hostBlock->delayInputServedSerial, 0);
        InterlockedExchange(&g_hostBlock->isHostSession, 0);
        InterlockedExchange(
            &g_hostBlock->consoleControlWakeRequestSerial,
            0);
        InterlockedExchange(
            &g_hostBlock->consoleControlWakeServedSerial,
            0);
        InterlockedExchange(
            &g_hostBlock->delayPromptTransitionWakeSerial,
            0);
        InterlockedExchange(
            &g_hostBlock->nativeDelayTimeoutSerial,
            0);
        InterlockedExchange(
            &g_hostBlock->nativeDelayTimeoutRequiredWakeSerial,
            0);
        InterlockedExchange(
            &g_hostBlock->nativeDelayTimeoutHandledSerial,
            0);
        g_hostBlock->delayInputValue = -1;
        g_hostBlock->delayAveragePingMs = -1;
        g_hostBlock->delayMinPingMs = -1;
        g_hostBlock->delayMaxPingMs = -1;
        g_hostBlock->delayRecommended = -1;
        g_hostBlock->delayRangeMin = 0;
        g_hostBlock->delayRangeMax = 20;
        InterlockedExchange(&g_hostBlock->spectateConfirmPromptSerial, 0);
        InterlockedExchange(&g_hostBlock->spectateConfirmPromptServedSerial, 0);
        g_hostBlock->spectateConfirmPromptKind = 0;
        InterlockedExchange(&g_hostBlock->spectateConfirmInputSerial, 0);
        InterlockedExchange(&g_hostBlock->spectateConfirmInputServedSerial, 0);
        g_hostBlock->spectateConfirmInputValue = 0;
        InterlockedExchange(&g_hostBlock->consoleErrorSerial, 0);
        g_hostBlock->consoleErrorText[0] = '\0';
    }
    LogRevival102jDeepStep("CancelSession.30.per_session_state_cleared", ioStatus);
    ResetNativeWorkflowFlags();
    g_lastConnectingDiagnosticTick = 0;
    g_lastSpectateConsoleSnapshotTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    {
        std::lock_guard<std::mutex> consoleLock(g_consoleLogMutex);
        g_consolePendingWriteFile.clear();
        g_consolePendingWriteFileDisk.clear();
        g_consolePendingWriteConsoleA.clear();
        g_consolePendingWriteConsoleW.clear();
        g_consolePendingWriteConsoleOutputCharacterA.clear();
        g_consolePendingWriteConsoleOutputCharacterW.clear();
        g_consolePendingOutputDebugStringA.clear();
        g_consolePendingOutputDebugStringW.clear();
    }
    CloseMirrorLogFiles();

    if (ioStatus != nullptr)
    {
        if (ShouldCancelToIdle(reason))
        {
            SetPhase(ioStatus, NetbridgePhase::Idle, nullptr);
            mod::Log(
                "Takeover: cancel acknowledged -> Idle reason='%s' hadProcess=%d",
                reason != nullptr ? reason : "",
                hadProcess ? 1 : 0);
        }
        else if (hadProcess)
        {
            SetPhase(ioStatus, NetbridgePhase::SessionEnded, nullptr);
        }
        else
        {
            SetPhase(ioStatus, NetbridgePhase::Idle, nullptr);
        }
        RefreshRuntimeStatus(ioStatus);
    }

    mod::Log("Takeover: cancel reason='%s' hadProcess=%d", (reason != nullptr) ? reason : "", hadProcess ? 1 : 0);

    // --- Diagnostic dump after teardown (2nd-session crash investigation) ---
    LogSessionDiagnosticState("CancelSession_exit");
    LogRevival102jDeepSnapshot("CancelSession.99.exit", ioStatus);
}

void CancelSession(const char* reason, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    CancelSessionUnlocked(reason, ioStatus);
}

bool ConsumeRevivalExitInterception(int* outMode, NetbridgeStatus* ioStatus)
{
    // Fast unlocked pre-check avoids the mutex on every frame.
    if (InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0) == 0)
    {
        return false;
    }

    if (netplay::bridge::recovery::ShouldSuppressLegacyGameplayExitCleanup())
    {
        const LONG clearedIntercepted =
            InterlockedExchange(&g_revivalExitIntercepted, 0);
        const LONG clearedMode =
            InterlockedExchange(&g_revivalExitMode, -1);
        mod::Log(
            "GAMEPLAY_EXIT_SUPPRESS_OLD_EXIT_INTERCEPTION reason=frontend_return_owner exitIntercepted=%ld exitMode=%ld state=%s",
            static_cast<long>(clearedIntercepted),
            static_cast<long>(clearedMode),
            netplay::bridge::recovery::CurrentGameplayExitRecoveryStateName());
        return false;
    }

    LogSessionDiagnosticState("ConsumeExitInterception_entry");
    LogRevival102jDeepSnapshot("ExitInterception.01.entry", ioStatus);

    std::lock_guard<std::mutex> lock(g_mutex);

    // Double-check under lock and atomically clear the flag.
    if (InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 1) != 1)
    {
        return false;
    }

    const int mode = static_cast<int>(InterlockedExchange(&g_revivalExitMode, -1));
    if (outMode != nullptr)
    {
        *outMode = mode;
    }

    // ---- Reverse-init teardown sequence ------------------------------------
    // This mirrors the forward init steps in reverse order:
    //
    //   FORWARD INIT                        REVERSE EXIT (this function)
    //   ──────────────────────────────────  ────────────────────────────────────
    //   a) EfzRevival.exe spawned        →  step 1: TerminateProcess
    //   b) DLL ExitProcess Jcc patched   →  step 1: RestoreDllExitProcessPatches
    //   c) EXE patches (tournament only) →  step 2: RestoreTournamentExePatches
    //   d) Online session object created →  step 3: ForceLocalPlayInit (new local)
    //   e) Text rendering active         →  step 4: ClearRevivalText /
    //                                               version-aware renderer reset
    //                                               (tournament: both;
    //                                                online/spectate: Disable only)
    // -----------------------------------------------------------------------

    mod::Log(
        "Takeover: consuming exit interception mode=%d "
        "(step 1: cancel session / terminate peer / restore DLL patches)",
        mode);
    CancelSessionUnlocked("exit_intercepted", ioStatus);
    LogRevival102jDeepSnapshot("ExitInterception.10.cancel_complete", ioStatus);

    // step 2: revert EXE patches applied by the session constructor.
    // Tournament: 4 inline EXE hooks (0x763F04, 0x763E50, 0x754C1A, 0x7599ED).
    // Online/spectate: no additional EXE patches (sub_1006E590 re-applies its
    // frame-by-frame patches every tick and they are self-healing after step 3).
    if (mode == kLocalRoleTournament)
    {
        mod::Log("Takeover: exit interception step 2 - RestoreTournamentExePatches");
        RestoreTournamentExePatches();
        LogRevival102jDeepSnapshot("ExitInterception.20.tournament_patches_restored", ioStatus);
    }
    else
    {
        mod::Log(
            "Takeover: exit interception step 2 - no EXE patch restore "
            "needed for mode=%d (online/spectate)",
            mode);
    }

    // step 3: reinstate a live local-play session.
    // Both OurFrameDispatch (frame-hook longjmp recovery) and
    // CancelSessionUnlocked (step 1 above) now call ForceLocalPlayInit
    // eagerly.  This third call is a defence-in-depth guarantee for the
    // older MSVC-built DLLs where repeated calls are harmless.  On 1.02j,
    // avoid re-running init once recovery has already produced a local
    // session because the MinGW object lifecycle is not ABI-compatible with
    // our old scalar-deleting destructor path.
    const bool alreadyRecoveredLocal102j =
        IsActiveRevival102jProfile()
        && g_localRoleFlag == kLocalRoleLocalPlay
        && GetForceLocalPlayInitCount() > 0
        && ReadSessionPointerFromRevival() != 0;
    if (alreadyRecoveredLocal102j)
    {
        mod::Log(
            "Takeover: exit interception step 3 - skipped ForceLocalPlayInit "
            "(1.02j already has recovered local session, count=%d)",
            GetForceLocalPlayInitCount());
    }
    else
    {
        mod::Log("Takeover: exit interception step 3 - ForceLocalPlayInit (defence-in-depth)");
        ForceLocalPlayInit();
        LogRevival102jDeepSnapshot("ExitInterception.30.defensive_local_init_complete", ioStatus);
    }

    // step 4: clear any DLL-side text overlay state left by the session.
    // Tournament writes win counters and nicknames to the EfzRender text
    // buffer.  init(2,102) zeroes dword_100A0778 so we must RestoreRenderContext
    // before the clear; ClearRevivalText does this internally.
    //
    // 1.02j keeps the renderer usable after cleanup; older builds retain the
    // historical defensive disable.
    mod::Log("Takeover: exit interception step 4 - clear text / reset renderer");
    // Both tournament and online/spectate paths share the same cleanup now.
    // SaveRenderContext() is called in StartSession for all session types,
    // so RestoreRenderContext inside ClearRevivalText works for all modes.
    ClearRevivalText();
    ResetRevivalTextRenderingAfterCleanup("exit_interception");

    mod::Log("Takeover: exit interception fully consumed mode=%d", mode);
    LogSessionDiagnosticState("ConsumeExitInterception_exit");
    LogRevival102jDeepSnapshot("ExitInterception.99.complete", ioStatus);
    return true;
}

DelayPromptMetrics GetDelayPromptMetrics()
{
    std::lock_guard<std::mutex> lock(g_mutex);

    DelayPromptMetrics metrics = g_delayPromptMetrics;
    if (g_hostBlock != nullptr)
    {
        const LONG metricsSerial = InterlockedCompareExchange(&g_hostBlock->delayMetricsSerial, 0, 0);
        if (metricsSerial > 0)
        {
            metrics.serial = static_cast<int>(metricsSerial);
            metrics.averagePingMs = g_hostBlock->delayAveragePingMs;
            metrics.minPingMs = g_hostBlock->delayMinPingMs;
            metrics.maxPingMs = g_hostBlock->delayMaxPingMs;
            metrics.recommendedDelay = g_hostBlock->delayRecommended;
            metrics.minDelay = g_hostBlock->delayRangeMin;
            metrics.maxDelay = g_hostBlock->delayRangeMax;
        }
        const LONG inputSerial = InterlockedCompareExchange(&g_hostBlock->delayInputSerial, 0, 0);
        metrics.inputSerial = static_cast<int>(inputSerial);
        metrics.inputValue = g_hostBlock->delayInputValue;
    }
    return metrics;
}

void ArmTournamentReturnCleanup(const char* reason)
{
    InterlockedExchange(&g_tournamentReturnCleanupStage, 0);
    InterlockedExchange(&g_tournamentReturnCleanupTerminalFailure, 0);
    g_tournamentReturnCleanupPending = true;
    InterlockedExchange(&g_deferredLifecycleWorkRequested, 1);
    mod::Log(
        "Takeover: tournament return cleanup armed reason=%s",
        reason != nullptr && reason[0] != '\0' ? reason : "unspecified");
}

bool NotifyTitleScreenActive(NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_localRoleFlag != kLocalRoleTournament)
    {
        return false;
    }

    if (g_launchDisposition
            == revival_launch::LaunchDisposition::AttachExistingTournament
        && InterlockedCompareExchange(
               &g_externalTournamentInitialTitleLeft, 0, 0) == 0)
    {
        // Launcher option 6 begins on game mode 0 while its queued inputs are
        // still driving the initial title selection. That is startup, not a
        // completed Tournament return.
        return false;
    }

    // The Jcc patches prevent ExitProcess from firing, so NeutralizeExitProcess
    // (and ConsumeRevivalExitInterception) never triggers on the normal path when
    // the tournament match ends and the game returns to mode 0 (title screen).
    // Poll the game mode directly: if we're still flagged as tournament but the
    // game is back on the title screen, run the same cleanup chain proactively.
    int gameMode = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(g_activeRevival->addrGameModeCurrentIndex), &gameMode)
        || gameMode != 0)
    {
        return false;
    }

    mod::Log("Takeover: tournament returned to title screen (mode 0) - cleaning up proactively");
    LogRevival102jDeepSnapshot("TournamentReturn.01.title_observed", ioStatus);

    if (g_tournamentReturnCleanupPending)
    {
        mod::Log(
            "Takeover: tournament return cleanup already pending");
        return false;
    }

    // This callback runs inside the role-3 virtual tick on every supported
    // version. Never destroy that object or restore its executing code here;
    // arm stage 2 and let the post-tick owner perform all destructive work.
    ArmTournamentReturnCleanup("native_title_return");
    mod::Log(
        "Takeover: tournament return cleanup stage 1 complete "
        "(all destructive work deferred post-tick)");
    RefreshRuntimeStatus(ioStatus);
    LogRevival102jDeepSnapshot("TournamentReturn.02.stage1_armed", ioStatus);
    return true;
}

bool CompletePendingTournamentReturnCleanup(NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!g_tournamentReturnCleanupPending)
    {
        return false;
    }

    if (IsInsideFrameTick())
    {
        InterlockedExchange(&g_deferredLifecycleWorkRequested, 1);
        mod::Log(
            "Takeover: tournament return cleanup remains deferred "
            "until the active role-3 tick returns");
        LogRevival102jDeepSnapshot("TournamentReturn.10.stage2_still_deferred", ioStatus);
        return false;
    }

    if (InterlockedCompareExchange(
            &g_tournamentReturnCleanupTerminalFailure, 0, 0) != 0)
    {
        // A prior attempt crossed the object-destruction boundary but could
        // not construct a verified local session.  Repeating init blindly
        // would compound an unknown partial state.  Keep the fatal tick latch
        // and journals owned for diagnosis/recovery instead.
        return false;
    }

    mod::Log("Takeover: tournament return cleanup stage 2 begin");
    LogRevival102jDeepSnapshot("TournamentReturn.11.stage2_pre", ioStatus);
    const auto retainTournamentQuarantine = [&]() {
        InterlockedExchange(
            &g_revivalExitMode,
            static_cast<LONG>(kLocalRoleTournament));
        InterlockedExchange(&g_revivalExitIntercepted, 1);
        InterlockedExchange(&g_deferredLifecycleWorkRequested, 1);
    };

    LONG stage = InterlockedCompareExchange(
        &g_tournamentReturnCleanupStage, 0, 0);
    if (stage < 1)
    {
        const bool exePatchOk = RestoreTournamentExePatches();
        mod::Log(
            "Takeover: tournament return cleanup RestoreTournamentExePatches=%d",
            exePatchOk ? 1 : 0);
        if (!exePatchOk)
        {
            retainTournamentQuarantine();
            return false;
        }
        InterlockedExchange(&g_tournamentReturnCleanupStage, 1);
        stage = 1;
    }

    if (stage < 2)
    {
        const bool initOk = ForceLocalPlayInit();
        mod::Log(
            "Takeover: tournament return cleanup ForceLocalPlayInit=%d",
            initOk ? 1 : 0);
        if (!initOk)
        {
            InterlockedExchange(
                &g_tournamentReturnCleanupTerminalFailure, 1);
            retainTournamentQuarantine();
            mod::Log(
                "Takeover: tournament cleanup quarantined after unverified local init; refusing repeated destruction/init");
            return false;
        }
        InterlockedExchange(&g_tournamentReturnCleanupStage, 2);
        stage = 2;
        LogRevival102jDeepSnapshot(
            "TournamentReturn.12.local_init_complete", ioStatus);
    }

    if (stage < 3)
    {
        const bool dllPatchOk = RestoreDllExitProcessPatches();
        mod::Log(
            "Takeover: tournament return cleanup RestoreDllExitProcessPatches=%d",
            dllPatchOk ? 1 : 0);
        if (!dllPatchOk)
        {
            retainTournamentQuarantine();
            return false;
        }
        InterlockedExchange(&g_tournamentReturnCleanupStage, 3);
    }

    const bool modeOk = ForceGameModeToTitle();
    if (!modeOk)
    {
        mod::Log(
            "Takeover: tournament cleanup could not publish title mode; retaining recovery owner");
        retainTournamentQuarantine();
        return false;
    }

    const bool clearOk = ClearRevivalText();
    const bool textOk = ResetRevivalTextRenderingAfterCleanup(
        "102j_tournament_return_cleanup_stage2");
    mod::Log(
        "Takeover: tournament return cleanup text clear=%d reset=%d",
        clearOk ? 1 : 0,
        textOk ? 1 : 0);

    g_tournamentReturnCleanupPending = false;
    InterlockedExchange(&g_tournamentReturnCleanupStage, 0);
    InterlockedExchange(&g_tournamentReturnCleanupTerminalFailure, 0);
    g_localRoleFlag = kLocalRoleLocalPlay;
    g_localInitAppliedForSession = false;
    g_launchDisposition =
        revival_launch::LaunchDisposition::DirectGameHost;
    InterlockedExchange(&g_externalTournamentInitialTitleLeft, 0);
    InterlockedExchange(&g_revivalExitIntercepted, 0);
    InterlockedExchange(&g_revivalExitMode, -1);
    if (g_hostBlock != nullptr)
    {
        InterlockedExchange(&g_hostBlock->consoleErrorSerial, 0);
        g_hostBlock->consoleErrorText[0] = '\0';
    }
    RefreshRuntimeStatus(ioStatus);
    LogRevival102jDeepSnapshot("TournamentReturn.99.stage2_complete", ioStatus);
    return true;
}

bool IsPeerProcessAlive()
{
    // Advisory: no mutex needed, single-pointer read is atomic on x86.
    // caller must handle the TOCTOU window between this check and acting on it.
    const HANDLE h = g_revivalProcess;
    if (h == nullptr)
    {
        return false;
    }
    DWORD exitCode = STILL_ACTIVE;
    if (GetExitCodeProcess(h, &exitCode) == FALSE)
    {
        return false;
    }
    return exitCode == STILL_ACTIVE;
}

bool IsNetplayExitInterceptionPending()
{
    return InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0) != 0;
}

} // namespace netplay::bridge::takeover


















