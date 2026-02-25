
#include "netplay/bridge/revival_takeover.h"

#include "logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <tlhelp32.h>
#include <windows.h>

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_CreateProcessA(
    LPCSTR lpApplicationName,
    LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadProcessMemory(
    HANDLE hProcess,
    LPCVOID lpBaseAddress,
    LPVOID lpBuffer,
    SIZE_T nSize,
    SIZE_T* lpNumberOfBytesRead);
extern "C" __declspec(dllexport) LPVOID WINAPI nb_stub_VirtualAllocEx(
    HANDLE hProcess,
    LPVOID lpAddress,
    SIZE_T dwSize,
    DWORD flAllocationType,
    DWORD flProtect);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_VirtualFreeEx(
    HANDLE hProcess,
    LPVOID lpAddress,
    SIZE_T dwSize,
    DWORD dwFreeType);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteProcessMemory(
    HANDLE hProcess,
    LPVOID lpBaseAddress,
    LPCVOID lpBuffer,
    SIZE_T nSize,
    SIZE_T* lpNumberOfBytesWritten);
extern "C" __declspec(dllexport) HANDLE WINAPI nb_stub_CreateRemoteThread(
    HANDLE hProcess,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_TerminateProcess(
    HANDLE hProcess,
    UINT uExitCode);
extern "C" __declspec(dllexport) HANDLE WINAPI nb_stub_OpenProcess(
    DWORD dwDesiredAccess,
    BOOL bInheritHandle,
    DWORD dwProcessId);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadConsoleA(
    HANDLE hConsoleInput,
    LPVOID lpBuffer,
    DWORD nNumberOfCharsToRead,
    LPDWORD lpNumberOfCharsRead,
    PCONSOLE_READCONSOLE_CONTROL pInputControl);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadConsoleW(
    HANDLE hConsoleInput,
    LPVOID lpBuffer,
    DWORD nNumberOfCharsToRead,
    LPDWORD lpNumberOfCharsRead,
    PCONSOLE_READCONSOLE_CONTROL pInputControl);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteFile(
    HANDLE hFile,
    LPCVOID lpBuffer,
    DWORD nNumberOfBytesToWrite,
    LPDWORD lpNumberOfBytesWritten,
    LPOVERLAPPED lpOverlapped);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleA(
    HANDLE hConsoleOutput,
    const VOID* lpBuffer,
    DWORD nNumberOfCharsToWrite,
    LPDWORD lpNumberOfCharsWritten,
    LPVOID lpReserved);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleW(
    HANDLE hConsoleOutput,
    const VOID* lpBuffer,
    DWORD nNumberOfCharsToWrite,
    LPDWORD lpNumberOfCharsWritten,
    LPVOID lpReserved);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleOutputCharacterA(
    HANDLE hConsoleOutput,
    LPCSTR lpCharacter,
    DWORD nLength,
    COORD dwWriteCoord,
    LPDWORD lpNumberOfCharsWritten);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleOutputCharacterW(
    HANDLE hConsoleOutput,
    LPCWSTR lpCharacter,
    DWORD nLength,
    COORD dwWriteCoord,
    LPDWORD lpNumberOfCharsWritten);
extern "C" __declspec(dllexport) VOID WINAPI nb_stub_OutputDebugStringA(LPCSTR lpOutputString);
extern "C" __declspec(dllexport) VOID WINAPI nb_stub_OutputDebugStringW(LPCWSTR lpOutputString);
extern "C" __declspec(dllexport) DWORD WINAPI nb_stub_WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode);
extern "C" __declspec(dllexport) DWORD WINAPI nb_stub_ResumeThread(HANDLE hThread);

namespace netplay::bridge::takeover
{
namespace
{
constexpr uint32_t kIpcMagic = 0x4E425247;
constexpr uint32_t kIpcVersion = 1;
constexpr char kSharedBlockName[] = "EFZNetbridge_Shared";
constexpr char kInitReadyEventName[] = "EFZNetbridge_InitReady";
constexpr char kConsoleReadyEventName[] = "EFZNetbridge_ConsoleReady";
constexpr DWORD kStartTimeoutMs = 15000;
constexpr DWORD kLatePatchRetryLogIntervalMs = 3000;
constexpr DWORD kPromptDelayInputWaitTimeoutMs = 30000;
// VerifyEfzVersion expects this exact 4-byte signature at 0x7871F4.
constexpr uint32_t kEfzFingerprint = 0x4386998F;
constexpr uint32_t kDefaultRevivalImageBase = 0x10000000;
constexpr uintptr_t kAddrGameModeStructTable = 0x00790110u;
constexpr uintptr_t kAddrGameModeCurrentIndex = 0x00790148u;
constexpr uintptr_t kRevivalRoleFlagOffsets[] = {0x00A05D0u, 0x00A05F0u, 0x00A15FCu};
constexpr uintptr_t kRevivalSessionPtrOffsets[] = {0x00A02CCu, 0x00A02ECu};
constexpr uintptr_t kRevivalGlobalStatePtrOffset = 0x000A07B8u;
constexpr uintptr_t kRevivalErrorCodeIsZeroRva = 0x000021E0u;
constexpr size_t kRevivalErrorCodeIsZeroPatchSize = 8u;
// RVA of the DLL's "Start init player" function (sub_10072880).
// This function reads the Init shared memory snapshot, assigns the local/remote
// player, seeds 22 initial inputs into the primary buffer, opens a handle to
// EfzRevival.exe, and marks the session as initialized (offset +1220 = 1).
// In vanilla EfzRevival flow this runs automatically via vtable dispatch, but
// our deferred init path bypasses the mechanism that triggers it.
constexpr uintptr_t kRevivalStartInitPlayerRva = 0x00072880u;
constexpr uintptr_t kSessionOffsetInitComplete = 1220u;
constexpr uintptr_t kSessionOffsetInputDelay = 688u;
constexpr uintptr_t kSessionOffsetPingMs = 936u;
constexpr uintptr_t kSessionOffsetHelperHandle = 700u;
constexpr uintptr_t kSessionOffsetHelperPid = 1216u;
constexpr uintptr_t kSessionOffsetActivePlayer = 680u;
constexpr uintptr_t kSessionOffsetQueuePlayer = 684u;
constexpr uintptr_t kSessionOffsetHistoryPrimaryPtr = 824u;   // +0x338
constexpr uintptr_t kSessionOffsetHistorySecondaryPtr = 828u; // +0x33C
constexpr uintptr_t kSessionOffsetHistoryPrimaryVec = 788u;   // +0x314
constexpr uintptr_t kSessionOffsetHistorySecondaryVec = 800u; // +0x320
constexpr uintptr_t kGlobalStateOffsetFlag4964 = 4964u;
constexpr uintptr_t kGlobalStateOffsetFlag4965 = 4965u;
constexpr uintptr_t kGlobalStateOffsetSessionByte = 82563u;
// Revival init() roleFlag semantics (from decompiled EfzRevival.dll):
// 0 = Online session (used for BOTH host and join — the host/join distinction
//     lives in Revival's shared-memory init block, not in roleFlag)
// 1 = Spectator session
// 2 = Local play (offline practice)
// 3 = Offline tournament
constexpr int kLocalRoleOnline = 0;
constexpr int kLocalRoleSpectate = 1;
constexpr int kLocalRoleLocalPlay = 2;
constexpr int kLocalRoleTournament = 3;

#pragma pack(push, 1)
struct SharedBlock
{
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t hostPid = 0;
    uint32_t hostRevivalBase = 0;
    volatile LONG initSerial = 0;
    int initParams[2] = {0, 0};
    volatile LONG consoleSerial = 0;
    char consoleInput[64] = {};
    volatile LONG consoleAuxSerial = 0;
    char consoleInputAux[128] = {};
    volatile LONG delayPromptSerial = 0;
    volatile LONG delayPromptServedSerial = 0;
    volatile LONG delayMetricsSerial = 0;
    int delayAveragePingMs = -1;
    int delayMinPingMs = -1;
    int delayMaxPingMs = -1;
    int delayRecommended = -1;
    int delayRangeMin = 0;
    int delayRangeMax = 20;
    volatile LONG delayInputSerial = 0;
    volatile LONG delayInputServedSerial = 0;
    int delayInputValue = -1;
    volatile LONG dbgReadConsoleHits = 0;
    volatile LONG dbgReadConsoleAutoHits = 0;
    volatile LONG dbgCreateProcessHits = 0;
    volatile LONG dbgWriteProcessHits = 0;
    volatile LONG dbgCreateRemoteThreadHits = 0;
};
#pragma pack(pop)

using RevivalInitFn = int(__cdecl*)(int*);

struct FakeThreadInfo
{
    HANDLE handle = nullptr;
    DWORD exitCode = 0;
};

struct RedirectAllocationInfo
{
    uintptr_t base = 0;
    SIZE_T size = 0;
};

struct RevivalSyncFlags
{
    int gameMode = -1;
    int mode0Flag1084 = -1;
    int sessionByte = -1;
    int globalFlag4964 = -1;
    int globalFlag4965 = -1;
    bool inRollbackSyncState = false;
    bool inRollbackActiveState = false;
};

std::mutex g_mutex;

HMODULE g_localRevivalModule = nullptr;
RevivalInitFn g_localInitFn = nullptr;
HANDLE g_revivalProcess = nullptr;
DWORD g_revivalProcessId = 0;
int g_localRoleFlag = -1;
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
DWORD g_injectedDelayPromptWaitStartTick = 0;
volatile LONG g_injectedFingerprintReadHits = 0;
uintptr_t g_injectedInitAddress = 0;
bool g_injectedLazyBound = false;
volatile LONG g_injectedLazyBootstrapState = 0;
volatile LONG g_remoteThreadCallIndex = 0;
volatile LONG g_startAbortRequested = 0;
HANDLE g_fakeProcessThreadHandle = nullptr;
bool g_initCapturedFromWrite = false;
DWORD g_lastConnectingDiagnosticTick = 0;
uintptr_t g_lastSessionPtrOffset = 0;
uintptr_t g_lastValidatedSessionPtr = 0;
DWORD g_lastSessionPointerMismatchTick = 0;
DWORD g_lastRuntimeReadyProbeLogTick = 0;
uint32_t g_lastRuntimeReadyProbeMask = 0;
bool g_lastRuntimeReadyProbeMaskValid = false;
bool g_localInitAppliedForSession = false;
uintptr_t g_remoteInjectedSelfBase = 0;
DWORD g_lastLatePatchRetryTick = 0;
DWORD g_lastLatePatchRetryLogTick = 0;
DWORD g_latePatchRetryAttempts = 0;
DWORD g_latePatchRetrySuccesses = 0;
bool g_lastLatePatchRetryResultValid = false;
bool g_lastLatePatchRetryResult = false;
bool g_observedTakeoverCreatePath = false;
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
std::unordered_map<std::string, LONG> g_diskCapturePathHits;
bool g_captureRevivalNativeLogsConfigured = false;
bool g_captureRevivalNativeLogs = false;
bool g_revivalErrorCodeNullGuardPatched = false;
uintptr_t g_revivalErrorCodeNullGuardPatchedBase = 0;
void* g_revivalErrorCodeNullGuardStub = nullptr;
DelayPromptMetrics g_delayPromptMetrics = {};
bool g_nativeWorkflowLoadedSeen = false;
bool g_nativeWorkflowMatchLoopSeen = false;
bool g_nativeWorkflowTournamentSeen = false;
bool g_nativeWorkflowPeerDiedSeen = false;
bool g_nativeWorkflowHolePunchDiedSeen = false;
bool g_holePunchServerConfigLoaded = false;
std::string g_configuredHolePunchServer;

uintptr_t ResolveHostRevivalBase();
void PublishHostRevivalBase();
uintptr_t ResolveInjectedExpectedRevivalBase();
HMODULE SelfModule();
void PublishDelayPromptSerial(LONG serial);
void ReadDelayPromptSignal(LONG* outPromptSerial, LONG* outPromptServedSerial);
bool CaptureRevivalNativeLogsEnabled();
bool PatchRevivalErrorCodeNullGuard();

void* EnsureRevivalErrorCodeNullGuardStub()
{
    if (g_revivalErrorCodeNullGuardStub != nullptr)
    {
        return g_revivalErrorCodeNullGuardStub;
    }

    void* stub = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (stub == nullptr)
    {
        mod::Log(
            "Takeover: failed to allocate null-guard stub err=%lu",
            static_cast<unsigned long>(GetLastError()));
        return nullptr;
    }

    // if ((ecx & 0xFFFF0000) == 0) return true; else return (*ecx == 0);
    constexpr std::array<uint8_t, 17> kStubBytes = {
        0xB0, 0x01,                         // mov al, 1
        0xF7, 0xC1, 0x00, 0x00, 0xFF, 0xFF, // test ecx, 0xFFFF0000
        0x74, 0x06,                         // jz +6
        0x83, 0x39, 0x00,                   // cmp dword ptr [ecx], 0
        0x0F, 0x94, 0xC0,                   // sete al
        0xC3,                               // ret
    };

    std::memcpy(stub, kStubBytes.data(), kStubBytes.size());
    (void)FlushInstructionCache(GetCurrentProcess(), stub, kStubBytes.size());
    g_revivalErrorCodeNullGuardStub = stub;
    return stub;
}

void CopyString(char* dst, size_t dstSize, const char* src)
{
    if (dst == nullptr || dstSize == 0)
    {
        return;
    }
    if (src == nullptr)
    {
        dst[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(dst, dstSize, src, _TRUNCATE);
#else
    std::snprintf(dst, dstSize, "%s", src);
#endif
}

bool ExtractConsoleScriptLine(const char* script, LONG* inOutOffset, char* outLine, size_t outLineSize, bool* outHasMore)
{
    if (outLine == nullptr || outLineSize == 0)
    {
        return false;
    }

    outLine[0] = '\0';
    if (outHasMore != nullptr)
    {
        *outHasMore = false;
    }

    if (script == nullptr || script[0] == '\0')
    {
        return false;
    }

    const size_t len = std::strlen(script);
    LONG offset = (inOutOffset != nullptr) ? *inOutOffset : 0;
    if (offset < 0)
    {
        offset = 0;
    }

    size_t start = static_cast<size_t>(offset);
    if (start >= len)
    {
        if (inOutOffset != nullptr)
        {
            *inOutOffset = static_cast<LONG>(len);
        }
        return false;
    }

    size_t end = start;
    while (end < len && script[end] != '\n')
    {
        ++end;
    }
    if (end < len && script[end] == '\n')
    {
        ++end;
    }

    const size_t bytes = end - start;
    const size_t copyBytes = (std::min)(bytes, outLineSize - 1);
    if (copyBytes > 0)
    {
        std::memcpy(outLine, script + start, copyBytes);
    }
    outLine[copyBytes] = '\0';

    if (inOutOffset != nullptr)
    {
        *inOutOffset = static_cast<LONG>(end);
    }
    if (outHasMore != nullptr)
    {
        *outHasMore = (end < len);
    }
    return copyBytes > 0;
}

bool IsLikelyTextChunk(const char* text, size_t length)
{
    if (text == nullptr || length == 0)
    {
        return false;
    }

    size_t printable = 0;
    size_t checked = 0;
    for (size_t i = 0; i < length; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\0')
        {
            break;
        }
        ++checked;
        if ((c >= 0x20 && c <= 0x7E) || c >= 0x80 || c == '\r' || c == '\n' || c == '\t')
        {
            ++printable;
        }
    }
    if (checked == 0)
    {
        return false;
    }
    return printable >= (checked / 2);
}

std::string TrimAscii(const std::string& text)
{
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n'))
    {
        ++start;
    }
    size_t end = text.size();
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }
    return text.substr(start, end - start);
}

std::string ToLowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool TryExtractDiedEndpoint(const std::string& text, std::string* outEndpoint)
{
    if (outEndpoint != nullptr)
    {
        outEndpoint->clear();
    }

    const std::string lowered = ToLowerAscii(text);
    const size_t diedPos = lowered.find(" died");
    if (diedPos == std::string::npos)
    {
        return false;
    }

    const std::string endpoint = TrimAscii(text.substr(0, diedPos));
    if (endpoint.empty())
    {
        return false;
    }

    if (outEndpoint != nullptr)
    {
        *outEndpoint = endpoint;
    }
    return true;
}

std::string LoadConfiguredHolePunchServer()
{
    if (g_holePunchServerConfigLoaded)
    {
        return g_configuredHolePunchServer;
    }

    g_holePunchServerConfigLoaded = true;
    g_configuredHolePunchServer.clear();

    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    {
        return g_configuredHolePunchServer;
    }

    char* slash = std::strrchr(exePath, '\\');
    if (slash == nullptr)
    {
        slash = std::strrchr(exePath, '/');
    }
    if (slash == nullptr)
    {
        return g_configuredHolePunchServer;
    }
    slash[1] = '\0';
    const std::string iniPath = std::string(exePath) + "EfzRevival.ini";

    char serverBuffer[128] = {};
    (void)GetPrivateProfileStringA(
        "Network",
        "HolePunchingServer",
        "",
        serverBuffer,
        static_cast<DWORD>(sizeof(serverBuffer)),
        iniPath.c_str());

    g_configuredHolePunchServer = ToLowerAscii(TrimAscii(serverBuffer));
    if (!g_configuredHolePunchServer.empty())
    {
        mod::Log(
            "Takeover: configured HolePunchingServer='%s'",
            g_configuredHolePunchServer.c_str());
    }
    return g_configuredHolePunchServer;
}

bool ContainsCaseInsensitive(const std::string& text, const char* needle)
{
    if (needle == nullptr || needle[0] == '\0')
    {
        return false;
    }

    const size_t needleLen = std::strlen(needle);
    if (needleLen == 0 || text.size() < needleLen)
    {
        return false;
    }

    for (size_t i = 0; i + needleLen <= text.size(); ++i)
    {
        bool match = true;
        for (size_t j = 0; j < needleLen; ++j)
        {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i + j])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b)
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }

    return false;
}

bool ParseIntAt(const std::string& text, size_t start, int* outValue, size_t* outEnd)
{
    if (outValue == nullptr)
    {
        return false;
    }

    size_t i = start;
    while (i < text.size())
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isdigit(c) || c == '-')
        {
            break;
        }
        ++i;
    }
    if (i >= text.size())
    {
        return false;
    }

    int sign = 1;
    if (text[i] == '-')
    {
        sign = -1;
        ++i;
    }
    if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i])))
    {
        return false;
    }

    int value = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
    {
        value = value * 10 + (text[i] - '0');
        ++i;
    }

    *outValue = value * sign;
    if (outEnd != nullptr)
    {
        *outEnd = i;
    }
    return true;
}

bool ExtractIntAfterToken(const std::string& text, const char* token, int* outValue)
{
    if (token == nullptr || token[0] == '\0' || outValue == nullptr)
    {
        return false;
    }

    const size_t pos = text.find(token);
    if (pos == std::string::npos)
    {
        return false;
    }
    const size_t start = pos + std::strlen(token);
    return ParseIntAt(text, start, outValue, nullptr);
}

bool ExtractDelayRange(const std::string& text, int* outMin, int* outMax)
{
    if (outMin == nullptr || outMax == nullptr)
    {
        return false;
    }

    const size_t betweenPos = text.find("between");
    if (betweenPos == std::string::npos)
    {
        return false;
    }

    size_t afterMin = 0;
    int minValue = 0;
    if (!ParseIntAt(text, betweenPos + 7, &minValue, &afterMin))
    {
        return false;
    }

    const size_t andPos = text.find("and", afterMin);
    if (andPos == std::string::npos)
    {
        return false;
    }

    int maxValue = 0;
    if (!ParseIntAt(text, andPos + 3, &maxValue, nullptr))
    {
        return false;
    }

    *outMin = minValue;
    *outMax = maxValue;
    return true;
}

DelayPromptMetrics ParseDelayPromptMetricsFromText(const std::string& text, bool* outHasMetrics)
{
    DelayPromptMetrics metrics = {};
    bool hasMetrics = false;
    metrics.averagePingMs = -1;
    metrics.minPingMs = -1;
    metrics.maxPingMs = -1;
    metrics.recommendedDelay = -1;
    metrics.minDelay = 0;
    metrics.maxDelay = 20;
    metrics.inputSerial = 0;
    metrics.inputValue = -1;

    int value = 0;
    if (ExtractIntAfterToken(text, "Average Ping:", &value))
    {
        metrics.averagePingMs = value;
        hasMetrics = true;
    }
    if (ExtractIntAfterToken(text, "Min Ping:", &value))
    {
        metrics.minPingMs = value;
        hasMetrics = true;
    }
    if (ExtractIntAfterToken(text, "Max Ping:", &value))
    {
        metrics.maxPingMs = value;
        hasMetrics = true;
    }
    if (ExtractIntAfterToken(text, "Recommended input delay:", &value))
    {
        metrics.recommendedDelay = value;
        hasMetrics = true;
    }

    int minDelay = 0;
    int maxDelay = 20;
    if (ExtractDelayRange(text, &minDelay, &maxDelay))
    {
        metrics.minDelay = minDelay;
        metrics.maxDelay = maxDelay;
        hasMetrics = true;
    }

    if (outHasMetrics != nullptr)
    {
        *outHasMetrics = hasMetrics;
    }
    return metrics;
}

void PublishDelayPromptMetrics(const DelayPromptMetrics& metrics, LONG serial)
{
    g_delayPromptMetrics = metrics;
    g_delayPromptMetrics.serial = static_cast<int>(serial);

    if (g_injectedBlock != nullptr)
    {
        g_injectedBlock->delayAveragePingMs = metrics.averagePingMs;
        g_injectedBlock->delayMinPingMs = metrics.minPingMs;
        g_injectedBlock->delayMaxPingMs = metrics.maxPingMs;
        g_injectedBlock->delayRecommended = metrics.recommendedDelay;
        g_injectedBlock->delayRangeMin = metrics.minDelay;
        g_injectedBlock->delayRangeMax = metrics.maxDelay;
        InterlockedExchange(&g_injectedBlock->delayMetricsSerial, serial);
    }
}

void ResetNativeWorkflowFlags()
{
    g_nativeWorkflowLoadedSeen = false;
    g_nativeWorkflowMatchLoopSeen = false;
    g_nativeWorkflowTournamentSeen = false;
    g_nativeWorkflowPeerDiedSeen = false;
    g_nativeWorkflowHolePunchDiedSeen = false;
    g_holePunchServerConfigLoaded = false;
    g_configuredHolePunchServer.clear();
}

void NoteConsolePromptLine(const std::string& text)
{
    if (text.empty())
    {
        return;
    }

    if (!g_nativeWorkflowLoadedSeen && ContainsCaseInsensitive(text, "Successfully loaded"))
    {
        g_nativeWorkflowLoadedSeen = true;
        mod::Log("Takeover: native workflow event=helper_loaded text='%s'", text.c_str());
    }
    if (!g_nativeWorkflowMatchLoopSeen
        && ContainsCaseInsensitive(text, "CurrentFrame")
        && ContainsCaseInsensitive(text, "Current State"))
    {
        g_nativeWorkflowMatchLoopSeen = true;
        mod::Log("Takeover: native workflow event=match_loop_started text='%s'", text.c_str());
    }
    if (!g_nativeWorkflowTournamentSeen && ContainsCaseInsensitive(text, "Starting Tournament Mode"))
    {
        g_nativeWorkflowTournamentSeen = true;
        mod::Log("Takeover: native workflow event=unexpected_tournament_mode text='%s'", text.c_str());
    }
    std::string diedEndpoint;
    if (TryExtractDiedEndpoint(text, &diedEndpoint))
    {
        const std::string configuredHolePunch = LoadConfiguredHolePunchServer();
        const bool isHolePunchDeath =
            !configuredHolePunch.empty()
            && ToLowerAscii(TrimAscii(diedEndpoint)) == configuredHolePunch;
        if (isHolePunchDeath)
        {
            if (!g_nativeWorkflowHolePunchDiedSeen)
            {
                g_nativeWorkflowHolePunchDiedSeen = true;
                mod::Log(
                    "Takeover: native workflow event=hole_punch_server_died endpoint='%s' text='%s'",
                    diedEndpoint.c_str(),
                    text.c_str());
            }
        }
        else if (!g_nativeWorkflowPeerDiedSeen)
        {
            g_nativeWorkflowPeerDiedSeen = true;
            mod::Log(
                "Takeover: native workflow event=peer_died endpoint='%s' text='%s'",
                diedEndpoint.c_str(),
                text.c_str());
        }
    }

    const bool isDelayPrompt =
        ContainsCaseInsensitive(text, "Enter the initial input delay")
        || ContainsCaseInsensitive(text, "Enter the input delay to use");

    if (!isDelayPrompt)
    {
        return;
    }

    const LONG serial = InterlockedIncrement(&g_injectedDelayPromptSerial);
    PublishDelayPromptSerial(serial);
    g_injectedDelayPromptWaitStartTick = GetTickCount();
    bool hasMetrics = false;
    const DelayPromptMetrics metrics = ParseDelayPromptMetricsFromText(text, &hasMetrics);
    PublishDelayPromptMetrics(metrics, serial);
    if (hasMetrics)
    {
        mod::Log(
            "Takeover: console prompt detected type=delay serial=%ld avg=%d minPing=%d maxPing=%d rec=%d range=%d..%d text='%s'",
            static_cast<long>(serial),
            metrics.averagePingMs,
            metrics.minPingMs,
            metrics.maxPingMs,
            metrics.recommendedDelay,
            metrics.minDelay,
            metrics.maxDelay,
            text.c_str());
        return;
    }
    mod::Log(
        "Takeover: console prompt detected type=delay serial=%ld text='%s'",
        static_cast<long>(serial),
        text.c_str());
}

std::string* SelectPendingConsoleLine(const char* sourceTag)
{
    if (sourceTag == nullptr)
    {
        return nullptr;
    }
    if (std::strcmp(sourceTag, "WriteFile") == 0)
    {
        return &g_consolePendingWriteFile;
    }
    if (std::strcmp(sourceTag, "WriteFileDisk") == 0)
    {
        return &g_consolePendingWriteFileDisk;
    }
    if (std::strcmp(sourceTag, "WriteConsoleA") == 0)
    {
        return &g_consolePendingWriteConsoleA;
    }
    if (std::strcmp(sourceTag, "WriteConsoleW") == 0)
    {
        return &g_consolePendingWriteConsoleW;
    }
    if (std::strcmp(sourceTag, "WriteConsoleOutputCharacterA") == 0)
    {
        return &g_consolePendingWriteConsoleOutputCharacterA;
    }
    if (std::strcmp(sourceTag, "WriteConsoleOutputCharacterW") == 0)
    {
        return &g_consolePendingWriteConsoleOutputCharacterW;
    }
    if (std::strcmp(sourceTag, "OutputDebugStringA") == 0)
    {
        return &g_consolePendingOutputDebugStringA;
    }
    if (std::strcmp(sourceTag, "OutputDebugStringW") == 0)
    {
        return &g_consolePendingOutputDebugStringW;
    }
    return nullptr;
}

bool IsLikelyRevivalDiskLogPath(const std::string& path)
{
    if (path.empty())
    {
        return false;
    }

    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    auto contains = [&](const char* needle) -> bool {
        return lower.find(needle) != std::string::npos;
    };

    const bool looksTextFile =
        contains(".log")
        || contains(".txt")
        || contains(".ini");
    const bool looksRevivalOwned =
        contains("efzrevival")
        || contains("revival")
        || contains("protobuf");

    return looksRevivalOwned || looksTextFile;
}

void LogConsoleTextChunk(const char* sourceTag, const char* text, size_t length)
{
    if (sourceTag == nullptr)
    {
        sourceTag = "unknown";
    }
    if (text == nullptr || length == 0)
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
    {
        return;
    }
    if (!IsLikelyTextChunk(text, length))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_consoleLogMutex);
    std::string localLine;
    std::string* line = SelectPendingConsoleLine(sourceTag);
    if (line == nullptr)
    {
        line = &localLine;
    }
    if (line->capacity() < 256)
    {
        line->reserve(256);
    }

    auto flushLine = [&](bool partial) {
        const std::string trimmed = TrimAscii(*line);
        line->clear();
        if (trimmed.empty())
        {
            return;
        }
        NoteConsolePromptLine(trimmed);
        const LONG count = InterlockedIncrement(&g_injectedConsoleOutputHits);
        mod::Log(
            "Takeover: console[%s] #%ld%s %s",
            sourceTag,
            static_cast<long>(count),
            partial ? " partial" : "",
            trimmed.c_str());
    };

    for (size_t i = 0; i < length; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\0')
        {
            break;
        }
        if (c == '\r' || c == '\n')
        {
            flushLine(false);
            continue;
        }
        if (c == '\t')
        {
            line->push_back(' ');
        }
        else if ((c >= 0x20 && c <= 0x7E) || c >= 0x80)
        {
            line->push_back(static_cast<char>(c));
        }
        else
        {
            line->push_back(' ');
        }

        if (line->size() >= 240)
        {
            flushLine(true);
        }
    }

    // For unknown/untracked sources, don't hold partial fragments indefinitely.
    if (SelectPendingConsoleLine(sourceTag) == nullptr && !line->empty())
    {
        flushLine(true);
    }
}

void FlushPendingConsoleOutput(const char* reason)
{
    if (!CaptureRevivalNativeLogsEnabled())
    {
        std::lock_guard<std::mutex> lock(g_consoleLogMutex);
        g_consolePendingWriteFile.clear();
        g_consolePendingWriteFileDisk.clear();
        g_consolePendingWriteConsoleA.clear();
        g_consolePendingWriteConsoleW.clear();
        g_consolePendingWriteConsoleOutputCharacterA.clear();
        g_consolePendingWriteConsoleOutputCharacterW.clear();
        g_consolePendingOutputDebugStringA.clear();
        g_consolePendingOutputDebugStringW.clear();
        return;
    }

    std::lock_guard<std::mutex> lock(g_consoleLogMutex);
    auto flushOne = [&](const char* sourceTag, std::string* line) {
        if (line == nullptr || line->empty())
        {
            return;
        }

        const std::string trimmed = TrimAscii(*line);
        line->clear();
        if (trimmed.empty())
        {
            return;
        }

        NoteConsolePromptLine(trimmed);
        const LONG count = InterlockedIncrement(&g_injectedConsoleOutputHits);
        mod::Log(
            "Takeover: console[%s] #%ld partial(%s) %s",
            sourceTag,
            static_cast<long>(count),
            reason != nullptr ? reason : "flush",
            trimmed.c_str());
    };

    flushOne("WriteFile", &g_consolePendingWriteFile);
    flushOne("WriteFileDisk", &g_consolePendingWriteFileDisk);
    flushOne("WriteConsoleA", &g_consolePendingWriteConsoleA);
    flushOne("WriteConsoleW", &g_consolePendingWriteConsoleW);
    flushOne("WriteConsoleOutputCharacterA", &g_consolePendingWriteConsoleOutputCharacterA);
    flushOne("WriteConsoleOutputCharacterW", &g_consolePendingWriteConsoleOutputCharacterW);
    flushOne("OutputDebugStringA", &g_consolePendingOutputDebugStringA);
    flushOne("OutputDebugStringW", &g_consolePendingOutputDebugStringW);
}

void MaybeLogConsoleOutputChunk(HANDLE hFile, LPCVOID lpBuffer, DWORD nBytes)
{
    if (lpBuffer == nullptr || nBytes == 0)
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
    {
        return;
    }

    const char* text = reinterpret_cast<const char*>(lpBuffer);
    const size_t textLen = static_cast<size_t>(nBytes);
    if (!IsLikelyTextChunk(text, textLen))
    {
        return;
    }

    // Console output can flow through redirected handles (pipes/unknown).
    // Keep console/pipe traffic visible, and selectively forward disk-backed
    // Revival text logs without enabling noisy binary file dumps.
    SetLastError(NO_ERROR);
    const DWORD fileType = GetFileType(hFile);
    if (fileType == FILE_TYPE_DISK)
    {
        char path[1024] = {};
        const DWORD pathLen = GetFinalPathNameByHandleA(
            hFile,
            path,
            static_cast<DWORD>(sizeof(path)),
            FILE_NAME_NORMALIZED);
        if (pathLen == 0 || pathLen >= sizeof(path))
        {
            return;
        }

        const std::string pathText(path, pathLen);
        if (!IsLikelyRevivalDiskLogPath(pathText))
        {
            return;
        }

        bool announcePath = false;
        LONG pathHitCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_consoleLogMutex);
            LONG& hitRef = g_diskCapturePathHits[pathText];
            ++hitRef;
            pathHitCount = hitRef;
            announcePath = (hitRef == 1);
        }
        if (announcePath)
        {
            mod::Log(
                "Takeover: disk text capture enabled path='%s'",
                pathText.c_str());
        }
        else if ((pathHitCount % 128) == 0)
        {
            mod::Log(
                "Takeover: disk text capture path='%s' chunks=%ld",
                pathText.c_str(),
                static_cast<long>(pathHitCount));
        }

        LogConsoleTextChunk("WriteFileDisk", text, textLen);
        return;
    }
    if (fileType == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR)
    {
        return;
    }

    LogConsoleTextChunk("WriteFile", text, textLen);
}

void MaybeLogConsoleWriteAChunk(const VOID* lpBuffer, DWORD nChars)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
    {
        return;
    }

    LogConsoleTextChunk("WriteConsoleA", reinterpret_cast<const char*>(lpBuffer), static_cast<size_t>(nChars));
}

void MaybeLogConsoleWriteWChunk(const VOID* lpBuffer, DWORD nChars)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
    {
        return;
    }

    const wchar_t* wideText = reinterpret_cast<const wchar_t*>(lpBuffer);
    const int wideLen = (std::min)(static_cast<int>(nChars), 0x4000);
    if (wideLen <= 0)
    {
        return;
    }

    int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, wideText, wideLen, nullptr, 0, nullptr, nullptr);
    UINT codePage = CP_UTF8;
    if (utf8Bytes <= 0)
    {
        codePage = CP_ACP;
        utf8Bytes = WideCharToMultiByte(codePage, 0, wideText, wideLen, nullptr, 0, nullptr, nullptr);
    }
    if (utf8Bytes <= 0)
    {
        return;
    }

    std::string utf8;
    utf8.resize(static_cast<size_t>(utf8Bytes));
    if (WideCharToMultiByte(codePage, 0, wideText, wideLen, utf8.data(), utf8Bytes, nullptr, nullptr) <= 0)
    {
        return;
    }

    LogConsoleTextChunk("WriteConsoleW", utf8.c_str(), utf8.size());
}

void MaybeLogConsoleOutputCharacterAChunk(const VOID* lpBuffer, DWORD nChars)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
    {
        return;
    }

    LogConsoleTextChunk("WriteConsoleOutputCharacterA", reinterpret_cast<const char*>(lpBuffer), static_cast<size_t>(nChars));
}

void MaybeLogConsoleOutputCharacterWChunk(const VOID* lpBuffer, DWORD nChars)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
    {
        return;
    }

    const wchar_t* wideText = reinterpret_cast<const wchar_t*>(lpBuffer);
    const int wideLen = (std::min)(static_cast<int>(nChars), 0x4000);
    if (wideLen <= 0)
    {
        return;
    }

    int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, wideText, wideLen, nullptr, 0, nullptr, nullptr);
    UINT codePage = CP_UTF8;
    if (utf8Bytes <= 0)
    {
        codePage = CP_ACP;
        utf8Bytes = WideCharToMultiByte(codePage, 0, wideText, wideLen, nullptr, 0, nullptr, nullptr);
    }
    if (utf8Bytes <= 0)
    {
        return;
    }

    std::string utf8;
    utf8.resize(static_cast<size_t>(utf8Bytes));
    if (WideCharToMultiByte(codePage, 0, wideText, wideLen, utf8.data(), utf8Bytes, nullptr, nullptr) <= 0)
    {
        return;
    }

    LogConsoleTextChunk("WriteConsoleOutputCharacterW", utf8.c_str(), utf8.size());
}

void MaybeLogOutputDebugStringA(LPCSTR lpOutputString)
{
    if (lpOutputString == nullptr || lpOutputString[0] == '\0')
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
    {
        return;
    }

    const size_t len = std::strlen(lpOutputString);
    if (len == 0)
    {
        return;
    }

    LogConsoleTextChunk("OutputDebugStringA", lpOutputString, len);
}

void MaybeLogOutputDebugStringW(LPCWSTR lpOutputString)
{
    if (lpOutputString == nullptr || lpOutputString[0] == L'\0')
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
    {
        return;
    }

    const int wideLen = static_cast<int>(wcslen(lpOutputString));
    if (wideLen <= 0)
    {
        return;
    }

    int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, lpOutputString, wideLen, nullptr, 0, nullptr, nullptr);
    UINT codePage = CP_UTF8;
    if (utf8Bytes <= 0)
    {
        codePage = CP_ACP;
        utf8Bytes = WideCharToMultiByte(codePage, 0, lpOutputString, wideLen, nullptr, 0, nullptr, nullptr);
    }
    if (utf8Bytes <= 0)
    {
        return;
    }

    std::string utf8;
    utf8.resize(static_cast<size_t>(utf8Bytes));
    if (WideCharToMultiByte(codePage, 0, lpOutputString, wideLen, utf8.data(), utf8Bytes, nullptr, nullptr) <= 0)
    {
        return;
    }

    LogConsoleTextChunk("OutputDebugStringW", utf8.c_str(), utf8.size());
}

std::string ErrorString(DWORD code)
{
    char buffer[256] = {};
    const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageA(flags, nullptr, code, 0, buffer, static_cast<DWORD>(sizeof(buffer)), nullptr);
    if (length == 0)
    {
        std::snprintf(buffer, sizeof(buffer), "win32_error_%lu", static_cast<unsigned long>(code));
    }
    std::string text = buffer;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t'))
    {
        text.pop_back();
    }
    return text;
}

std::string BaseLower(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return base;
}

bool IsReadableRange(const void* address, size_t size)
{
    if (address == nullptr || size == 0)
    {
        return false;
    }

    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = cursor + size;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) == 0)
        {
            return false;
        }

        if (mbi.State != MEM_COMMIT)
        {
            return false;
        }

        const DWORD rawProt = mbi.Protect;
        if ((rawProt & PAGE_GUARD) != 0 || (rawProt & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        const DWORD prot = rawProt & 0xFFu;
        const bool readable =
            prot == PAGE_READONLY ||
            prot == PAGE_READWRITE ||
            prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_READ ||
            prot == PAGE_EXECUTE_READWRITE ||
            prot == PAGE_EXECUTE_WRITECOPY;
        if (!readable)
        {
            return false;
        }

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor)
        {
            return false;
        }
        cursor = regionEnd;
    }

    return true;
}

bool IsWritableRange(void* address, size_t size)
{
    if (address == nullptr || size == 0)
    {
        return false;
    }

    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = cursor + size;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) == 0)
        {
            return false;
        }

        if (mbi.State != MEM_COMMIT)
        {
            return false;
        }

        const DWORD rawProt = mbi.Protect;
        if ((rawProt & PAGE_GUARD) != 0 || (rawProt & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        const DWORD prot = rawProt & 0xFFu;
        const bool writable =
            prot == PAGE_READWRITE ||
            prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_READWRITE ||
            prot == PAGE_EXECUTE_WRITECOPY;
        if (!writable)
        {
            return false;
        }

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor)
        {
            return false;
        }
        cursor = regionEnd;
    }

    return true;
}

bool SafeReadInt(const void* address, int* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(int)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const int*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

bool SafeReadPtr(const void* address, uintptr_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uintptr_t)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const uintptr_t*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

bool SafeReadByte(const void* address, uint8_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uint8_t)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const uint8_t*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

bool ReadModuleImageRange(HMODULE module, uintptr_t* outBase, uintptr_t* outEnd)
{
    if (module == nullptr || outBase == nullptr || outEnd == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos = {};
    if (!IsReadableRange(reinterpret_cast<const void*>(base), sizeof(dos)))
    {
        return false;
    }

    __try
    {
        dos = *reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
    {
        return false;
    }

    const uintptr_t ntAddress = base + static_cast<uintptr_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS32 nt = {};
    if (!IsReadableRange(reinterpret_cast<const void*>(ntAddress), sizeof(nt)))
    {
        return false;
    }

    __try
    {
        nt = *reinterpret_cast<const IMAGE_NT_HEADERS32*>(ntAddress);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt.OptionalHeader.SizeOfImage == 0)
    {
        return false;
    }

    const uintptr_t end = base + static_cast<uintptr_t>(nt.OptionalHeader.SizeOfImage);
    if (end <= base)
    {
        return false;
    }

    *outBase = base;
    *outEnd = end;
    return true;
}

int ReadRoleFlagFromRevival()
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return -1;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    for (uintptr_t offset : kRevivalRoleFlagOffsets)
    {
        int role = -1;
        if (SafeReadInt(reinterpret_cast<const void*>(base + offset), &role)
            && role >= 0 && role <= 3)
        {
            return role;
        }
    }
    return -1;
}

bool IsSessionPointerByVtable(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd)
{
    // Reject null / low-memory sentinels that can appear at fallback offsets.
    if (sessionPtr < 0x00100000u)
    {
        return false;
    }

    // Session objects are C++ classes; first word must be a vtable inside EfzRevival.dll.
    uintptr_t vtable = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable))
    {
        return false;
    }
    if (revivalImageBase == 0 || revivalImageEnd <= revivalImageBase || vtable < revivalImageBase || vtable >= revivalImageEnd)
    {
        return false;
    }

    return true;
}

bool IsLikelySessionPointer(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd)
{
    if (!IsSessionPointerByVtable(sessionPtr, revivalImageBase, revivalImageEnd))
    {
        return false;
    }

    int delayFrames = -1;
    int pingMs = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetInputDelay), &delayFrames))
    {
        return false;
    }
    if (!SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetPingMs), &pingMs))
    {
        return false;
    }

    const bool delayLooksValid = delayFrames >= 0 && delayFrames < 128;
    const bool pingLooksValid = pingMs >= 0 && pingMs < 60000;
    return delayLooksValid || pingLooksValid;
}

uintptr_t ReadSessionPointerFromRevival()
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        g_lastSessionPtrOffset = 0;
        return 0;
    }

    g_lastSessionPtrOffset = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t revivalImageBase = 0;
    uintptr_t revivalImageEnd = 0;
    if (!ReadModuleImageRange(revival, &revivalImageBase, &revivalImageEnd))
    {
        revivalImageBase = base;
        revivalImageEnd = base + 0x02000000u;
    }

    for (uintptr_t offset : kRevivalSessionPtrOffsets)
    {
        uintptr_t sessionPtr = 0;
        if (SafeReadPtr(reinterpret_cast<const void*>(base + offset), &sessionPtr)
            && IsLikelySessionPointer(sessionPtr, revivalImageBase, revivalImageEnd))
        {
            g_lastSessionPtrOffset = offset;
            g_lastValidatedSessionPtr = sessionPtr;
            return sessionPtr;
        }
    }
    return 0;
}

uintptr_t ReadSessionPointerFromRevivalLoose()
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return 0;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t revivalImageBase = 0;
    uintptr_t revivalImageEnd = 0;
    if (!ReadModuleImageRange(revival, &revivalImageBase, &revivalImageEnd))
    {
        revivalImageBase = base;
        revivalImageEnd = base + 0x02000000u;
    }

    for (uintptr_t offset : kRevivalSessionPtrOffsets)
    {
        uintptr_t sessionPtr = 0;
        if (SafeReadPtr(reinterpret_cast<const void*>(base + offset), &sessionPtr)
            && IsSessionPointerByVtable(sessionPtr, revivalImageBase, revivalImageEnd))
        {
            return sessionPtr;
        }
    }

    return 0;
}

uintptr_t ReadSessionPointerForMutation(bool* outUsedCached)
{
    if (outUsedCached != nullptr)
    {
        *outUsedCached = false;
    }

    const uintptr_t strictSessionPtr = ReadSessionPointerFromRevival();
    if (strictSessionPtr != 0)
    {
        return strictSessionPtr;
    }

    const uintptr_t cachedSessionPtr = g_lastValidatedSessionPtr;
    if (cachedSessionPtr == 0)
    {
        return 0;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return 0;
    }

    uintptr_t revivalImageBase = 0;
    uintptr_t revivalImageEnd = 0;
    if (!ReadModuleImageRange(revival, &revivalImageBase, &revivalImageEnd))
    {
        revivalImageBase = reinterpret_cast<uintptr_t>(revival);
        revivalImageEnd = revivalImageBase + 0x02000000u;
    }

    if (!IsSessionPointerByVtable(cachedSessionPtr, revivalImageBase, revivalImageEnd))
    {
        return 0;
    }

    int activePlayer = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(cachedSessionPtr + kSessionOffsetActivePlayer), &activePlayer))
    {
        return 0;
    }
    if (activePlayer != 0 && activePlayer != 1)
    {
        return 0;
    }

    if (outUsedCached != nullptr)
    {
        *outUsedCached = true;
    }
    return cachedSessionPtr;
}

bool ReadRevivalSyncFlags(RevivalSyncFlags* outFlags)
{
    if (outFlags == nullptr)
    {
        return false;
    }

    *outFlags = RevivalSyncFlags{};
    bool hasAny = false;

    int gameMode = -1;
    if (SafeReadInt(reinterpret_cast<const void*>(kAddrGameModeCurrentIndex), &gameMode))
    {
        outFlags->gameMode = gameMode;
        hasAny = true;
    }

    uintptr_t mode0Struct = 0;
    if (SafeReadPtr(reinterpret_cast<const void*>(kAddrGameModeStructTable), &mode0Struct) && mode0Struct != 0)
    {
        uint8_t mode0Flag = 0;
        if (SafeReadByte(reinterpret_cast<const void*>(mode0Struct + 1084u), &mode0Flag))
        {
            outFlags->mode0Flag1084 = static_cast<int>(mode0Flag);
            hasAny = true;
        }
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival != nullptr)
    {
        uintptr_t globalStatePtr = 0;
        const uintptr_t globalStatePtrAddr = reinterpret_cast<uintptr_t>(revival) + kRevivalGlobalStatePtrOffset;
        if (SafeReadPtr(reinterpret_cast<const void*>(globalStatePtrAddr), &globalStatePtr) && globalStatePtr != 0)
        {
            uint8_t sessionByte = 0;
            if (SafeReadByte(reinterpret_cast<const void*>(globalStatePtr + kGlobalStateOffsetSessionByte), &sessionByte))
            {
                outFlags->sessionByte = static_cast<int>(sessionByte);
                hasAny = true;
            }

            uint8_t globalFlag4964 = 0;
            if (SafeReadByte(reinterpret_cast<const void*>(globalStatePtr + kGlobalStateOffsetFlag4964), &globalFlag4964))
            {
                outFlags->globalFlag4964 = static_cast<int>(globalFlag4964);
                hasAny = true;
            }

            uint8_t globalFlag4965 = 0;
            if (SafeReadByte(reinterpret_cast<const void*>(globalStatePtr + kGlobalStateOffsetFlag4965), &globalFlag4965))
            {
                outFlags->globalFlag4965 = static_cast<int>(globalFlag4965);
                hasAny = true;
            }
        }
    }

    const bool sessionLooksRollback = outFlags->sessionByte == 0 || outFlags->sessionByte == 1 || outFlags->sessionByte == 2;
    outFlags->inRollbackSyncState = (outFlags->gameMode == 3 && outFlags->mode0Flag1084 == 4 && sessionLooksRollback);
    outFlags->inRollbackActiveState = (outFlags->gameMode == 3 && outFlags->mode0Flag1084 == 4 && outFlags->sessionByte == 2);
    return hasAny;
}

void RefreshRuntimeStatus(NetbridgeStatus* ioStatus)
{
    if (ioStatus == nullptr)
    {
        return;
    }

    ioStatus->syncGameMode = -1;
    ioStatus->syncMode0Flag1084 = -1;
    ioStatus->syncSessionByte = -1;
    ioStatus->syncGlobalFlag4964 = -1;
    ioStatus->syncGlobalFlag4965 = -1;
    ioStatus->pingMs = -1;
    ioStatus->rollbackFrames = -1;
    ioStatus->delayPromptSerial = 0;
    ioStatus->delayPromptServedSerial = 0;
    ioStatus->localInitApplied = g_localInitAppliedForSession ? 1 : 0;
    ioStatus->delaySetupReady = 0;
    ioStatus->vsHumanSyncReady = 0;
    ioStatus->p1Name[0] = '\0';
    ioStatus->p2Name[0] = '\0';

    LONG delayPromptSerial = 0;
    LONG delayPromptServedSerial = 0;
    ReadDelayPromptSignal(&delayPromptSerial, &delayPromptServedSerial);
    ioStatus->delayPromptSerial = static_cast<int>(delayPromptSerial);
    ioStatus->delayPromptServedSerial = static_cast<int>(delayPromptServedSerial);

    DelayPromptMetrics promptMetrics = g_delayPromptMetrics;
    if (g_hostBlock != nullptr)
    {
        const LONG metricsSerial = InterlockedCompareExchange(&g_hostBlock->delayMetricsSerial, 0, 0);
        if (metricsSerial > 0)
        {
            promptMetrics.serial = static_cast<int>(metricsSerial);
            promptMetrics.averagePingMs = g_hostBlock->delayAveragePingMs;
            promptMetrics.minPingMs = g_hostBlock->delayMinPingMs;
            promptMetrics.maxPingMs = g_hostBlock->delayMaxPingMs;
            promptMetrics.recommendedDelay = g_hostBlock->delayRecommended;
            promptMetrics.minDelay = g_hostBlock->delayRangeMin;
            promptMetrics.maxDelay = g_hostBlock->delayRangeMax;
        }
        const LONG inputSerial = InterlockedCompareExchange(&g_hostBlock->delayInputSerial, 0, 0);
        promptMetrics.inputSerial = static_cast<int>(inputSerial);
        promptMetrics.inputValue = g_hostBlock->delayInputValue;
    }
    g_delayPromptMetrics = promptMetrics;

    RevivalSyncFlags syncFlags = {};
    if (ReadRevivalSyncFlags(&syncFlags))
    {
        ioStatus->syncGameMode = syncFlags.gameMode;
        ioStatus->syncMode0Flag1084 = syncFlags.mode0Flag1084;
        ioStatus->syncSessionByte = syncFlags.sessionByte;
        ioStatus->syncGlobalFlag4964 = syncFlags.globalFlag4964;
        ioStatus->syncGlobalFlag4965 = syncFlags.globalFlag4965;
        ioStatus->vsHumanSyncReady = syncFlags.inRollbackSyncState ? 1 : 0;
    }

    int roleFlag = ReadRoleFlagFromRevival();
    if (roleFlag < 0)
    {
        roleFlag = g_localRoleFlag;
    }
    ioStatus->roleFlag = roleFlag;

    const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
    if (sessionPtr == 0)
    {
        if (promptMetrics.averagePingMs >= 0)
        {
            ioStatus->pingMs = promptMetrics.averagePingMs;
        }
        if (promptMetrics.recommendedDelay >= 0)
        {
            ioStatus->rollbackFrames = promptMetrics.recommendedDelay;
        }
        ioStatus->delaySetupReady =
            (ioStatus->delayPromptSerial > 0 || ioStatus->delayPromptServedSerial > 0)
                ? 1
                : 0;
        return;
    }

    int delayFrames = -1;
    int pingMs = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetInputDelay), &delayFrames);
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetPingMs), &pingMs);

    if (delayFrames >= 0 && delayFrames < 128)
    {
        ioStatus->rollbackFrames = delayFrames;
    }
    if (pingMs >= 0 && pingMs < 60000)
    {
        ioStatus->pingMs = pingMs;
    }
    else if (promptMetrics.averagePingMs >= 0)
    {
        ioStatus->pingMs = promptMetrics.averagePingMs;
    }

    if (ioStatus->rollbackFrames < 0 && promptMetrics.recommendedDelay >= 0)
    {
        ioStatus->rollbackFrames = promptMetrics.recommendedDelay;
    }

    auto tryReadInlineName = [](uintptr_t baseAddress, char* outText, size_t outSize) -> void {
        if (outText == nullptr || outSize == 0)
        {
            return;
        }
        outText[0] = '\0';

        constexpr size_t kMaxChars = 24;
        wchar_t wide[kMaxChars + 1] = {};
        if (!IsReadableRange(reinterpret_cast<const void*>(baseAddress), kMaxChars * sizeof(wchar_t)))
        {
            return;
        }

        __try
        {
            std::memcpy(wide, reinterpret_cast<const void*>(baseAddress), kMaxChars * sizeof(wchar_t));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        wide[kMaxChars] = L'\0';

        size_t n = 0;
        while (n < kMaxChars && wide[n] != L'\0')
        {
            if (wide[n] < 0x20)
            {
                return;
            }
            ++n;
        }
        if (n == 0 || n >= kMaxChars)
        {
            return;
        }

        const int converted = WideCharToMultiByte(CP_ACP, 0, wide, static_cast<int>(n), outText, static_cast<int>(outSize - 1), nullptr, nullptr);
        if (converted > 0)
        {
            outText[converted] = '\0';
        }
    };

    auto sanitizeInlineName = [](char* text, size_t textSize) -> void {
        if (text == nullptr || textSize == 0 || text[0] == '\0')
        {
            return;
        }

        size_t len = 0;
        int questionMarks = 0;
        for (; len + 1 < textSize && text[len] != '\0'; ++len)
        {
            const unsigned char c = static_cast<unsigned char>(text[len]);
            if (c < 0x20 || c == 0x7F)
            {
                text[0] = '\0';
                return;
            }
            if (c == '?')
            {
                ++questionMarks;
            }
        }
        if (len == 0 || len + 1 >= textSize)
        {
            text[0] = '\0';
            return;
        }

        // Reject mojibake-like names (mostly '?' from failed conversion).
        if (questionMarks >= 3 && questionMarks * 2 >= static_cast<int>(len))
        {
            text[0] = '\0';
        }
    };

    const bool allowSessionInlineNames =
        syncFlags.inRollbackSyncState
        || syncFlags.inRollbackActiveState
        || static_cast<NetbridgePhase>(ioStatus->phase) == NetbridgePhase::Connected;

    if (allowSessionInlineNames)
    {
        tryReadInlineName(sessionPtr + 740u, ioStatus->p1Name, sizeof(ioStatus->p1Name));
        tryReadInlineName(sessionPtr + 764u, ioStatus->p2Name, sizeof(ioStatus->p2Name));
        sanitizeInlineName(ioStatus->p1Name, sizeof(ioStatus->p1Name));
        sanitizeInlineName(ioStatus->p2Name, sizeof(ioStatus->p2Name));
    }

    // Only expose player names when we have both sides; showing a single local
    // nickname during delay setup looks misleading.
    if (ioStatus->p1Name[0] == '\0' || ioStatus->p2Name[0] == '\0')
    {
        ioStatus->p1Name[0] = '\0';
        ioStatus->p2Name[0] = '\0';
    }

    ioStatus->delaySetupReady =
        (ioStatus->delayPromptSerial > 0 || ioStatus->delayPromptServedSerial > 0)
            ? 1
            : 0;
}

bool SetLocalRoleFlag(int roleFlag, const char* reason)
{
    if (g_localInitFn == nullptr)
    {
        return false;
    }

    if (roleFlag < 0 || roleFlag > 3)
    {
        return false;
    }

    if (g_localRoleFlag == roleFlag)
    {
        return true;
    }

    int localParams[2] = {roleFlag, 102};
    const int result = g_localInitFn(localParams);
    g_localRoleFlag = roleFlag;
    mod::Log("Takeover: local role switch mode=%d result=%d reason=%s", roleFlag, result, reason != nullptr ? reason : "");
    return true;
}

void ResetDebugCounters(SharedBlock* block)
{
    if (block == nullptr)
    {
        return;
    }
    block->delayPromptSerial = 0;
    block->delayPromptServedSerial = 0;
    block->delayMetricsSerial = 0;
    block->delayAveragePingMs = -1;
    block->delayMinPingMs = -1;
    block->delayMaxPingMs = -1;
    block->delayRecommended = -1;
    block->delayRangeMin = 0;
    block->delayRangeMax = 20;
    block->delayInputSerial = 0;
    block->delayInputServedSerial = 0;
    block->delayInputValue = -1;
    block->dbgReadConsoleHits = 0;
    block->dbgReadConsoleAutoHits = 0;
    block->dbgCreateProcessHits = 0;
    block->dbgWriteProcessHits = 0;
    block->dbgCreateRemoteThreadHits = 0;
}

struct TempIpcContext
{
    HANDLE mapHandle = nullptr;
    SharedBlock* block = nullptr;
    HANDLE initEvent = nullptr;
    HANDLE consoleEvent = nullptr;
};

bool OpenTempIpcContext(TempIpcContext* ctx, bool needInitEvent, bool needConsoleEvent)
{
    if (ctx == nullptr)
    {
        return false;
    }

    ctx->mapHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kSharedBlockName);
    if (ctx->mapHandle == nullptr)
    {
        return false;
    }

    ctx->block = static_cast<SharedBlock*>(MapViewOfFile(ctx->mapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
    if (ctx->block == nullptr)
    {
        CloseHandle(ctx->mapHandle);
        ctx->mapHandle = nullptr;
        return false;
    }

    if (needInitEvent)
    {
        ctx->initEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kInitReadyEventName);
    }
    if (needConsoleEvent)
    {
        ctx->consoleEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kConsoleReadyEventName);
    }

    return true;
}

void CloseTempIpcContext(TempIpcContext* ctx)
{
    if (ctx == nullptr)
    {
        return;
    }

    if (ctx->block != nullptr)
    {
        UnmapViewOfFile(ctx->block);
        ctx->block = nullptr;
    }
    if (ctx->mapHandle != nullptr)
    {
        CloseHandle(ctx->mapHandle);
        ctx->mapHandle = nullptr;
    }
    if (ctx->initEvent != nullptr)
    {
        CloseHandle(ctx->initEvent);
        ctx->initEvent = nullptr;
    }
    if (ctx->consoleEvent != nullptr)
    {
        CloseHandle(ctx->consoleEvent);
        ctx->consoleEvent = nullptr;
    }
}

void PublishDelayPromptSerial(LONG serial)
{
    if (serial <= 0)
    {
        return;
    }

    if (g_injectedBlock != nullptr)
    {
        const LONG current = InterlockedCompareExchange(&g_injectedBlock->delayPromptSerial, 0, 0);
        if (serial > current)
        {
            InterlockedExchange(&g_injectedBlock->delayPromptSerial, serial);
        }
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        const LONG current = InterlockedCompareExchange(&temp.block->delayPromptSerial, 0, 0);
        if (serial > current)
        {
            InterlockedExchange(&temp.block->delayPromptSerial, serial);
        }
    }
    CloseTempIpcContext(&temp);
}

HMODULE SelfModule()
{
    HMODULE module = nullptr;
    (void)GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&SelfModule),
        &module);
    return module;
}

std::string ModulePath(HMODULE module)
{
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return std::string();
    }
    return std::string(path);
}

bool TryReadCaptureRevivalNativeLogsConfig(bool* outEnabled, std::string* outSourceTag)
{
    if (outEnabled == nullptr)
    {
        return false;
    }

    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    {
        return false;
    }
    char* slash = std::strrchr(exePath, '\\');
    if (slash == nullptr)
    {
        slash = std::strrchr(exePath, '/');
    }
    if (slash == nullptr)
    {
        return false;
    }
    slash[1] = '\0';
    std::string iniPath = std::string(exePath) + "EfzRevival.ini";

    const UINT debugValue = GetPrivateProfileIntA("Global", "Debug", 2, iniPath.c_str());
    if (debugValue > 1)
    {
        return false;
    }

    *outEnabled = (debugValue != 0);
    if (outSourceTag != nullptr)
    {
        *outSourceTag = std::string("ini:") + iniPath;
    }
    return true;
}

bool CaptureRevivalNativeLogsEnabled()
{
    if (g_captureRevivalNativeLogsConfigured)
    {
        return g_captureRevivalNativeLogs;
    }

    bool enabled = false;
    std::string sourceTag = "default_off";
    if (TryReadCaptureRevivalNativeLogsConfig(&enabled, &sourceTag))
    {
        g_captureRevivalNativeLogs = enabled;
    }
    else
    {
        g_captureRevivalNativeLogs = enabled;
    }
    g_captureRevivalNativeLogsConfigured = true;

    mod::Log(
        "Takeover: capture revival native logs=%d source=%s",
        g_captureRevivalNativeLogs ? 1 : 0,
        sourceTag.c_str());

    return g_captureRevivalNativeLogs;
}

std::string GameDirectory()
{
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
    {
        return std::string();
    }
    char* slash = std::strrchr(path, '\\');
    if (slash == nullptr)
    {
        return std::string();
    }
    *slash = '\0';
    return std::string(path);
}

bool TryWriteClipboardAscii(const char* text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return false;
    }

    const size_t bytes = std::strlen(text) + 1;
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (OpenClipboard(nullptr) == FALSE)
        {
            Sleep(10);
            continue;
        }

        bool success = false;
        if (EmptyClipboard() != FALSE)
        {
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (memory != nullptr)
            {
                void* const locked = GlobalLock(memory);
                if (locked != nullptr)
                {
                    std::memcpy(locked, text, bytes);
                    GlobalUnlock(memory);
                    if (SetClipboardData(CF_TEXT, memory) != nullptr)
                    {
                        // Clipboard owns the handle after success.
                        memory = nullptr;
                        success = true;
                    }
                }
                if (memory != nullptr)
                {
                    GlobalFree(memory);
                }
            }
        }

        CloseClipboard();
        if (success)
        {
            return true;
        }

        Sleep(10);
    }

    return false;
}

void SetPhase(NetbridgeStatus* status, NetbridgePhase phase, const char* error)
{
    if (status == nullptr)
    {
        return;
    }

    const NetbridgePhase oldPhase = static_cast<NetbridgePhase>(status->phase);
    char oldError[sizeof(status->errorMsg)] = {};
    std::memcpy(oldError, status->errorMsg, sizeof(status->errorMsg));

    status->phase = static_cast<int>(phase);
    status->phaseTick = GetTickCount();
    if (error != nullptr)
    {
        CopyString(status->errorMsg, sizeof(status->errorMsg), error);
    }
    else if (phase != NetbridgePhase::Failed)
    {
        status->errorMsg[0] = '\0';
    }

    if (oldPhase != phase || std::strncmp(oldError, status->errorMsg, sizeof(status->errorMsg)) != 0)
    {
        mod::Log(
            "Takeover: phase %s -> %s reason='%s'",
            netplay::bridge::PhaseToString(oldPhase),
            netplay::bridge::PhaseToString(phase),
            status->errorMsg[0] != '\0' ? status->errorMsg : "");
    }
}

void CloseProcessHandle(NetbridgeStatus* status)
{
    if (g_revivalProcess != nullptr)
    {
        CloseHandle(g_revivalProcess);
        g_revivalProcess = nullptr;
    }
    g_revivalProcessId = 0;
    g_remoteInjectedSelfBase = 0;
    g_lastLatePatchRetryTick = 0;
    g_lastLatePatchRetryLogTick = 0;
    g_latePatchRetryAttempts = 0;
    g_latePatchRetrySuccesses = 0;
    g_lastLatePatchRetryResultValid = false;
    g_lastLatePatchRetryResult = false;
    g_observedTakeoverCreatePath = false;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    if (status != nullptr)
    {
        status->processId = 0;
    }
}

bool ProcessAlive(NetbridgeStatus* status)
{
    if (g_revivalProcess == nullptr)
    {
        return false;
    }
    DWORD exitCode = 0;
    if (GetExitCodeProcess(g_revivalProcess, &exitCode) == FALSE || exitCode != STILL_ACTIVE)
    {
        CloseProcessHandle(status);
        return false;
    }
    return true;
}

bool IsSyncReadyForVsHuman(const NetbridgeStatus* status)
{
    return status != nullptr
        && status->syncGameMode == 3
        && status->syncMode0Flag1084 == 4
        && (status->syncSessionByte == 0 || status->syncSessionByte == 1 || status->syncSessionByte == 2);
}

void ReadDelayPromptSignal(LONG* outPromptSerial, LONG* outPromptServedSerial)
{
    LONG promptSerial = InterlockedCompareExchange(&g_injectedDelayPromptSerial, 0, 0);
    LONG promptServedSerial = InterlockedCompareExchange(&g_injectedDelayPromptServedSerial, 0, 0);

    if (g_hostBlock != nullptr)
    {
        const LONG sharedPromptSerial = InterlockedCompareExchange(&g_hostBlock->delayPromptSerial, 0, 0);
        const LONG sharedPromptServedSerial = InterlockedCompareExchange(&g_hostBlock->delayPromptServedSerial, 0, 0);
        if (sharedPromptSerial > promptSerial)
        {
            promptSerial = sharedPromptSerial;
        }
        if (sharedPromptServedSerial > promptServedSerial)
        {
            promptServedSerial = sharedPromptServedSerial;
        }
    }

    if (outPromptSerial != nullptr)
    {
        *outPromptSerial = promptSerial;
    }
    if (outPromptServedSerial != nullptr)
    {
        *outPromptServedSerial = promptServedSerial;
    }
}

struct RuntimeReadyProbe
{
    bool nativeSyncReady = false;
    bool localInitApplied = false;
    bool delayPromptSeen = false;
    bool delayInputApplied = false;
    bool sessionPointerValid = false;
    bool helperPidMatches = false;
    bool helperHandleMatches = false;
    bool helperBindingReady = false;
    bool ready = false;
    const char* source = "none";
};

RuntimeReadyProbe EvaluateRuntimeReadyProbe(const NetbridgeStatus* status)
{
    RuntimeReadyProbe probe = {};
    probe.nativeSyncReady = IsSyncReadyForVsHuman(status);
    if (probe.nativeSyncReady)
    {
        probe.ready = true;
        probe.source = "native_sync";
        return probe;
    }

    probe.localInitApplied = g_localInitAppliedForSession;
    if (!probe.localInitApplied)
    {
        return probe;
    }

    LONG promptSerial = 0;
    LONG promptServedSerial = 0;
    ReadDelayPromptSignal(&promptSerial, &promptServedSerial);
    probe.delayPromptSeen = promptSerial > 0;
    if (!probe.delayPromptSeen)
    {
        return probe;
    }

    LONG inputSerial = 0;
    LONG inputServedSerial = 0;
    if (g_hostBlock != nullptr)
    {
        inputSerial = InterlockedCompareExchange(&g_hostBlock->delayInputSerial, 0, 0);
        inputServedSerial = InterlockedCompareExchange(&g_hostBlock->delayInputServedSerial, 0, 0);
    }

    probe.delayInputApplied =
        (inputSerial > 0 && inputServedSerial >= inputSerial)
        || (inputSerial <= 0 && promptServedSerial >= promptSerial);
    if (!probe.delayInputApplied)
    {
        return probe;
    }

    if (g_revivalProcessId == 0)
    {
        return probe;
    }

    const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
    probe.sessionPointerValid = sessionPtr != 0;
    if (!probe.sessionPointerValid)
    {
        return probe;
    }

    int helperPidField = -1;
    uintptr_t helperHandleFieldRaw = 0;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetHelperPid), &helperPidField);
    (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetHelperHandle), &helperHandleFieldRaw);

    probe.helperPidMatches = helperPidField == static_cast<int>(g_revivalProcessId);
    const HANDLE helperHandle = reinterpret_cast<HANDLE>(helperHandleFieldRaw);
    if (helperHandle != nullptr && helperHandle != INVALID_HANDLE_VALUE)
    {
        probe.helperHandleMatches = GetProcessId(helperHandle) == g_revivalProcessId;
    }

    probe.helperBindingReady = probe.helperPidMatches || probe.helperHandleMatches;
    if (probe.helperBindingReady)
    {
        probe.ready = true;
        probe.source = "post_delay_binding";
    }

    return probe;
}

bool HasRuntimeReadySignal(const NetbridgeStatus* status)
{
    return EvaluateRuntimeReadyProbe(status).ready;
}

uint32_t BuildRuntimeReadyProbeMask(const RuntimeReadyProbe& probe)
{
    uint32_t mask = 0;
    if (probe.nativeSyncReady)
    {
        mask |= (1u << 0);
    }
    if (probe.localInitApplied)
    {
        mask |= (1u << 1);
    }
    if (probe.delayPromptSeen)
    {
        mask |= (1u << 2);
    }
    if (probe.delayInputApplied)
    {
        mask |= (1u << 3);
    }
    if (probe.sessionPointerValid)
    {
        mask |= (1u << 4);
    }
    if (probe.helperPidMatches)
    {
        mask |= (1u << 5);
    }
    if (probe.helperHandleMatches)
    {
        mask |= (1u << 6);
    }
    if (probe.ready)
    {
        mask |= (1u << 7);
    }
    return mask;
}

bool EnsureHostIpc()
{
    if (g_hostBlock != nullptr && g_hostInitEvent != nullptr && g_hostConsoleEvent != nullptr)
    {
        return true;
    }

    if (g_hostMapHandle == nullptr)
    {
        g_hostMapHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedBlock), kSharedBlockName);
        if (g_hostMapHandle == nullptr)
        {
            mod::Log("Takeover: CreateFileMapping failed: %s", ErrorString(GetLastError()).c_str());
            return false;
        }
    }
    if (g_hostBlock == nullptr)
    {
        g_hostBlock = static_cast<SharedBlock*>(MapViewOfFile(g_hostMapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
        if (g_hostBlock == nullptr)
        {
            mod::Log("Takeover: MapViewOfFile failed: %s", ErrorString(GetLastError()).c_str());
            return false;
        }
    }

    if (g_hostInitEvent == nullptr)
    {
        g_hostInitEvent = CreateEventA(nullptr, FALSE, FALSE, kInitReadyEventName);
        if (g_hostInitEvent == nullptr)
        {
            return false;
        }
    }
    if (g_hostConsoleEvent == nullptr)
    {
        g_hostConsoleEvent = CreateEventA(nullptr, FALSE, FALSE, kConsoleReadyEventName);
        if (g_hostConsoleEvent == nullptr)
        {
            return false;
        }
    }

    g_hostBlock->magic = kIpcMagic;
    g_hostBlock->version = kIpcVersion;
    g_hostBlock->hostPid = GetCurrentProcessId();
    g_hostBlock->hostRevivalBase = static_cast<uint32_t>(g_hostRevivalBase);
    return true;
}

void CloseHostIpc()
{
    if (g_hostBlock != nullptr)
    {
        UnmapViewOfFile(g_hostBlock);
        g_hostBlock = nullptr;
    }
    if (g_hostMapHandle != nullptr)
    {
        CloseHandle(g_hostMapHandle);
        g_hostMapHandle = nullptr;
    }
    if (g_hostInitEvent != nullptr)
    {
        CloseHandle(g_hostInitEvent);
        g_hostInitEvent = nullptr;
    }
    if (g_hostConsoleEvent != nullptr)
    {
        CloseHandle(g_hostConsoleEvent);
        g_hostConsoleEvent = nullptr;
    }
    g_hostRevivalBase = 0;
}

bool EnsureLocalRevivalLoaded()
{
    if (g_localInitFn != nullptr)
    {
        PublishHostRevivalBase();
        if (!PatchRevivalErrorCodeNullGuard())
        {
            mod::Log("Takeover: warning failed to verify EfzRevival null-guard");
        }
        if (g_localRoleFlag < 0)
        {
            g_localRoleFlag = kLocalRoleLocalPlay;
        }
        return true;
    }

    if (g_localRevivalModule == nullptr)
    {
        g_localRevivalModule = GetModuleHandleA("EfzRevival.dll");
        if (g_localRevivalModule == nullptr)
        {
            g_localRevivalModule = LoadLibraryA("EfzRevival.dll");
        }
    }
    if (g_localRevivalModule == nullptr)
    {
        mod::Log("Takeover: LoadLibrary(EfzRevival.dll) failed");
        return false;
    }

    PublishHostRevivalBase();
    if (!PatchRevivalErrorCodeNullGuard())
    {
        mod::Log("Takeover: warning failed to patch EfzRevival null-guard");
    }

    g_localInitFn = reinterpret_cast<RevivalInitFn>(GetProcAddress(g_localRevivalModule, "init"));
    if (g_localInitFn == nullptr)
    {
        mod::Log("Takeover: GetProcAddress(init) failed");
        return false;
    }

    int localParams[2] = {2, 102};
    const int initResult = g_localInitFn(localParams);
    g_localRoleFlag = kLocalRoleLocalPlay;
    mod::Log("Takeover: local init(2,102) result=%d", initResult);
    return true;
}

void ReinitLocalPlay()
{
    if (g_localRoleFlag == kLocalRoleLocalPlay)
    {
        mod::Log("Takeover: local re-init skipped (already local play)");
        return;
    }

    // Calling Revival init() during teardown/timeout paths can race with
    // in-flight audio/network cleanup and crash. Keep local role state in
    // local play mode without forcing another immediate init() call.
    mod::Log(
        "Takeover: local re-init deferred (from role=%d -> local play, no init call)",
        g_localRoleFlag);
    g_localRoleFlag = kLocalRoleLocalPlay;
}

uintptr_t ResolveHostRevivalBase()
{
    if (g_localRevivalModule != nullptr)
    {
        return reinterpret_cast<uintptr_t>(g_localRevivalModule);
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival != nullptr)
    {
        return reinterpret_cast<uintptr_t>(revival);
    }

    return 0;
}

bool PatchRevivalErrorCodeNullGuard()
{
    const uintptr_t base = ResolveHostRevivalBase();
    if (base == 0)
    {
        return false;
    }

    void* const stub = EnsureRevivalErrorCodeNullGuardStub();
    if (stub == nullptr)
    {
        return false;
    }

    uint8_t* const target = reinterpret_cast<uint8_t*>(base + kRevivalErrorCodeIsZeroRva);
    std::array<uint8_t, kRevivalErrorCodeIsZeroPatchSize> patchBytes = {};
    patchBytes[0] = 0xE9;
    const intptr_t delta = reinterpret_cast<uint8_t*>(stub) - (target + 5);
    const int32_t rel = static_cast<int32_t>(delta);
    std::memcpy(&patchBytes[1], &rel, sizeof(rel));
    patchBytes[5] = 0x90;
    patchBytes[6] = 0x90;
    patchBytes[7] = 0x90;

    if (g_revivalErrorCodeNullGuardPatched && g_revivalErrorCodeNullGuardPatchedBase == base)
    {
        uint8_t verify[kRevivalErrorCodeIsZeroPatchSize] = {};
        std::memcpy(verify, target, sizeof(verify));
        if (std::memcmp(verify, patchBytes.data(), sizeof(verify)) == 0)
        {
            return true;
        }
        mod::Log("Takeover: null-guard bytes changed, reapplying");
        g_revivalErrorCodeNullGuardPatched = false;
    }

    constexpr std::array<uint8_t, 8> kExpectedPrefix = {
        0x33, 0xC0, 0x39, 0x01, 0x0F, 0x94, 0xC0, 0xC3,
    };
    constexpr std::array<uint8_t, 8> kLegacyPatchedPrefix = {
        0x85, 0xC9, 0x74, 0x06, 0x83, 0x39, 0x00, 0x0F,
    };

    uint8_t current[kRevivalErrorCodeIsZeroPatchSize] = {};
    std::memcpy(current, target, sizeof(current));
    if (std::memcmp(current, patchBytes.data(), sizeof(current)) == 0)
    {
        g_revivalErrorCodeNullGuardPatched = true;
        g_revivalErrorCodeNullGuardPatchedBase = base;
        return true;
    }

    if (std::memcmp(current, kExpectedPrefix.data(), kExpectedPrefix.size()) != 0
        && std::memcmp(current, kLegacyPatchedPrefix.data(), kLegacyPatchedPrefix.size()) != 0)
    {
        mod::Log(
            "Takeover: skip null-guard patch at +0x%04lX (unexpected bytes %02X %02X %02X %02X)",
            static_cast<unsigned long>(kRevivalErrorCodeIsZeroRva),
            static_cast<unsigned>(current[0]),
            static_cast<unsigned>(current[1]),
            static_cast<unsigned>(current[2]),
            static_cast<unsigned>(current[3]));
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, sizeof(patchBytes), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        mod::Log(
            "Takeover: failed null-guard patch protect at +0x%04lX err=%s",
            static_cast<unsigned long>(kRevivalErrorCodeIsZeroRva),
            ErrorString(GetLastError()).c_str());
        return false;
    }

    std::memcpy(target, patchBytes.data(), sizeof(patchBytes));
    (void)FlushInstructionCache(GetCurrentProcess(), target, sizeof(patchBytes));

    DWORD restoredProtect = 0;
    if (!VirtualProtect(target, sizeof(patchBytes), oldProtect, &restoredProtect))
    {
        mod::Log(
            "Takeover: null-guard patch restore protect failed at +0x%04lX err=%s",
            static_cast<unsigned long>(kRevivalErrorCodeIsZeroRva),
            ErrorString(GetLastError()).c_str());
    }

    g_revivalErrorCodeNullGuardPatched = true;
    g_revivalErrorCodeNullGuardPatchedBase = base;
    mod::Log(
        "Takeover: patched EfzRevival.dll null-guard at +0x%04lX",
        static_cast<unsigned long>(kRevivalErrorCodeIsZeroRva));
    return true;
}

void PublishHostRevivalBase()
{
    const uintptr_t base = ResolveHostRevivalBase();
    g_hostRevivalBase = base;
    if (g_hostBlock != nullptr)
    {
        g_hostBlock->hostRevivalBase = static_cast<uint32_t>(base);
    }
    if (base != 0)
    {
        mod::Log("Takeover: host revival base=0x%08lX", static_cast<unsigned long>(base));
    }
}

uintptr_t ResolveInjectedExpectedRevivalBase()
{
    if (g_injectedBlock != nullptr && g_injectedBlock->hostRevivalBase != 0)
    {
        return static_cast<uintptr_t>(g_injectedBlock->hostRevivalBase);
    }

    TempIpcContext temp = {};
    uintptr_t base = 0;
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        base = static_cast<uintptr_t>(temp.block->hostRevivalBase);
    }
    CloseTempIpcContext(&temp);
    return base;
}

bool WriteIni(const std::string& gameDir, int role, uint16_t port, const char* address, const char* nickname)
{
    std::string iniPath = gameDir;
    if (!iniPath.empty())
    {
        iniPath += "\\";
    }
    iniPath += "EfzRevival.ini";

    const DWORD existingAttrs = GetFileAttributesA(iniPath.c_str());
    const bool existed = (existingAttrs != INVALID_FILE_ATTRIBUTES);
    const char* safeAddress = (address != nullptr) ? address : "";
    const char* safeNickname = (nickname != nullptr && nickname[0] != '\0') ? nickname : "Player";
    char portText[16] = {};
    std::snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));

    auto writeIniKey = [&iniPath](const char* section, const char* key, const char* value) -> bool {
        if (WritePrivateProfileStringA(section, key, value, iniPath.c_str()) == FALSE)
        {
            mod::Log(
                "Takeover: WriteIni key failed section='%s' key='%s' value='%s' err=%s",
                section != nullptr ? section : "",
                key != nullptr ? key : "",
                value != nullptr ? value : "",
                ErrorString(GetLastError()).c_str());
            return false;
        }
        return true;
    };
    bool ok = true;

    // Keep user INI intact. Update only the settings currently supported by
    // InGameNetplay menu integration.
    ok = writeIniKey("Network", "Name", safeNickname) && ok;
    ok = writeIniKey("Network", "Port", portText) && ok;

    mod::Log(
        "Takeover: WriteIni path='%s' existed=%d role=%d port=%u nickname='%s' address='%s' result=%d (updated keys: Network.Name, Network.Port)",
        iniPath.c_str(),
        existed ? 1 : 0,
        role,
        static_cast<unsigned>(port),
        safeNickname,
        safeAddress,
        ok ? 1 : 0);
    return ok;
}

struct RemoteModuleRecord
{
    uintptr_t base = 0;
    std::string moduleLower;
};

std::vector<RemoteModuleRecord> EnumerateRemoteModules(DWORD processId)
{
    std::vector<RemoteModuleRecord> modules;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snap == INVALID_HANDLE_VALUE)
    {
        return modules;
    }

    MODULEENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32First(snap, &entry) == TRUE)
    {
        do
        {
            RemoteModuleRecord record = {};
            record.base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            record.moduleLower = entry.szModule;
            std::transform(record.moduleLower.begin(), record.moduleLower.end(), record.moduleLower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            modules.push_back(std::move(record));
        }
        while (Module32Next(snap, &entry) == TRUE);
    }

    CloseHandle(snap);
    return modules;
}

bool ReadRemoteString(HANDLE process, uintptr_t address, char* out, size_t outSize)
{
    if (out == nullptr || outSize == 0)
    {
        return false;
    }
    size_t index = 0;
    while (index + 1 < outSize)
    {
        char ch = '\0';
        SIZE_T read = 0;
        if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address + index), &ch, sizeof(ch), &read) == FALSE || read != 1)
        {
            out[0] = '\0';
            return false;
        }
        out[index++] = ch;
        if (ch == '\0')
        {
            return true;
        }
    }
    out[outSize - 1] = '\0';
    return true;
}

bool InjectSelf(HANDLE process, uintptr_t* outRemoteBase)
{
    if (outRemoteBase == nullptr)
    {
        return false;
    }

    const std::string selfPath = ModulePath(SelfModule());
    if (selfPath.empty())
    {
        mod::Log("Takeover: failed to get self module path");
        return false;
    }

    const size_t size = selfPath.size() + 1;
    LPVOID remotePath = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remotePath == nullptr)
    {
        mod::Log("Takeover: VirtualAllocEx for path failed");
        return false;
    }

    SIZE_T written = 0;
    if (WriteProcessMemory(process, remotePath, selfPath.c_str(), size, &written) == FALSE || written != size)
    {
        mod::Log("Takeover: WriteProcessMemory for path failed");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));
    if (loadLibrary == nullptr)
    {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (thread == nullptr)
    {
        mod::Log("Takeover: CreateRemoteThread LoadLibraryA failed: %s", ErrorString(GetLastError()).c_str());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 10000);
    DWORD remoteBase = 0;
    if (wait != WAIT_OBJECT_0 || GetExitCodeThread(thread, &remoteBase) == FALSE || remoteBase == 0)
    {
        mod::Log("Takeover: remote LoadLibrary failed wait=%lu exit=0x%08lX", static_cast<unsigned long>(wait), static_cast<unsigned long>(remoteBase));
        CloseHandle(thread);
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    *outRemoteBase = static_cast<uintptr_t>(remoteBase);
    mod::Log("Takeover: injected self module base=0x%08lX", static_cast<unsigned long>(remoteBase));
    return true;
}

uint32_t RemoteExportAddress(uintptr_t remoteBase, const void* localExport)
{
    const uintptr_t localBase = reinterpret_cast<uintptr_t>(SelfModule());
    const uintptr_t localAddr = reinterpret_cast<uintptr_t>(localExport);
    return static_cast<uint32_t>(remoteBase + (localAddr - localBase));
}

std::unordered_map<std::string, uint32_t> BuildPatchMap(uintptr_t remoteBase)
{
    std::unordered_map<std::string, uint32_t> patches;
    patches["CreateProcessA"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_CreateProcessA));
    patches["OpenProcess"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_OpenProcess));
    patches["ReadProcessMemory"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_ReadProcessMemory));
    patches["VirtualAllocEx"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_VirtualAllocEx));
    patches["VirtualFreeEx"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_VirtualFreeEx));
    patches["WriteProcessMemory"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_WriteProcessMemory));
    patches["CreateRemoteThread"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_CreateRemoteThread));
    patches["TerminateProcess"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_TerminateProcess));
    patches["ReadConsoleA"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_ReadConsoleA));
    patches["ReadConsoleW"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_ReadConsoleW));
    patches["WriteFile"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_WriteFile));
    patches["WriteConsoleA"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_WriteConsoleA));
    patches["WriteConsoleW"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_WriteConsoleW));
    patches["WriteConsoleOutputCharacterA"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_WriteConsoleOutputCharacterA));
    patches["WriteConsoleOutputCharacterW"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_WriteConsoleOutputCharacterW));
    patches["OutputDebugStringA"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_OutputDebugStringA));
    patches["OutputDebugStringW"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_OutputDebugStringW));
    patches["WaitForSingleObject"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_WaitForSingleObject));
    patches["GetExitCodeThread"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_GetExitCodeThread));
    patches["ResumeThread"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_ResumeThread));
    return patches;
}

bool PatchIatModule(
    HANDLE process,
    uintptr_t imageBase,
    const char* moduleName,
    const std::unordered_map<std::string, uint32_t>& patchMap,
    int* outPatchedCount,
    bool verboseLogs)
{
    if (outPatchedCount != nullptr)
    {
        *outPatchedCount = 0;
    }

    if (imageBase == 0 || moduleName == nullptr || moduleName[0] == '\0')
    {
        return false;
    }

    IMAGE_DOS_HEADER dos = {};
    SIZE_T read = 0;
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(imageBase), &dos, sizeof(dos), &read) == FALSE || read != sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
    {
        return false;
    }

    IMAGE_NT_HEADERS32 nt = {};
    const uintptr_t ntAddress = imageBase + static_cast<uintptr_t>(dos.e_lfanew);
    if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(ntAddress), &nt, sizeof(nt), &read) == FALSE || read != sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE)
    {
        return false;
    }

    const DWORD importRva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (importRva == 0)
    {
        if (verboseLogs)
        {
            mod::Log("Takeover: module '%s' has no import directory", moduleName);
        }
        return true;
    }

    int patched = 0;
    for (DWORD idx = 0;; ++idx)
    {
        IMAGE_IMPORT_DESCRIPTOR desc = {};
        const uintptr_t descAddress = imageBase + importRva + static_cast<uintptr_t>(idx) * sizeof(desc);
        if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(descAddress), &desc, sizeof(desc), &read) == FALSE || read != sizeof(desc))
        {
            return false;
        }
        if (desc.Name == 0)
        {
            break;
        }

        char dllName[128] = {};
        if (!ReadRemoteString(process, imageBase + desc.Name, dllName, sizeof(dllName)))
        {
            continue;
        }

        const bool isKernelProvider =
            (_stricmp(dllName, "KERNEL32.dll") == 0) ||
            (_stricmp(dllName, "KERNELBASE.dll") == 0) ||
            (_strnicmp(dllName, "api-ms-win-core-", 16) == 0) ||
            (_strnicmp(dllName, "api-ms-win-crt-", 15) == 0) ||
            (_strnicmp(dllName, "ext-ms-win-", 11) == 0);
        if (!isKernelProvider)
        {
            continue;
        }

        const DWORD oftRva = desc.OriginalFirstThunk != 0 ? desc.OriginalFirstThunk : desc.FirstThunk;
        const DWORD ftRva = desc.FirstThunk;

        for (DWORD thunk = 0;; ++thunk)
        {
            IMAGE_THUNK_DATA32 oft = {};
            const uintptr_t oftAddress = imageBase + oftRva + static_cast<uintptr_t>(thunk) * sizeof(oft);
            if (ReadProcessMemory(process, reinterpret_cast<LPCVOID>(oftAddress), &oft, sizeof(oft), &read) == FALSE || read != sizeof(oft))
            {
                return false;
            }
            if (oft.u1.AddressOfData == 0)
            {
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL32(oft.u1.Ordinal))
            {
                continue;
            }

            char importName[128] = {};
            if (!ReadRemoteString(process, imageBase + oft.u1.AddressOfData + 2, importName, sizeof(importName)))
            {
                continue;
            }

            const auto it = patchMap.find(importName);
            if (it == patchMap.end())
            {
                continue;
            }

            const uintptr_t ftAddress = imageBase + ftRva + static_cast<uintptr_t>(thunk) * sizeof(uint32_t);
            DWORD oldProtect = 0;
            (void)VirtualProtectEx(process, reinterpret_cast<LPVOID>(ftAddress), sizeof(uint32_t), PAGE_READWRITE, &oldProtect);

            uint32_t oldAddress = 0;
            SIZE_T oldRead = 0;
            (void)ReadProcessMemory(process, reinterpret_cast<LPCVOID>(ftAddress), &oldAddress, sizeof(oldAddress), &oldRead);

            const uint32_t newAddress = it->second;
            if (oldAddress != newAddress)
            {
                SIZE_T written = 0;
                if (WriteProcessMemory(process, reinterpret_cast<LPVOID>(ftAddress), &newAddress, sizeof(newAddress), &written) == FALSE || written != sizeof(newAddress))
                {
                    return false;
                }
            }
            DWORD ignored = 0;
            (void)VirtualProtectEx(process, reinterpret_cast<LPVOID>(ftAddress), sizeof(uint32_t), oldProtect, &ignored);

            ++patched;
            auto shouldLogPatchedImport = [](const char* name) -> bool {
                if (name == nullptr || name[0] == '\0')
                {
                    return false;
                }
                return _stricmp(name, "CreateProcessA") == 0
                    || _stricmp(name, "OpenProcess") == 0
                    || _stricmp(name, "TerminateProcess") == 0
                    || _stricmp(name, "ReadConsoleA") == 0
                    || _stricmp(name, "ReadConsoleW") == 0
                    || _stricmp(name, "WriteConsoleOutputCharacterW") == 0
                    || _stricmp(name, "OutputDebugStringW") == 0;
            };
            if (verboseLogs && oldAddress != newAddress && shouldLogPatchedImport(importName))
            {
                mod::Log(
                    "Takeover: module '%s' patched import %s old=0x%08lX new=0x%08lX",
                    moduleName,
                    importName,
                    static_cast<unsigned long>(oldAddress),
                    static_cast<unsigned long>(newAddress));
            }
        }
    }

    if (outPatchedCount != nullptr)
    {
        *outPatchedCount = patched;
    }
    if (verboseLogs)
    {
        mod::Log("Takeover: module '%s' patched import count=%d", moduleName, patched);
    }
    return true;
}

bool PatchIat(HANDLE process, DWORD processId, const std::unordered_map<std::string, uint32_t>& patchMap, bool verboseLogs)
{
    auto isRuntimePatchModule = [](const std::string& moduleLower) -> bool {
        if (moduleLower.empty())
        {
            return false;
        }

        if (moduleLower == "kernel32.dll" || moduleLower == "kernelbase.dll" || moduleLower == "ntdll.dll")
        {
            return false;
        }

        auto startsWith = [&](const char* prefix) -> bool {
            const size_t n = std::strlen(prefix);
            return moduleLower.size() >= n && moduleLower.compare(0, n, prefix) == 0;
        };

        return startsWith("msvcr")
            || startsWith("msvcp")
            || startsWith("vcruntime")
            || startsWith("ucrtbase")
            || startsWith("api-ms-win-crt")
            || startsWith("concrt");
    };

    auto isRevivalTargetModule = [](const std::string& moduleLower) -> bool {
        if (moduleLower == "efz_netplay_mod.dll")
        {
            return false;
        }
        if (moduleLower == "efzrevival.exe" || moduleLower == "efzrevival.dll")
        {
            return true;
        }

        if (moduleLower.find("efzrevival") == std::string::npos)
        {
            return false;
        }

        const bool isExe = moduleLower.size() >= 4 && moduleLower.rfind(".exe") == (moduleLower.size() - 4);
        const bool isDll = moduleLower.size() >= 4 && moduleLower.rfind(".dll") == (moduleLower.size() - 4);
        return isExe || isDll;
    };

    const std::vector<RemoteModuleRecord> modules = EnumerateRemoteModules(processId);
    if (modules.empty())
    {
        mod::Log("Takeover: PatchIat failed to enumerate remote modules");
        return false;
    }

    std::vector<RemoteModuleRecord> targets;
    targets.reserve(16);
    auto pushUniqueTarget = [&](const RemoteModuleRecord& candidate) {
        for (const RemoteModuleRecord& existing : targets)
        {
            if (existing.base == candidate.base)
            {
                return;
            }
        }
        targets.push_back(candidate);
    };

    for (const RemoteModuleRecord& module : modules)
    {
        if (module.moduleLower == "efzrevival.exe")
        {
            pushUniqueTarget(module);
        }
    }

    for (const RemoteModuleRecord& module : modules)
    {
        if (module.moduleLower == "efzrevival.dll")
        {
            pushUniqueTarget(module);
        }
    }

    if (targets.empty())
    {
        for (const RemoteModuleRecord& module : modules)
        {
            if (isRevivalTargetModule(module.moduleLower))
            {
                pushUniqueTarget(module);
            }
        }
    }

    // Runtime CRT modules often own WriteConsole*/WriteFile paths used by
    // ostream/log output; patch them too so prompt detection sees console text.
    for (const RemoteModuleRecord& module : modules)
    {
        if (isRuntimePatchModule(module.moduleLower))
        {
            pushUniqueTarget(module);
        }
    }

    if (targets.empty())
    {
        mod::Log("Takeover: PatchIat no matching modules in process module list");
        for (const RemoteModuleRecord& module : modules)
        {
            mod::Log(
                "Takeover: module scan saw '%s' base=0x%08lX",
                module.moduleLower.c_str(),
                static_cast<unsigned long>(module.base));
        }
    }

    if (targets.empty())
    {
        mod::Log(
            "Takeover: PatchIat refused - no target modules found (first='%s')",
            modules.front().moduleLower.c_str());
        return false;
    }

    int totalPatched = 0;
    for (const RemoteModuleRecord& module : targets)
    {
        if (module.moduleLower == "efz_netplay_mod.dll")
        {
            if (verboseLogs)
            {
                mod::Log("Takeover: skipping self module patch target '%s'", module.moduleLower.c_str());
            }
            continue;
        }

        if (verboseLogs)
        {
            mod::Log(
                "Takeover: PatchIat target module='%s' base=0x%08lX",
                module.moduleLower.c_str(),
                static_cast<unsigned long>(module.base));
        }

        int patched = 0;
        if (!PatchIatModule(process, module.base, module.moduleLower.c_str(), patchMap, &patched, verboseLogs))
        {
            mod::Log(
                "Takeover: PatchIatModule failed module='%s' base=0x%08lX",
                module.moduleLower.c_str(),
                static_cast<unsigned long>(module.base));
            return false;
        }
        totalPatched += patched;
    }

    if (verboseLogs)
    {
        mod::Log(
            "Takeover: patched import total count=%d modules=%zu",
            totalPatched,
            static_cast<size_t>(targets.size()));
    }
    return totalPatched > 0;
}

HANDLE CreateFakeThread(DWORD exitCode)
{
    HANDLE handle = CreateEventA(nullptr, TRUE, TRUE, nullptr);
    if (handle == nullptr)
    {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_fakeThreadMutex);
    g_fakeThreads.push_back({handle, exitCode});
    return handle;
}

bool LookupFakeThread(HANDLE handle, DWORD* outExitCode)
{
    std::lock_guard<std::mutex> lock(g_fakeThreadMutex);
    for (const FakeThreadInfo& info : g_fakeThreads)
    {
        if (info.handle == handle)
        {
            if (outExitCode != nullptr)
            {
                *outExitCode = info.exitCode;
            }
            return true;
        }
    }
    return false;
}

void ClearFakeThreads()
{
    std::lock_guard<std::mutex> lock(g_fakeThreadMutex);
    for (const FakeThreadInfo& info : g_fakeThreads)
    {
        if (info.handle != nullptr)
        {
            CloseHandle(info.handle);
        }
    }
    g_fakeThreads.clear();
}

void RegisterRedirectAllocation(void* base, SIZE_T size)
{
    if (base == nullptr || size == 0)
    {
        return;
    }

    const uintptr_t allocBase = reinterpret_cast<uintptr_t>(base);
    std::lock_guard<std::mutex> lock(g_redirectAllocMutex);
    for (RedirectAllocationInfo& entry : g_redirectAllocations)
    {
        if (entry.base == allocBase)
        {
            entry.size = size;
            return;
        }
    }
    g_redirectAllocations.push_back({allocBase, size});
}

void ForgetRedirectAllocation(void* base)
{
    if (base == nullptr)
    {
        return;
    }

    const uintptr_t allocBase = reinterpret_cast<uintptr_t>(base);
    std::lock_guard<std::mutex> lock(g_redirectAllocMutex);
    g_redirectAllocations.erase(
        std::remove_if(
            g_redirectAllocations.begin(),
            g_redirectAllocations.end(),
            [allocBase](const RedirectAllocationInfo& entry)
            {
                return entry.base == allocBase;
            }),
        g_redirectAllocations.end());
}

bool IsWithinRedirectAllocation(const void* address, SIZE_T size)
{
    if (address == nullptr || size == 0)
    {
        return false;
    }

    const uintptr_t writeBase = reinterpret_cast<uintptr_t>(address);
    const uintptr_t writeEnd = writeBase + size;
    if (writeEnd < writeBase)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_redirectAllocMutex);
    for (const RedirectAllocationInfo& entry : g_redirectAllocations)
    {
        const uintptr_t allocBase = entry.base;
        const uintptr_t allocEnd = allocBase + entry.size;
        if (allocEnd <= allocBase)
        {
            continue;
        }
        if (writeBase >= allocBase && writeEnd <= allocEnd)
        {
            return true;
        }
    }
    return false;
}

void ClearRedirectAllocations()
{
    std::lock_guard<std::mutex> lock(g_redirectAllocMutex);
    g_redirectAllocations.clear();
}

bool HasInjectedContext()
{
    return g_injectedReady && g_injectedBlock != nullptr && g_injectedInitEvent != nullptr && g_injectedConsoleEvent != nullptr;
}

void TryLazyBootstrapInjected()
{
    if (!IsCurrentProcessRevival() || HasInjectedContext())
    {
        return;
    }

    if (InterlockedCompareExchange(&g_injectedLazyBootstrapState, 1, 0) != 0)
    {
        return;
    }

    const HMODULE self = SelfModule();
    if (self != nullptr)
    {
        (void)mod::InitializeLogger(self, false);
    }
    mod::Log("Takeover: injected lazy bootstrap start");
    InitializeInjected();

    const bool ready = HasInjectedContext();
    mod::Log("Takeover: injected lazy bootstrap result=%d", ready ? 1 : 0);
    InterlockedExchange(&g_injectedLazyBootstrapState, ready ? 2 : 0);
}

bool EnsureInjectedContextFast()
{
    if (HasInjectedContext())
    {
        return true;
    }

    TryLazyBootstrapInjected();
    if (HasInjectedContext())
    {
        return true;
    }

    if (g_injectedMapHandle == nullptr)
    {
        g_injectedMapHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kSharedBlockName);
    }
    if (g_injectedMapHandle != nullptr && g_injectedBlock == nullptr)
    {
        g_injectedBlock = static_cast<SharedBlock*>(MapViewOfFile(g_injectedMapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
    }
    if (g_injectedInitEvent == nullptr)
    {
        g_injectedInitEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kInitReadyEventName);
    }
    if (g_injectedConsoleEvent == nullptr)
    {
        g_injectedConsoleEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kConsoleReadyEventName);
    }

    g_injectedReady = (g_injectedBlock != nullptr && g_injectedInitEvent != nullptr && g_injectedConsoleEvent != nullptr);
    if (g_injectedReady && !g_injectedLazyBound)
    {
        g_injectedLazyBound = true;
        mod::Log(
            "Takeover: injected lazy-bind ready block=0x%p init=0x%p console=0x%p",
            g_injectedBlock,
            g_injectedInitEvent,
            g_injectedConsoleEvent);
    }

    return HasInjectedContext();
}

} // namespace

bool IsCurrentProcessRevival()
{
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
    {
        return false;
    }
    return BaseLower(path) == "efzrevival.exe";
}

void InitializeHost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    (void)EnsureHostIpc();
    if (!EnsureLocalRevivalLoaded())
    {
        mod::Log("Takeover: host local revival load failed");
    }
    else
    {
        // Keep offline tooling active by default.
        (void)SetLocalRoleFlag(kLocalRoleLocalPlay, "host_initialize");
    }
    mod::Log("Takeover: host initialized");
}

void ShutdownHost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_revivalProcess != nullptr)
    {
        TerminateProcess(g_revivalProcess, 0);
    }
    CloseProcessHandle(nullptr);
    CloseHostIpc();
    g_localRoleFlag = -1;
    g_localInitAppliedForSession = false;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_injectedDelayPromptWaitStartTick = 0;
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
    mod::Log("Takeover: host shutdown");
}

void EmergencyShutdownHost()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    InterlockedExchange(&g_startAbortRequested, 1);
    if (g_revivalProcess != nullptr)
    {
        TerminateProcess(g_revivalProcess, 0);
    }
    CloseProcessHandle(nullptr);
    CloseHostIpc();
    g_localRoleFlag = -1;
    g_localInitAppliedForSession = false;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_injectedDelayPromptWaitStartTick = 0;
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
}

void InitializeInjected()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (g_injectedMapHandle == nullptr)
        {
            g_injectedMapHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kSharedBlockName);
        }
        if (g_injectedMapHandle != nullptr && g_injectedBlock == nullptr)
        {
            g_injectedBlock = static_cast<SharedBlock*>(MapViewOfFile(g_injectedMapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedBlock)));
        }
        if (g_injectedInitEvent == nullptr)
        {
            g_injectedInitEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kInitReadyEventName);
        }
        if (g_injectedConsoleEvent == nullptr)
        {
            g_injectedConsoleEvent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, kConsoleReadyEventName);
        }

        if (g_injectedBlock != nullptr && g_injectedInitEvent != nullptr && g_injectedConsoleEvent != nullptr)
        {
            break;
        }
        Sleep(10);
    }

    g_remoteThreadCallIndex = 0;
    g_startAbortRequested = 0;
    g_injectedLastConsoleSerialServed = 0;
    g_injectedLastConsoleAuxSerialServed = 0;
    g_injectedActiveConsoleAuxSerial = 0;
    g_injectedConsoleAuxScriptOffset = 0;
    g_injectedAutoConsoleFallbackCount = 0;
    g_injectedConsoleOutputHits = 0;
    g_injectedDelayPromptWaitStartTick = 0;
    g_fakeProcessThreadHandle = nullptr;
    g_initCapturedFromWrite = false;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_injectedInitAddress = 0;
    g_injectedLazyBound = false;
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    ClearFakeThreads();
    ClearRedirectAllocations();
    g_redirectWriteBlockedHits = 0;
    g_injectedReady = (g_injectedBlock != nullptr && g_injectedInitEvent != nullptr && g_injectedConsoleEvent != nullptr);
    if (g_injectedReady)
    {
        HMODULE revival = GetModuleHandleA("EfzRevival.dll");
        if (revival != nullptr)
        {
            const FARPROC initProc = GetProcAddress(revival, "init");
            g_injectedInitAddress = reinterpret_cast<uintptr_t>(initProc);
        }
    }
    mod::Log(
        "Takeover: injected initialized ready=%d block=0x%p init=0x%p console=0x%p initAddr=0x%p hostRevivalBase=0x%08lX",
        g_injectedReady ? 1 : 0,
        g_injectedBlock,
        g_injectedInitEvent,
        g_injectedConsoleEvent,
        reinterpret_cast<void*>(g_injectedInitAddress),
        static_cast<unsigned long>(g_injectedBlock != nullptr ? g_injectedBlock->hostRevivalBase : 0));
    InterlockedExchange(&g_injectedLazyBootstrapState, g_injectedReady ? 2 : 0);
}

void ShutdownInjected()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ClearFakeThreads();
    ClearRedirectAllocations();

    if (g_injectedBlock != nullptr)
    {
        UnmapViewOfFile(g_injectedBlock);
        g_injectedBlock = nullptr;
    }
    if (g_injectedMapHandle != nullptr)
    {
        CloseHandle(g_injectedMapHandle);
        g_injectedMapHandle = nullptr;
    }
    if (g_injectedInitEvent != nullptr)
    {
        CloseHandle(g_injectedInitEvent);
        g_injectedInitEvent = nullptr;
    }
    if (g_injectedConsoleEvent != nullptr)
    {
        CloseHandle(g_injectedConsoleEvent);
        g_injectedConsoleEvent = nullptr;
    }
    g_injectedReady = false;
    g_injectedInitAddress = 0;
    g_injectedLazyBound = false;
    g_startAbortRequested = 0;
    g_injectedLastConsoleSerialServed = 0;
    g_injectedLastConsoleAuxSerialServed = 0;
    g_injectedActiveConsoleAuxSerial = 0;
    g_injectedConsoleAuxScriptOffset = 0;
    g_injectedAutoConsoleFallbackCount = 0;
    g_injectedConsoleOutputHits = 0;
    g_injectedDelayPromptWaitStartTick = 0;
    g_fakeProcessThreadHandle = nullptr;
    g_initCapturedFromWrite = false;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_localRoleFlag = -1;
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    InterlockedExchange(&g_injectedLazyBootstrapState, 0);
    mod::Log("Takeover: injected shutdown");
}

void RequestAbortStart()
{
    InterlockedExchange(&g_startAbortRequested, 1);
}

void OnTitleSelectionConfirmed(int selection, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!EnsureLocalRevivalLoaded())
    {
        return;
    }

    const bool sessionActive = ProcessAlive(ioStatus);
    if (sessionActive)
    {
        return;
    }

    // Revival mode=3 routes to offline tournament flow; VS-human handoff must
    // stay on play mode (2) to preserve rollback session continuity.
    const int desiredRole = kLocalRoleLocalPlay;
    (void)SetLocalRoleFlag(desiredRole, selection == 2 ? "title_vs_human" : "title_other");
    RefreshRuntimeStatus(ioStatus);
}

bool StartSession(NetbridgeRole role, uint16_t port, const char* address, const char* nickname, NetbridgeStatus* ioStatus, uint32_t* outConnectStartTick)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    mod::Log(
        "Takeover: StartSession role=%d port=%u address='%s' nickname='%s'",
        static_cast<int>(role),
        static_cast<unsigned>(port),
        (address != nullptr) ? address : "",
        (nickname != nullptr) ? nickname : "");

    InterlockedExchange(&g_startAbortRequested, 0);

    if (!EnsureHostIpc())
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "IPC setup failed");
        return false;
    }
    if (!EnsureLocalRevivalLoaded())
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "EfzRevival.dll unavailable");
        return false;
    }

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
    g_lastConnectingDiagnosticTick = 0;
    g_lastSessionPtrOffset = 0;
    g_lastValidatedSessionPtr = 0;
    g_lastSessionPointerMismatchTick = 0;
    g_lastRuntimeReadyProbeLogTick = 0;
    g_lastRuntimeReadyProbeMask = 0;
    g_lastRuntimeReadyProbeMaskValid = false;
    if (outConnectStartTick != nullptr)
    {
        *outConnectStartTick = GetTickCount();
    }

    const std::string gameDir = GameDirectory();
    if (!WriteIni(gameDir, static_cast<int>(role), port, address, nickname))
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

    BOOL created = CreateProcessA(
        exePath.c_str(),
        nullptr,
        nullptr,
        nullptr,
        FALSE,
        CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr,
        gameDir.empty() ? nullptr : gameDir.c_str(),
        &si,
        &pi);

    if (!created)
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "CreateProcess(EfzRevival.exe) failed");
        return false;
    }

    mod::Log("Takeover: spawned EfzRevival suspended pid=%lu", static_cast<unsigned long>(pi.dwProcessId));

    uintptr_t remoteBase = 0;
    if (!InjectSelf(pi.hProcess, &remoteBase))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "Self injection failed");
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    std::unordered_map<std::string, uint32_t> patches = BuildPatchMap(remoteBase);

    if (!PatchIat(pi.hProcess, pi.dwProcessId, patches, true))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "IAT patch failed");
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    // Both Host and Join use roleFlag=0 (online). The host/join distinction
    // is encoded in Revival's internal shared-memory init block, not roleFlag.
    // Spectate uses roleFlag=1.
    int localRoleMode = kLocalRoleOnline;
    if (role == NetbridgeRole::Spectate)
    {
        localRoleMode = kLocalRoleSpectate;
    }

    g_hostBlock->initParams[0] = localRoleMode;
    g_hostBlock->initParams[1] = 102;
    ResetDebugCounters(g_hostBlock);
    g_localInitAppliedForSession = false;
    g_sessionHistoryRepairHits = 0;
    InterlockedExchange(&g_injectedDelayPromptSerial, 0);
    InterlockedExchange(&g_injectedDelayPromptServedSerial, 0);
    InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, 0);
    g_injectedDelayPromptWaitStartTick = 0;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_holePunchServerConfigLoaded = false;
    g_configuredHolePunchServer.clear();
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
        g_diskCapturePathHits.clear();
    }
    InterlockedIncrement(&g_hostBlock->initSerial);

    std::string primaryInput;
    std::string auxInput;
    int menuChoice = 0;
    if (role == NetbridgeRole::Host)
    {
        menuChoice = 1;
        primaryInput = "1\r\n";
        // Host startup may require one hidden follow-up confirmation read
        // before visible delay/ping prompts are emitted.
        auxInput = "\r\n";
    }
    else if (role == NetbridgeRole::Join)
    {
        // Use option 3 ("Join from clipboard") instead of option 2 (typed
        // address). Option 3 avoids a second ReadConsoleA call for the
        // ip:port string — the address is seeded to the clipboard instead.
        menuChoice = 3;
        primaryInput = "3\r\n";

        if (address != nullptr && address[0] != '\0')
        {
            std::string clipboardAddress = address;
            clipboardAddress.erase(
                std::remove_if(
                    clipboardAddress.begin(),
                    clipboardAddress.end(),
                    [](char c) { return c == '\r' || c == '\n'; }),
                clipboardAddress.end());

            if (!clipboardAddress.empty())
            {
                if (TryWriteClipboardAscii(clipboardAddress.c_str()))
                {
                    mod::Log("Takeover: join clipboard seeded with address");
                }
                else
                {
                    mod::Log("Takeover: join clipboard write failed; Revival will use existing clipboard");
                }
            }
        }
    }
    else if (role == NetbridgeRole::Spectate)
    {
        // Spectate flow in EfzRevival is option 4 ("Spectate from clipboard").
        menuChoice = 4;
        primaryInput = "4\r\n";

        if (address != nullptr && address[0] != '\0')
        {
            std::string clipboardAddress = address;
            clipboardAddress.erase(
                std::remove_if(
                    clipboardAddress.begin(),
                    clipboardAddress.end(),
                    [](char c) { return c == '\r' || c == '\n'; }),
                clipboardAddress.end());

            if (clipboardAddress.empty())
            {
                // Nothing valid to put on the clipboard; keep option 4 and let
                // Revival use whatever clipboard entry already exists.
                mod::Log("Takeover: spectate address empty after trim; using clipboard option as-is");
            }
            else if (!TryWriteClipboardAscii(clipboardAddress.c_str()))
            {
                // Clipboard write failed. Log and proceed — Revival will use
                // whatever clipboard content already exists.  Do NOT fall back
                // to option 2 (typed join) because that creates a roleFlag
                // mismatch (spectate initParams vs join menu path) and requires
                // a fragile second ReadConsoleA interception.
                mod::Log("Takeover: spectate clipboard write failed; using option 4 with existing clipboard");
            }
            else
            {
                mod::Log("Takeover: spectate clipboard seeded with explicit address");
            }
        }
    }

    if (primaryInput.empty())
    {
        // Defensive fallback — should never be reached since every role branch
        // above sets primaryInput.  Default to host (option 1).
        primaryInput = "1\r\n";
        menuChoice = 1;
    }

    CopyString(g_hostBlock->consoleInput, sizeof(g_hostBlock->consoleInput), primaryInput.c_str());
    InterlockedIncrement(&g_hostBlock->consoleSerial);
    g_hostBlock->consoleInputAux[0] = '\0';
    LONG auxSerialForSession = 0;
    if (!auxInput.empty())
    {
        CopyString(g_hostBlock->consoleInputAux, sizeof(g_hostBlock->consoleInputAux), auxInput.c_str());
        auxSerialForSession = InterlockedIncrement(&g_hostBlock->consoleAuxSerial);
    }

    ResetEvent(g_hostInitEvent);
    ResetEvent(g_hostConsoleEvent);

    const DWORD resumeResult = ResumeThread(pi.hThread);
    mod::Log("Takeover: resumed main thread result=%lu", static_cast<unsigned long>(resumeResult));

    g_revivalProcess = pi.hProcess;
    g_revivalProcessId = pi.dwProcessId;
    g_remoteInjectedSelfBase = remoteBase;
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
    mod::Log("Takeover: start session armed (asynchronous handshake via Tick)");
    return true;
}

bool ApplyInputDelay(int delayFrames, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

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
        RefreshRuntimeStatus(ioStatus);
        if (ioStatus != nullptr)
        {
            ioStatus->rollbackFrames = value;
        }
        return true;
    };

    if (promptPending && queuePromptInput(delayFrames))
    {
        return true;
    }

    const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
    if (sessionPtr == 0)
    {
        mod::Log("Takeover: ApplyInputDelay failed (session pointer unavailable)");
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    void* const delayAddress = reinterpret_cast<void*>(sessionPtr + kSessionOffsetInputDelay);
    if (!IsWritableRange(delayAddress, sizeof(int)))
    {
        mod::Log(
            "Takeover: ApplyInputDelay failed (delay field not writable addr=0x%p)",
            delayAddress);
        RefreshRuntimeStatus(ioStatus);
        return false;
    }

    *reinterpret_cast<int*>(delayAddress) = delayFrames;
    mod::Log(
        "Takeover: ApplyInputDelay applied value=%d session=0x%08lX",
        delayFrames,
        static_cast<unsigned long>(sessionPtr));
    RefreshRuntimeStatus(ioStatus);
    if (ioStatus != nullptr)
    {
        ioStatus->rollbackFrames = delayFrames;
    }
    return true;
}

bool PrepareVsHumanHandoff(NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

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

    // After the init handshake, the local role is already set to online (0)
    // for host/join or spectator (1). Calling init() again with ANY roleFlag
    // would destroy the active session object and crash (access violation in
    // EfzRevival.dll when the game tries to use the now-null session pointer).
    // We must NOT re-init here.  The role is correct as-is.
    const bool roleSet = (g_localRoleFlag >= 0);
    RefreshRuntimeStatus(ioStatus);

    const bool syncReady = IsSyncReadyForVsHuman(ioStatus);
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
    const DWORD now = GetTickCount();
    const bool changed =
        s_lastRoleSet != (roleSet ? 1 : 0)
        || s_lastSyncReady != (syncReady ? 1 : 0)
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
        s_lastMode = mode;
        s_lastFlag1084 = flag1084;
        s_lastSessionByte = sessionByte;
        s_lastFlag4964 = flag4964;
        s_lastFlag4965 = flag4965;
        s_lastRoleFlag = roleFlag;
        s_lastLogTick = now;
        mod::Log(
            "Takeover: PrepareVsHumanHandoff roleSet=%d syncReady=%d mode=%d flag1084=%d sessionByte=%d flags=%d/%d roleFlag=%d",
            roleSet ? 1 : 0,
            syncReady ? 1 : 0,
            mode,
            flag1084,
            sessionByte,
            flag4964,
            flag4965,
            roleFlag);
    }
    return roleSet || syncReady;
}

// Directly invoke the DLL's "Start init player" function (sub_10072880) on the
// current session object.  In the vanilla EfzRevival flow this runs automatically
// via vtable dispatch after the session is created (e.g. triggered by a mode
// transition or a flag check that our deferred-init path does not replicate).
// Without it the session's input buffers (offset +788) stay empty and the
// rollback WaitLoop deadlocks with the "too few remote inputs" message.
//
// The function performs these critical actions:
//   - Reads the Init shared memory snapshot into session+944
//   - Sets the active / queue player indices (+680, +684)
//   - Assigns the history buffer pointers (+824, +828)
//   - Seeds 22 initial BytePair inputs into the primary buffer (+788)
//   - Opens a process handle to EfzRevival.exe via the PID in the snapshot
//   - Marks the session as initialized (offset +1220 = 1)
//
// Must be called AFTER init() has created the session and the Init shared
// memory has been populated by EfzRevival.exe (ensured because we only invoke
// this after g_hostInitEvent is signaled).
bool InvokeStartInitPlayer(int initMode)
{
    if (initMode != kLocalRoleOnline)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("Takeover: InvokeStartInitPlayer skipped (DLL not loaded)");
        return false;
    }

    const uintptr_t dllBase = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t fnAddr = dllBase + kRevivalStartInitPlayerRva;

    // Read the session pointer DIRECTLY from the DLL's global variable
    // (dword_100A02CC at RVA 0xA02CC) without the heuristic validation that
    // ReadSessionPointerFromRevival() performs.  That validation can return a
    // stale / wrong pointer from a previous session or from unrelated memory
    // that happens to pass the vtable + delay/ping heuristic.  Since we only
    // call this immediately after init() stored the freshly-allocated session
    // into that global, a raw read is both correct and sufficient.
    uintptr_t sessionPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(dllBase + kRevivalSessionPtrOffsets[0]), &sessionPtr)
        || sessionPtr == 0 || sessionPtr < 0x00100000u)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer skipped (session pointer unavailable dllBase=0x%08lX raw=0x%08lX)",
            static_cast<unsigned long>(dllBase),
            static_cast<unsigned long>(sessionPtr));
        return false;
    }

    // Verify the session's initComplete flag is still 0 (un-initialized).
    // If already 1 (exactly), the function has already executed (perhaps
    // through the normal vtable path) and calling it again would reset frame
    // counters.  Any other non-zero value is garbage from a stale pointer and
    // must NOT be treated as "already initialized".
    int initComplete = -1;
    if (SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetInitComplete), &initComplete)
        && initComplete == 1)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer skipped (already initialized session=0x%08lX initComplete=%d)",
            static_cast<unsigned long>(sessionPtr),
            initComplete);
        return true;
    }

    mod::Log(
        "Takeover: InvokeStartInitPlayer calling sub_10072880 session=0x%08lX fn=0x%08lX initComplete=%d",
        static_cast<unsigned long>(sessionPtr),
        static_cast<unsigned long>(fnAddr),
        initComplete);

    // sub_10072880 is void __thiscall(int this).
    // In MSVC __thiscall: ECX = this, no explicit first parameter.
    typedef void (__thiscall *StartInitPlayerFn)(void* session);
    StartInitPlayerFn fn = reinterpret_cast<StartInitPlayerFn>(fnAddr);

    __try
    {
        fn(reinterpret_cast<void*>(sessionPtr));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer EXCEPTION session=0x%08lX fn=0x%08lX",
            static_cast<unsigned long>(sessionPtr),
            static_cast<unsigned long>(fnAddr));
        return false;
    }

    // Verify the session is now marked as initialized.
    int postInitComplete = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetInitComplete), &postInitComplete);

    int activePlayer = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetActivePlayer), &activePlayer);

    mod::Log(
        "Takeover: InvokeStartInitPlayer completed session=0x%08lX initComplete=%d activePlayer=%d",
        static_cast<unsigned long>(sessionPtr),
        postInitComplete,
        activePlayer);

    return postInitComplete == 1;
}

void StabilizeOnlineSessionBindingAfterInit(int initMode)
{
    if (initMode != kLocalRoleOnline)
    {
        return;
    }

    if (g_revivalProcessId == 0)
    {
        mod::Log("Takeover: post-init session binding skipped (helper pid unavailable)");
        return;
    }

    bool usedCachedSessionPtr = false;
    const uintptr_t sessionPtr = ReadSessionPointerForMutation(&usedCachedSessionPtr);
    if (sessionPtr == 0)
    {
        mod::Log(
            "Takeover: post-init session binding skipped (session pointer unavailable role=%d helperPid=%lu strict=0 cached=0x%08lX)",
            initMode,
            static_cast<unsigned long>(g_revivalProcessId),
            static_cast<unsigned long>(g_lastValidatedSessionPtr));
        return;
    }

    const char* const sessionPtrSource = usedCachedSessionPtr ? "cached" : "strict";

    int helperPidField = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetHelperPid), &helperPidField);

    uintptr_t helperHandleFieldRaw = 0;
    (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetHelperHandle), &helperHandleFieldRaw);
    HANDLE helperHandleField = reinterpret_cast<HANDLE>(helperHandleFieldRaw);

    DWORD helperHandlePid = 0;
    DWORD helperHandleExitCode = 0xFFFFFFFFu;
    if (helperHandleField != nullptr && helperHandleField != INVALID_HANDLE_VALUE)
    {
        helperHandlePid = GetProcessId(helperHandleField);
        if (!GetExitCodeProcess(helperHandleField, &helperHandleExitCode))
        {
            helperHandleExitCode = 0xFFFFFFFEu;
        }
    }
    const DWORD expectedPid = g_revivalProcessId;
    const bool pidMatches = helperPidField == static_cast<int>(expectedPid);
    const bool handleMatches =
        helperHandleField != nullptr
        && helperHandleField != INVALID_HANDLE_VALUE
        && helperHandlePid == expectedPid;

    bool patchedPid = false;
    bool patchedHandle = false;
    HANDLE replacementHandle = nullptr;
    bool replacementIsOwnedHandle = false;

    if (!pidMatches || !handleMatches)
    {
        const void* const pidFieldAddress = reinterpret_cast<const void*>(sessionPtr + kSessionOffsetHelperPid);
        const void* const handleFieldAddress = reinterpret_cast<const void*>(sessionPtr + kSessionOffsetHelperHandle);
        if (!IsWritableRange(const_cast<void*>(pidFieldAddress), sizeof(int))
            || !IsWritableRange(const_cast<void*>(handleFieldAddress), sizeof(uintptr_t)))
        {
            mod::Log(
                "Takeover: post-init session binding skipped (fields not writable session=0x%08lX pidWritable=%d handleWritable=%d)",
                static_cast<unsigned long>(sessionPtr),
                IsWritableRange(const_cast<void*>(pidFieldAddress), sizeof(int)) ? 1 : 0,
                IsWritableRange(const_cast<void*>(handleFieldAddress), sizeof(uintptr_t)) ? 1 : 0);
            return;
        }

        if (!handleMatches)
        {
            replacementHandle = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, expectedPid);
            replacementIsOwnedHandle = (replacementHandle != nullptr);
            if (replacementHandle == nullptr && g_revivalProcess != nullptr)
            {
                const DWORD hostHandlePid = GetProcessId(g_revivalProcess);
                if (hostHandlePid == expectedPid)
                {
                    HANDLE duplicated = nullptr;
                    if (DuplicateHandle(
                            GetCurrentProcess(),
                            g_revivalProcess,
                            GetCurrentProcess(),
                            &duplicated,
                            PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
                            FALSE,
                            0))
                    {
                        replacementHandle = duplicated;
                        replacementIsOwnedHandle = true;
                    }
                    else
                    {
                        replacementHandle = g_revivalProcess;
                        replacementIsOwnedHandle = false;
                    }
                }
            }
        }

        __try
        {
            if (!pidMatches)
            {
                *reinterpret_cast<int*>(const_cast<void*>(pidFieldAddress)) = static_cast<int>(expectedPid);
                patchedPid = true;
            }
            if (!handleMatches && replacementHandle != nullptr)
            {
                *reinterpret_cast<uintptr_t*>(const_cast<void*>(handleFieldAddress)) = reinterpret_cast<uintptr_t>(replacementHandle);
                patchedHandle = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (replacementIsOwnedHandle && replacementHandle != nullptr)
            {
                CloseHandle(replacementHandle);
            }
            mod::Log("Takeover: post-init session binding patch raised exception");
            return;
        }

        if (patchedHandle
            && helperHandleField != nullptr
            && helperHandleField != INVALID_HANDLE_VALUE
            && helperHandleField != g_revivalProcess
            && helperHandleField != replacementHandle)
        {
            CloseHandle(helperHandleField);
        }
        else if (!patchedHandle && replacementIsOwnedHandle && replacementHandle != nullptr)
        {
            CloseHandle(replacementHandle);
        }

        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetHelperPid), &helperPidField);
        helperHandleFieldRaw = 0;
        (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetHelperHandle), &helperHandleFieldRaw);
        helperHandleField = reinterpret_cast<HANDLE>(helperHandleFieldRaw);
        helperHandlePid = 0;
        helperHandleExitCode = 0xFFFFFFFFu;
        if (helperHandleField != nullptr && helperHandleField != INVALID_HANDLE_VALUE)
        {
            helperHandlePid = GetProcessId(helperHandleField);
            if (!GetExitCodeProcess(helperHandleField, &helperHandleExitCode))
            {
                helperHandleExitCode = 0xFFFFFFFEu;
            }
        }
    }

    mod::Log(
        "Takeover: post-init session binding mode=%d source=%s session=0x%08lX pidField=%d expectedPid=%lu handle=0x%08lX handlePid=%lu handleExit=%ld patched(pid=%d handle=%d)",
        initMode,
        sessionPtrSource,
        static_cast<unsigned long>(sessionPtr),
        helperPidField,
        static_cast<unsigned long>(expectedPid),
        static_cast<unsigned long>(helperHandleFieldRaw),
        static_cast<unsigned long>(helperHandlePid),
        static_cast<long>(helperHandleExitCode),
        patchedPid ? 1 : 0,
        patchedHandle ? 1 : 0);
}

void RepairRollbackHistoryBindingsIfNeeded()
{
    if (!g_localInitAppliedForSession)
    {
        return;
    }

    bool usedCachedSessionPtr = false;
    const uintptr_t sessionPtr = ReadSessionPointerForMutation(&usedCachedSessionPtr);
    if (sessionPtr == 0)
    {
        return;
    }
    const char* const sessionPtrSource = usedCachedSessionPtr ? "cached" : "strict";

    int activePlayer = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetActivePlayer), &activePlayer))
    {
        return;
    }
    if (activePlayer != 0 && activePlayer != 1)
    {
        return;
    }

    int queuePlayer = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetQueuePlayer), &queuePlayer);

    uintptr_t historyPrimaryPtr = 0;
    uintptr_t historySecondaryPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetHistoryPrimaryPtr), &historyPrimaryPtr)
        || !SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetHistorySecondaryPtr), &historySecondaryPtr))
    {
        return;
    }

    const uintptr_t expectedPrimary =
        sessionPtr + ((activePlayer == 0) ? kSessionOffsetHistoryPrimaryVec : kSessionOffsetHistorySecondaryVec);
    const uintptr_t expectedSecondary =
        sessionPtr + ((activePlayer == 0) ? kSessionOffsetHistorySecondaryVec : kSessionOffsetHistoryPrimaryVec);
    const int expectedQueuePlayer = (activePlayer == 0) ? 1 : 0;

    const bool queuePlayerInvalid = (queuePlayer != 0 && queuePlayer != 1) || queuePlayer != expectedQueuePlayer;
    const bool needsRepair =
        (historyPrimaryPtr != expectedPrimary)
        || (historySecondaryPtr != expectedSecondary)
        || queuePlayerInvalid;
    if (!needsRepair)
    {
        return;
    }

    void* const primaryField = reinterpret_cast<void*>(sessionPtr + kSessionOffsetHistoryPrimaryPtr);
    void* const secondaryField = reinterpret_cast<void*>(sessionPtr + kSessionOffsetHistorySecondaryPtr);
    void* const queueField = reinterpret_cast<void*>(sessionPtr + kSessionOffsetQueuePlayer);
    if (!IsWritableRange(primaryField, sizeof(uintptr_t))
        || !IsWritableRange(secondaryField, sizeof(uintptr_t))
        || !IsWritableRange(queueField, sizeof(int)))
    {
        const LONG hits = InterlockedIncrement(&g_sessionHistoryRepairHits);
        if (hits <= 8 || (hits % 64) == 0)
        {
            mod::Log(
                "Takeover: skipped history binding repair source=%s session=0x%08lX writable=%d/%d/%d count=%ld",
                sessionPtrSource,
                static_cast<unsigned long>(sessionPtr),
                IsWritableRange(primaryField, sizeof(uintptr_t)) ? 1 : 0,
                IsWritableRange(secondaryField, sizeof(uintptr_t)) ? 1 : 0,
                IsWritableRange(queueField, sizeof(int)) ? 1 : 0,
                static_cast<long>(hits));
        }
        return;
    }

    __try
    {
        *reinterpret_cast<uintptr_t*>(primaryField) = expectedPrimary;
        *reinterpret_cast<uintptr_t*>(secondaryField) = expectedSecondary;
        *reinterpret_cast<int*>(queueField) = expectedQueuePlayer;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        const LONG hits = InterlockedIncrement(&g_sessionHistoryRepairHits);
        if (hits <= 8 || (hits % 64) == 0)
        {
            mod::Log(
                "Takeover: exception while repairing history bindings source=%s session=0x%08lX count=%ld",
                sessionPtrSource,
                static_cast<unsigned long>(sessionPtr),
                static_cast<long>(hits));
        }
        return;
    }

    const LONG hits = InterlockedIncrement(&g_sessionHistoryRepairHits);
    if (hits <= 8 || (hits % 64) == 0)
    {
        mod::Log(
            "Takeover: repaired history bindings source=%s session=0x%08lX active=%d queue=%d->%d hist=0x%08lX/0x%08lX -> 0x%08lX/0x%08lX count=%ld",
            sessionPtrSource,
            static_cast<unsigned long>(sessionPtr),
            activePlayer,
            queuePlayer,
            expectedQueuePlayer,
            static_cast<unsigned long>(historyPrimaryPtr),
            static_cast<unsigned long>(historySecondaryPtr),
            static_cast<unsigned long>(expectedPrimary),
            static_cast<unsigned long>(expectedSecondary),
            static_cast<long>(hits));
    }
}

void Tick(NetbridgeStatus* ioStatus, uint32_t* ioConnectStartTick)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ioStatus == nullptr)
    {
        return;
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(ioStatus->phase);
    if (phase != NetbridgePhase::Connecting
        && phase != NetbridgePhase::DelaySetup
        && phase != NetbridgePhase::Connected)
    {
        RefreshRuntimeStatus(ioStatus);
        return;
    }

    if (!ProcessAlive(ioStatus))
    {
        RefreshRuntimeStatus(ioStatus);
        const bool runtimeReady = HasRuntimeReadySignal(ioStatus);
        if (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
        {
            if (g_localInitAppliedForSession)
            {
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
            if (!runtimeReady)
            {
                SetPhase(ioStatus, NetbridgePhase::SessionEnded, nullptr);
                ReinitLocalPlay();
            }
            else
            {
                mod::Log("Takeover: helper process exited but runtime session still active");
            }
        }
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
        }

        if (!g_observedTakeoverCreatePath
            && g_revivalProcess != nullptr
            && g_revivalProcessId != 0
            && g_remoteInjectedSelfBase != 0
            && (g_lastLatePatchRetryTick == 0 || now - g_lastLatePatchRetryTick >= 500))
        {
            const std::unordered_map<std::string, uint32_t> patches = BuildPatchMap(g_remoteInjectedSelfBase);
            const bool patchedLate = PatchIat(g_revivalProcess, g_revivalProcessId, patches, false);
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
                (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetPingMs), &pingMs);
                (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSessionOffsetInputDelay), &delayFrames);
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
        }
    }

    if ((phase == NetbridgePhase::Connecting || phase == NetbridgePhase::DelaySetup)
        && !g_localInitAppliedForSession
        && g_hostInitEvent != nullptr
        && g_hostBlock != nullptr
        && g_localInitFn != nullptr)
    {
        const DWORD initReady = WaitForSingleObject(g_hostInitEvent, 0);
        if (initReady == WAIT_OBJECT_0)
        {
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

            const int initResult = g_localInitFn(initParams);
            g_localRoleFlag = initParams[0];
            g_localInitAppliedForSession = true;
            mod::Log(
                "Takeover: local init(mode=%d magic=%d) result=%d [deferred]",
                initParams[0],
                initParams[1],
                initResult);

            // The DLL's init() creates a new session object and installs the
            // main-loop hook, but does NOT run the "Start init player" routine
            // (sub_10072880) that reads the Init shared-memory snapshot, seeds
            // the 22 initial inputs, and sets up the active/queue player.
            // In vanilla EfzRevival this routine fires automatically through a
            // vtable/mode-transition mechanism that our deferred-init path
            // bypasses entirely.  Calling it explicitly here fixes the
            // "Pause, too few remote inputs" deadlock caused by empty input
            // buffers (offset +788 = 0).
            const bool startInitOk = InvokeStartInitPlayer(initParams[0]);
            mod::Log(
                "Takeover: InvokeStartInitPlayer result=%d [deferred]",
                startInitOk ? 1 : 0);

            StabilizeOnlineSessionBindingAfterInit(initParams[0]);
        }
    }

    if (g_localInitAppliedForSession)
    {
        RepairRollbackHistoryBindingsIfNeeded();
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
    }
}

void CancelSession(const char* reason, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    InterlockedExchange(&g_startAbortRequested, 1);
    FlushPendingConsoleOutput("cancel");

    const bool hadProcess = ProcessAlive(ioStatus);
    if (hadProcess && g_revivalProcess != nullptr)
    {
        TerminateProcess(g_revivalProcess, 0);
    }
    CloseProcessHandle(ioStatus);
    ReinitLocalPlay();
    g_localInitAppliedForSession = false;
    InterlockedExchange(&g_injectedDelayPromptSerial, 0);
    InterlockedExchange(&g_injectedDelayPromptServedSerial, 0);
    InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, 0);
    g_injectedDelayPromptWaitStartTick = 0;
    g_delayPromptMetrics = {};
    ResetNativeWorkflowFlags();
    g_lastConnectingDiagnosticTick = 0;
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
        g_diskCapturePathHits.clear();
    }

    if (ioStatus != nullptr)
    {
        if (reason != nullptr && (std::strcmp(reason, "user_cancel") == 0 || std::strcmp(reason, "leave_menu") == 0 || std::strcmp(reason, "external_cancel") == 0))
        {
            SetPhase(ioStatus, NetbridgePhase::Idle, nullptr);
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

constexpr DWORD kRedirectProcessAccess =
    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE;

bool ContainsInsensitiveAscii(const char* haystack, const char* needle)
{
    if (haystack == nullptr || needle == nullptr || needle[0] == '\0')
    {
        return false;
    }

    const size_t needleLen = std::strlen(needle);
    for (size_t i = 0; haystack[i] != '\0'; ++i)
    {
        size_t j = 0;
        while (j < needleLen && haystack[i + j] != '\0')
        {
            const unsigned char lhs = static_cast<unsigned char>(haystack[i + j]);
            const unsigned char rhs = static_cast<unsigned char>(needle[j]);
            if (std::tolower(lhs) != std::tolower(rhs))
            {
                break;
            }
            ++j;
        }
        if (j == needleLen)
        {
            return true;
        }
    }

    return false;
}

bool ShouldRedirectCreateProcessA(LPCSTR lpApplicationName, LPCSTR lpCommandLine)
{
    return ContainsInsensitiveAscii(lpApplicationName, "efz.exe")
        || ContainsInsensitiveAscii(lpCommandLine, "efz.exe");
}

enum class RedirectHostSource
{
    InjectedBlock,
    TempIpc,
};

bool TryOpenHostProcessByPid(uint32_t hostPid, HANDLE* outProcessHandle)
{
    if (hostPid == 0 || outProcessHandle == nullptr)
    {
        return false;
    }

    const HANDLE processHandle = OpenProcess(kRedirectProcessAccess, FALSE, hostPid);
    if (processHandle == nullptr)
    {
        return false;
    }

    *outProcessHandle = processHandle;
    return true;
}

bool ResolveHostProcessForRedirect(HANDLE* outProcessHandle, uint32_t* outHostPid, RedirectHostSource* outSource)
{
    if (outProcessHandle == nullptr || outHostPid == nullptr)
    {
        return false;
    }

    *outProcessHandle = nullptr;
    *outHostPid = 0;

    if (g_injectedBlock != nullptr)
    {
        if (TryOpenHostProcessByPid(g_injectedBlock->hostPid, outProcessHandle))
        {
            *outHostPid = g_injectedBlock->hostPid;
            if (outSource != nullptr)
            {
                *outSource = RedirectHostSource::InjectedBlock;
            }
            return true;
        }
    }

    TempIpcContext temp = {};
    if (!OpenTempIpcContext(&temp, false, false) || temp.block == nullptr)
    {
        CloseTempIpcContext(&temp);
        return false;
    }

    InterlockedIncrement(&temp.block->dbgCreateProcessHits);
    const uint32_t tempHostPid = temp.block->hostPid;
    const bool opened = TryOpenHostProcessByPid(tempHostPid, outProcessHandle);
    CloseTempIpcContext(&temp);
    if (!opened)
    {
        return false;
    }

    if (g_injectedBlock != nullptr && g_injectedBlock->hostPid != tempHostPid)
    {
        g_injectedBlock->hostPid = tempHostPid;
    }

    *outHostPid = tempHostPid;
    if (outSource != nullptr)
    {
        *outSource = RedirectHostSource::TempIpc;
    }
    return true;
}

BOOL StubCreateProcessA(
    LPCSTR lpApplicationName,
    LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation)
{
    if (g_injectedBlock != nullptr)
    {
        InterlockedIncrement(&g_injectedBlock->dbgCreateProcessHits);
    }

    if (!ShouldRedirectCreateProcessA(lpApplicationName, lpCommandLine))
    {
        return CreateProcessA(
            lpApplicationName,
            lpCommandLine,
            lpProcessAttributes,
            lpThreadAttributes,
            bInheritHandles,
            dwCreationFlags,
            lpEnvironment,
            lpCurrentDirectory,
            lpStartupInfo,
            lpProcessInformation);
    }

    if (lpProcessInformation == nullptr)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        mod::Log("nb_stub_CreateProcessA: redirect rejected (null PROCESS_INFORMATION)");
        return FALSE;
    }

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }

    HANDLE processHandle = nullptr;
    uint32_t hostPid = 0;
    RedirectHostSource source = RedirectHostSource::InjectedBlock;
    if (!ResolveHostProcessForRedirect(&processHandle, &hostPid, &source))
    {
        const uint32_t injectedHostPid = (g_injectedBlock != nullptr) ? g_injectedBlock->hostPid : 0u;
        mod::Log(
            "nb_stub_CreateProcessA: OpenProcess failed injectedHostPid=%lu err=%s",
            static_cast<unsigned long>(injectedHostPid),
            ErrorString(GetLastError()).c_str());
        return FALSE;
    }

    HANDLE threadHandle = CreateEventA(nullptr, TRUE, TRUE, nullptr);
    if (threadHandle == nullptr)
    {
        mod::Log("nb_stub_CreateProcessA: failed to create fake process thread handle: %s", ErrorString(GetLastError()).c_str());
        CloseHandle(processHandle);
        return FALSE;
    }

    g_fakeProcessThreadHandle = threadHandle;

    std::memset(lpProcessInformation, 0, sizeof(PROCESS_INFORMATION));
    lpProcessInformation->hProcess = processHandle;
    lpProcessInformation->hThread = threadHandle;
    lpProcessInformation->dwProcessId = hostPid;
    lpProcessInformation->dwThreadId = GetCurrentThreadId();
    mod::Log(
        "nb_stub_CreateProcessA: redirected to host pid=%lu source=%s",
        static_cast<unsigned long>(lpProcessInformation->dwProcessId),
        (source == RedirectHostSource::TempIpc) ? "temp_ipc" : "injected");
    return TRUE;
}

BOOL StubReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(lpBaseAddress);
    if (address == 0x787237)
    {
        if (lpBuffer != nullptr && nSize > 0)
        {
            std::memset(lpBuffer, 0, nSize);
        }
        if (lpNumberOfBytesRead != nullptr)
        {
            *lpNumberOfBytesRead = nSize;
        }
        return TRUE;
    }

    if (address == 0x7871F4)
    {
        std::array<uint8_t, 4> bytes = {
            static_cast<uint8_t>(kEfzFingerprint & 0xFF),
            static_cast<uint8_t>((kEfzFingerprint >> 8) & 0xFF),
            static_cast<uint8_t>((kEfzFingerprint >> 16) & 0xFF),
            static_cast<uint8_t>((kEfzFingerprint >> 24) & 0xFF),
        };
        const SIZE_T copy = (std::min)(nSize, static_cast<SIZE_T>(bytes.size()));
        if (lpBuffer != nullptr && copy > 0)
        {
            std::memcpy(lpBuffer, bytes.data(), copy);
        }
        if (lpNumberOfBytesRead != nullptr)
        {
            *lpNumberOfBytesRead = copy;
        }
        const LONG hits = InterlockedIncrement(&g_injectedFingerprintReadHits);
        if (hits <= 8 || (hits % 64) == 0)
        {
            mod::Log(
                "nb_stub_ReadProcessMemory: spoof addr=0x%08lX size=%zu value=0x%08lX count=%ld",
                static_cast<unsigned long>(address),
                static_cast<size_t>(nSize),
                static_cast<unsigned long>(kEfzFingerprint),
                static_cast<long>(hits));
        }
        return TRUE;
    }

    return ReadProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead);
}

LPVOID StubVirtualAllocEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (!HasInjectedContext())
    {
        LPVOID local = VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
        if (local == nullptr)
        {
            mod::Log("nb_stub_VirtualAllocEx: fallback VirtualAlloc failed size=%zu err=%s", static_cast<size_t>(dwSize), ErrorString(GetLastError()).c_str());
        }
        else
        {
            RegisterRedirectAllocation(local, dwSize);
            mod::Log("nb_stub_VirtualAllocEx: fallback local alloc size=%zu addr=0x%p", static_cast<size_t>(dwSize), local);
        }
        return local;
    }

    const LPVOID local = VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
    if (local == nullptr)
    {
        mod::Log("nb_stub_VirtualAllocEx: VirtualAlloc failed size=%zu err=%s", static_cast<size_t>(dwSize), ErrorString(GetLastError()).c_str());
    }
    else
    {
        RegisterRedirectAllocation(local, dwSize);
    }
    return local;
}

BOOL StubVirtualFreeEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (!HasInjectedContext())
    {
        if (lpAddress == nullptr)
        {
            return TRUE;
        }
        DWORD freeType = dwFreeType == 0 ? MEM_RELEASE : dwFreeType;
        SIZE_T freeSize = (freeType == MEM_RELEASE) ? 0 : dwSize;
        BOOL ok = VirtualFree(lpAddress, freeSize, freeType);
        if (ok == FALSE && freeType == MEM_RELEASE)
        {
            ok = VirtualFree(lpAddress, 0, MEM_RELEASE);
        }
        if (ok != FALSE && freeType == MEM_RELEASE)
        {
            ForgetRedirectAllocation(lpAddress);
        }
        return ok;
    }

    if (lpAddress == nullptr)
    {
        return TRUE;
    }

    DWORD freeType = dwFreeType;
    SIZE_T freeSize = dwSize;
    if (freeType == 0)
    {
        freeType = MEM_RELEASE;
        freeSize = 0;
    }

    BOOL ok = VirtualFree(lpAddress, freeSize, freeType);
    if (ok == FALSE && freeType == MEM_RELEASE && freeSize != 0)
    {
        ok = VirtualFree(lpAddress, 0, MEM_RELEASE);
    }
    if (ok != FALSE && freeType == MEM_RELEASE)
    {
        ForgetRedirectAllocation(lpAddress);
    }
    return ok;
}

BOOL StubWriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesWritten)
{
    if (g_injectedBlock != nullptr)
    {
        InterlockedIncrement(&g_injectedBlock->dbgWriteProcessHits);
    }

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (!HasInjectedContext())
    {
        bool copied = true;
        if (lpBaseAddress != nullptr && lpBuffer != nullptr && nSize > 0)
        {
            if (IsWithinRedirectAllocation(lpBaseAddress, nSize) && IsWritableRange(lpBaseAddress, nSize))
            {
                std::memcpy(lpBaseAddress, lpBuffer, nSize);
            }
            else
            {
                copied = false;
                SetLastError(ERROR_NOACCESS);
                const LONG blockedHits = InterlockedIncrement(&g_redirectWriteBlockedHits);
                if (blockedHits <= 8 || (blockedHits % 64) == 0)
                {
                    mod::Log(
                        "nb_stub_WriteProcessMemory: fallback write blocked addr=0x%p size=%zu tracked=%d count=%ld",
                        lpBaseAddress,
                        static_cast<size_t>(nSize),
                        IsWithinRedirectAllocation(lpBaseAddress, nSize) ? 1 : 0,
                        static_cast<long>(blockedHits));
                }
            }
        }
        if (lpNumberOfBytesWritten != nullptr)
        {
            *lpNumberOfBytesWritten = copied ? nSize : 0;
        }

        if (nSize == sizeof(int) * 2 && lpBuffer != nullptr)
        {
            const int* vals = reinterpret_cast<const int*>(lpBuffer);
            TempIpcContext temp = {};
            if (OpenTempIpcContext(&temp, true, false) && temp.block != nullptr)
            {
                InterlockedIncrement(&temp.block->dbgWriteProcessHits);
                if (vals[1] == 102 && vals[0] >= 0 && vals[0] <= 3)
                {
                    temp.block->initParams[0] = vals[0];
                    temp.block->initParams[1] = vals[1];
                    InterlockedIncrement(&temp.block->initSerial);
                    if (temp.initEvent != nullptr)
                    {
                        SetEvent(temp.initEvent);
                    }
                    mod::Log(
                        "nb_stub_WriteProcessMemory: fallback captured init params mode=%d magic=%d",
                        vals[0],
                        vals[1]);
                }
            }
            CloseTempIpcContext(&temp);
        }
        else
        {
            TempIpcContext temp = {};
            if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
            {
                InterlockedIncrement(&temp.block->dbgWriteProcessHits);
            }
            CloseTempIpcContext(&temp);
        }
        return copied ? TRUE : FALSE;
    }

    bool copied = true;
    if (lpBaseAddress != nullptr && lpBuffer != nullptr && nSize > 0)
    {
        if (IsWithinRedirectAllocation(lpBaseAddress, nSize) && IsWritableRange(lpBaseAddress, nSize))
        {
            std::memcpy(lpBaseAddress, lpBuffer, nSize);
        }
        else
        {
            copied = false;
            SetLastError(ERROR_NOACCESS);
            const LONG blockedHits = InterlockedIncrement(&g_redirectWriteBlockedHits);
            if (blockedHits <= 8 || (blockedHits % 64) == 0)
            {
                mod::Log(
                    "nb_stub_WriteProcessMemory: write blocked addr=0x%p size=%zu tracked=%d count=%ld",
                    lpBaseAddress,
                    static_cast<size_t>(nSize),
                    IsWithinRedirectAllocation(lpBaseAddress, nSize) ? 1 : 0,
                    static_cast<long>(blockedHits));
            }
        }
    }

    if (lpNumberOfBytesWritten != nullptr)
    {
        *lpNumberOfBytesWritten = copied ? nSize : 0;
    }

    if (nSize == sizeof(int) * 2)
    {
        const int* vals = reinterpret_cast<const int*>(lpBuffer);
        mod::Log("nb_stub_WriteProcessMemory: observed init write mode=%d magic=%d dst=0x%p", vals[0], vals[1], lpBaseAddress);

        if (!g_initCapturedFromWrite
            && vals[1] == 102
            && vals[0] >= 0
            && vals[0] <= 3
            && g_injectedBlock != nullptr
            && g_injectedInitEvent != nullptr)
        {
            g_injectedBlock->initParams[0] = vals[0];
            g_injectedBlock->initParams[1] = vals[1];
            InterlockedIncrement(&g_injectedBlock->initSerial);
            SetEvent(g_injectedInitEvent);
            g_initCapturedFromWrite = true;
            mod::Log(
                "nb_stub_WriteProcessMemory: captured init params via write mode=%d magic=%d",
                vals[0],
                vals[1]);
        }
    }

    return copied ? TRUE : FALSE;
}

uintptr_t ResolveInjectedInitAddress()
{
    if (g_injectedInitAddress != 0)
    {
        return g_injectedInitAddress;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return 0;
    }

    const FARPROC initProc = GetProcAddress(revival, "init");
    g_injectedInitAddress = reinterpret_cast<uintptr_t>(initProc);
    if (g_injectedInitAddress != 0)
    {
        mod::Log("Takeover: resolved injected init address=0x%p", reinterpret_cast<void*>(g_injectedInitAddress));
    }
    return g_injectedInitAddress;
}

bool IsLikelyInitThreadCall(LPTHREAD_START_ROUTINE lpStartAddress, LONG callIndex)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(lpStartAddress);
    const uintptr_t initAddress = ResolveInjectedInitAddress();
    if (initAddress != 0 && start == initAddress)
    {
        return true;
    }

    if (initAddress != 0)
    {
        const uintptr_t localBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("EfzRevival.dll"));
        if (localBase != 0 && initAddress >= localBase)
        {
            const uintptr_t initRva = initAddress - localBase;
            uintptr_t remoteBase = ResolveInjectedExpectedRevivalBase();
            if (remoteBase == 0)
            {
                remoteBase = static_cast<uintptr_t>(kDefaultRevivalImageBase);
            }
            const uintptr_t expectedRemoteInit = remoteBase + initRva;
            if (start == expectedRemoteInit)
            {
                return true;
            }
        }
    }

    // Fallback for builds where init cannot be resolved yet.
    return callIndex >= 2;
}

bool TryReadRemoteInitParams(LPVOID lpParameter, int* outMode, int* outMagic)
{
    if (lpParameter == nullptr || outMode == nullptr || outMagic == nullptr)
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(lpParameter, &mbi, sizeof(mbi)) == 0)
    {
        return false;
    }
    if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS)
    {
        return false;
    }

    const DWORD protect = mbi.Protect & 0xFF;
    const bool readable =
        protect == PAGE_READONLY ||
        protect == PAGE_READWRITE ||
        protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READ ||
        protect == PAGE_EXECUTE_READWRITE ||
        protect == PAGE_EXECUTE_WRITECOPY;
    if (!readable)
    {
        return false;
    }

    int params[2] = {0, 0};
    std::memcpy(params, lpParameter, sizeof(params));
    *outMode = params[0];
    *outMagic = params[1];
    return true;
}

HANDLE StubCreateRemoteThread(HANDLE hProcess, LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId)
{
    (void)hProcess;
    (void)lpThreadAttributes;
    (void)dwStackSize;
    (void)dwCreationFlags;

    if (lpThreadId != nullptr)
    {
        *lpThreadId = GetCurrentThreadId();
    }

    if (g_injectedBlock != nullptr)
    {
        InterlockedIncrement(&g_injectedBlock->dbgCreateRemoteThreadHits);
    }

    const uintptr_t expectedRemoteBase = [&]() -> uintptr_t {
        uintptr_t base = ResolveInjectedExpectedRevivalBase();
        if (base == 0)
        {
            base = ResolveHostRevivalBase();
        }
        if (base == 0)
        {
            base = static_cast<uintptr_t>(kDefaultRevivalImageBase);
        }
        return base;
    }();

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (!HasInjectedContext())
    {
        TempIpcContext temp = {};
        const LONG callIndexFallback = InterlockedIncrement(&g_remoteThreadCallIndex);
        int paramMode = 0;
        int paramMagic = 0;
        const bool hasParamHint = TryReadRemoteInitParams(lpParameter, &paramMode, &paramMagic);
        const bool paramLooksLikeInit = hasParamHint && paramMagic == 102 && paramMode >= 0 && paramMode <= 3;

        if (OpenTempIpcContext(&temp, true, false) && temp.block != nullptr)
        {
            InterlockedIncrement(&temp.block->dbgCreateRemoteThreadHits);
            if (paramLooksLikeInit && temp.initEvent != nullptr)
            {
                temp.block->initParams[0] = paramMode;
                temp.block->initParams[1] = paramMagic;
                InterlockedIncrement(&temp.block->initSerial);
                SetEvent(temp.initEvent);
                mod::Log(
                    "nb_stub_CreateRemoteThread: fallback captured init params mode=%d magic=%d start=0x%p call=%ld",
                    paramMode,
                    paramMagic,
                    reinterpret_cast<void*>(lpStartAddress),
                    static_cast<long>(callIndexFallback));
                CloseTempIpcContext(&temp);
                return CreateFakeThread(1);
            }
        }
        CloseTempIpcContext(&temp);

        mod::Log("nb_stub_CreateRemoteThread: passthrough (ready=%d start=0x%p)", HasInjectedContext() ? 1 : 0, reinterpret_cast<void*>(lpStartAddress));
        return CreateRemoteThread(hProcess, lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
    }

    const LONG callIndex = InterlockedIncrement(&g_remoteThreadCallIndex);
    int paramMode = 0;
    int paramMagic = 0;
    const bool hasParamHint = TryReadRemoteInitParams(lpParameter, &paramMode, &paramMagic);
    const bool paramLooksLikeInit = hasParamHint && paramMagic == 102 && paramMode >= 0 && paramMode <= 3;
    const bool initCall = paramLooksLikeInit || IsLikelyInitThreadCall(lpStartAddress, callIndex);
    DWORD fakeExitCode = static_cast<DWORD>(expectedRemoteBase);

    if (initCall)
    {
        int params[2] = {0, 102};
        if (hasParamHint)
        {
            params[0] = paramMode;
            params[1] = paramMagic;
        }
        else if (lpParameter != nullptr)
        {
            std::memcpy(params, lpParameter, sizeof(params));
        }
        g_injectedBlock->initParams[0] = params[0];
        g_injectedBlock->initParams[1] = params[1];
        InterlockedIncrement(&g_injectedBlock->initSerial);
        SetEvent(g_injectedInitEvent);
        fakeExitCode = 1;
        mod::Log(
            "nb_stub_CreateRemoteThread: captured init params mode=%d magic=%d start=0x%p call=%ld",
            params[0],
            params[1],
            reinterpret_cast<void*>(lpStartAddress),
            static_cast<long>(callIndex));
    }
    else
    {
        mod::Log(
            "nb_stub_CreateRemoteThread: bypass call start=0x%p call=%ld base=0x%08lX",
            reinterpret_cast<void*>(lpStartAddress),
            static_cast<long>(callIndex),
            static_cast<unsigned long>(fakeExitCode));
    }

    return CreateFakeThread(fakeExitCode);
}

BOOL StubTerminateProcess(HANDLE hProcess, UINT uExitCode)
{
    if (hProcess == nullptr)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }

    uint32_t hostPid = 0;
    if (g_injectedBlock != nullptr)
    {
        hostPid = g_injectedBlock->hostPid;
    }

    if (hostPid != 0)
    {
        const DWORD targetPid = GetProcessId(hProcess);
        if (targetPid == 0)
        {
            const LONG hits = InterlockedIncrement(&g_injectedTerminateUnknownPidHits);
            if (hits <= 8 || (hits % 64) == 0)
            {
                const DWORD pidError = GetLastError();
                mod::Log(
                    "nb_stub_TerminateProcess: unresolved target pid hostPid=%lu handle=0x%p err=%s (blocked) count=%ld",
                    static_cast<unsigned long>(hostPid),
                    hProcess,
                    ErrorString(pidError).c_str(),
                    static_cast<long>(hits));
            }
            return TRUE;
        }
        if (targetPid == hostPid)
        {
            mod::Log(
                "nb_stub_TerminateProcess: blocked host termination pid=%lu exit=%u",
                static_cast<unsigned long>(targetPid),
                static_cast<unsigned>(uExitCode));
            return TRUE;
        }
    }

    return TerminateProcess(hProcess, uExitCode);
}

HANDLE StubOpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId)
{
    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }

    uint32_t hostPid = 0;
    if (g_injectedBlock != nullptr)
    {
        hostPid = g_injectedBlock->hostPid;
    }

    DWORD requestedAccess = dwDesiredAccess;
    if (hostPid != 0 && dwProcessId == hostPid && (requestedAccess & PROCESS_TERMINATE) != 0)
    {
        requestedAccess &= ~PROCESS_TERMINATE;
        mod::Log(
            "nb_stub_OpenProcess: stripped PROCESS_TERMINATE for host pid=%lu requested=0x%08lX effective=0x%08lX",
            static_cast<unsigned long>(dwProcessId),
            static_cast<unsigned long>(dwDesiredAccess),
            static_cast<unsigned long>(requestedAccess));
    }

    return OpenProcess(requestedAccess, bInheritHandle, dwProcessId);
}

BOOL StubReadConsoleA(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl)
{
    (void)hConsoleInput;
    (void)pInputControl;
    FlushPendingConsoleOutput("before_ReadConsoleA");

    auto copyInputOut = [&](const char* input) -> BOOL {
        if (input == nullptr || input[0] == '\0')
        {
            input = "2\n";
        }
        const size_t len = std::strlen(input);
        const DWORD copy = static_cast<DWORD>((std::min)(static_cast<size_t>(nNumberOfCharsToRead), len));
        if (lpBuffer != nullptr && copy > 0)
        {
            std::memcpy(lpBuffer, input, copy);
            if (copy < nNumberOfCharsToRead)
            {
                reinterpret_cast<char*>(lpBuffer)[copy] = '\0';
            }
        }
        if (lpNumberOfCharsRead != nullptr)
        {
            *lpNumberOfCharsRead = copy;
        }
        return TRUE;
    };

    auto serveScriptedInput = [&](SharedBlock* block, const char* input, const char* sourceTag, const char* reason, LONG serial, bool autoInput) -> BOOL {
        if (block != nullptr)
        {
            InterlockedIncrement(&block->dbgReadConsoleHits);
            if (autoInput)
            {
                InterlockedIncrement(&block->dbgReadConsoleAutoHits);
            }
        }

        const BOOL ok = copyInputOut(input);
        if (autoInput)
        {
            const LONG fallbackCount = InterlockedIncrement(&g_injectedAutoConsoleFallbackCount);
            if (fallbackCount <= 8 || (fallbackCount % 64) == 0)
            {
                mod::Log(
                    "nb_stub_ReadConsoleA: auto input serial=%ld source=%s reason=%s read=%lu value='%s' count=%ld",
                    static_cast<long>(serial),
                    sourceTag != nullptr ? sourceTag : "unknown",
                    reason != nullptr ? reason : "",
                    static_cast<unsigned long>(nNumberOfCharsToRead),
                    (input != nullptr) ? input : "",
                    static_cast<long>(fallbackCount));
            }
        }
        else
        {
            mod::Log(
                "nb_stub_ReadConsoleA: served input serial=%ld source=%s value='%s'",
                static_cast<long>(serial),
                sourceTag != nullptr ? sourceTag : "unknown",
                (input != nullptr) ? input : "");
        }
        return ok;
    };

    auto waitAndServe = [&](SharedBlock* block, HANDLE consoleEvent, const char* sourceTag) -> BOOL {
        if (block == nullptr)
        {
            return FALSE;
        }

        for (;;)
        {
            const LONG serial = block->consoleSerial;
            const LONG servedSerial = InterlockedCompareExchange(&g_injectedLastConsoleSerialServed, 0, 0);
            if (serial > 0 && serial != servedSerial)
            {
                InterlockedExchange(&g_injectedLastConsoleSerialServed, serial);
                return serveScriptedInput(block, block->consoleInput, sourceTag, "primary", serial, false);
            }

            const LONG auxSerial = block->consoleAuxSerial;
            if (auxSerial > 0 && serial > 0 && serial == servedSerial && block->consoleInputAux[0] != '\0')
            {
                const int roleMode = block->initParams[0];
                const LONG promptSerial = InterlockedCompareExchange(&g_injectedDelayPromptSerial, 0, 0);
                const LONG promptServed = InterlockedCompareExchange(&g_injectedDelayPromptServedSerial, 0, 0);
                const bool delayPromptPending = promptSerial > 0 && promptSerial != promptServed;
                const LONG servedAuxSerial = InterlockedCompareExchange(&g_injectedLastConsoleAuxSerialServed, 0, 0);
                const bool auxNotServedYet = servedAuxSerial != auxSerial;
                bool allowAuxNow = (roleMode == kLocalRoleLocalPlay) || delayPromptPending;
                // Online/spectate can require one hidden follow-up read before
                // prompt text is visible. Allow a single aux line in that case.
                if (!allowAuxNow && auxNotServedYet && (roleMode == kLocalRoleOnline || roleMode == kLocalRoleSpectate))
                {
                    allowAuxNow = true;
                }
                if (!allowAuxNow)
                {
                    // Skip eager aux consumption to avoid feeding blank lines to
                    // unrelated reads before the delay prompt appears.
                    goto maybe_default_input;
                }

                if (auxSerial != g_injectedActiveConsoleAuxSerial)
                {
                    InterlockedExchange(&g_injectedActiveConsoleAuxSerial, auxSerial);
                    InterlockedExchange(&g_injectedConsoleAuxScriptOffset, 0);
                    InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, 0);
                }

                if (servedAuxSerial != auxSerial)
                {
                    char auxLine[128] = {};
                    LONG auxOffset = InterlockedCompareExchange(&g_injectedConsoleAuxScriptOffset, 0, 0);
                    bool auxHasMore = false;
                    if (ExtractConsoleScriptLine(
                            block->consoleInputAux,
                            &auxOffset,
                            auxLine,
                            sizeof(auxLine),
                            &auxHasMore))
                    {
                        InterlockedExchange(&g_injectedConsoleAuxScriptOffset, auxOffset);
                        if (!auxHasMore)
                        {
                            InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, auxSerial);
                        }
                        return serveScriptedInput(block, auxLine, sourceTag, "aux_line", auxSerial, true);
                    }

                    InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, auxSerial);
                }
            }

        maybe_default_input:
            if (serial > 0 && serial == servedSerial && nNumberOfCharsToRead <= 0x80u)
            {
                const LONG promptSerial = InterlockedCompareExchange(&g_injectedDelayPromptSerial, 0, 0);
                const LONG promptServed = InterlockedCompareExchange(&g_injectedDelayPromptServedSerial, 0, 0);
                if (promptSerial > 0 && promptSerial != promptServed)
                {
                    const LONG delayInputSerial = InterlockedCompareExchange(&block->delayInputSerial, 0, 0);
                    const LONG delayInputServedSerial = InterlockedCompareExchange(&block->delayInputServedSerial, 0, 0);
                    if (delayInputSerial > delayInputServedSerial)
                    {
                        const int delayValue = block->delayInputValue;
                        char delayLine[16] = {};
                        std::snprintf(delayLine, sizeof(delayLine), "%d\r\n", delayValue);
                        InterlockedExchange(&block->delayInputServedSerial, delayInputSerial);
                        InterlockedExchange(&g_injectedDelayPromptServedSerial, promptSerial);
                        InterlockedExchange(&block->delayPromptServedSerial, promptSerial);
                        g_injectedDelayPromptWaitStartTick = 0;
                        return serveScriptedInput(block, delayLine, sourceTag, "prompt_delay_selected", delayInputSerial, true);
                    }

                    const DWORD nowTick = GetTickCount();
                    if (g_injectedDelayPromptWaitStartTick == 0)
                    {
                        g_injectedDelayPromptWaitStartTick = nowTick;
                        mod::Log(
                            "Takeover: delay prompt waiting for overlay selection promptSerial=%ld",
                            static_cast<long>(promptSerial));
                    }
                    if (nowTick - g_injectedDelayPromptWaitStartTick < kPromptDelayInputWaitTimeoutMs)
                    {
                        if (consoleEvent != nullptr)
                        {
                            const DWORD wait = WaitForSingleObject(consoleEvent, 200);
                            if (wait == WAIT_FAILED)
                            {
                                break;
                            }
                        }
                        else
                        {
                            Sleep(10);
                        }
                        continue;
                    }

                    InterlockedExchange(&g_injectedDelayPromptServedSerial, promptSerial);
                    InterlockedExchange(&block->delayPromptServedSerial, promptSerial);
                    g_injectedDelayPromptWaitStartTick = 0;
                    mod::Log(
                        "Takeover: delay prompt timed out; falling back to default promptSerial=%ld",
                        static_cast<long>(promptSerial));
                    return serveScriptedInput(block, "\r\n", sourceTag, "prompt_delay_default", promptSerial, true);
                }
                // No more scripted lines: fall through to native ReadConsoleA,
                // which blocks naturally instead of spinning on synthetic input.
                return FALSE;
            }

            if (consoleEvent != nullptr)
            {
                const DWORD wait = WaitForSingleObject(consoleEvent, 200);
                if (wait == WAIT_FAILED)
                {
                    break;
                }
                continue;
            }

            Sleep(10);
        }

        return FALSE;
    };

    // Prefer the long-lived injected handles first.
    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (HasInjectedContext() && waitAndServe(g_injectedBlock, g_injectedConsoleEvent, "injected"))
    {
        return TRUE;
    }

    // Fallback path for early startup races before injected context resolves.
    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, true))
    {
        const BOOL ok = waitAndServe(temp.block, temp.consoleEvent, "fallback");
        CloseTempIpcContext(&temp);
        if (ok)
        {
            return TRUE;
        }
    }

    SetLastError(NO_ERROR);
    const BOOL nativeResult = ReadConsoleA(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, pInputControl);
    const DWORD nativeError = GetLastError();
    const DWORD nativeRead = (lpNumberOfCharsRead != nullptr) ? *lpNumberOfCharsRead : 0;
    mod::Log(
        "nb_stub_ReadConsoleA: passthrough (ready=%d result=%d read=%lu err=%lu)",
        HasInjectedContext() ? 1 : 0,
        nativeResult ? 1 : 0,
        static_cast<unsigned long>(nativeRead),
        static_cast<unsigned long>(nativeError));
    return nativeResult;
}

BOOL StubReadConsoleW(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl)
{
    (void)hConsoleInput;
    (void)pInputControl;
    FlushPendingConsoleOutput("before_ReadConsoleW");

    auto copyInputOut = [&](const char* input) -> BOOL {
        if (input == nullptr || input[0] == '\0')
        {
            input = "2\n";
        }

        wchar_t wide[256] = {};
        const int converted =
            MultiByteToWideChar(CP_ACP, 0, input, -1, wide, static_cast<int>(sizeof(wide) / sizeof(wide[0])));
        if (converted <= 0)
        {
            if (lpNumberOfCharsRead != nullptr)
            {
                *lpNumberOfCharsRead = 0;
            }
            return FALSE;
        }

        const DWORD wideLen = static_cast<DWORD>((converted > 0) ? (converted - 1) : 0);
        const DWORD copy = (std::min)(nNumberOfCharsToRead, wideLen);
        if (lpBuffer != nullptr && copy > 0)
        {
            std::memcpy(lpBuffer, wide, static_cast<size_t>(copy) * sizeof(wchar_t));
            if (copy < nNumberOfCharsToRead)
            {
                reinterpret_cast<wchar_t*>(lpBuffer)[copy] = L'\0';
            }
        }
        if (lpNumberOfCharsRead != nullptr)
        {
            *lpNumberOfCharsRead = copy;
        }
        return TRUE;
    };

    auto serveScriptedInput = [&](SharedBlock* block, const char* input, const char* sourceTag, const char* reason, LONG serial, bool autoInput) -> BOOL {
        if (block != nullptr)
        {
            InterlockedIncrement(&block->dbgReadConsoleHits);
            if (autoInput)
            {
                InterlockedIncrement(&block->dbgReadConsoleAutoHits);
            }
        }

        const BOOL ok = copyInputOut(input);
        if (autoInput)
        {
            const LONG fallbackCount = InterlockedIncrement(&g_injectedAutoConsoleFallbackCount);
            if (fallbackCount <= 8 || (fallbackCount % 64) == 0)
            {
                mod::Log(
                    "nb_stub_ReadConsoleW: auto input serial=%ld source=%s reason=%s read=%lu value='%s' count=%ld",
                    static_cast<long>(serial),
                    sourceTag != nullptr ? sourceTag : "unknown",
                    reason != nullptr ? reason : "",
                    static_cast<unsigned long>(nNumberOfCharsToRead),
                    (input != nullptr) ? input : "",
                    static_cast<long>(fallbackCount));
            }
        }
        else
        {
            mod::Log(
                "nb_stub_ReadConsoleW: served input serial=%ld source=%s value='%s'",
                static_cast<long>(serial),
                sourceTag != nullptr ? sourceTag : "unknown",
                (input != nullptr) ? input : "");
        }
        return ok;
    };

    auto waitAndServe = [&](SharedBlock* block, HANDLE consoleEvent, const char* sourceTag) -> BOOL {
        if (block == nullptr)
        {
            return FALSE;
        }

        for (;;)
        {
            const LONG serial = block->consoleSerial;
            const LONG servedSerial = InterlockedCompareExchange(&g_injectedLastConsoleSerialServed, 0, 0);
            if (serial > 0 && serial != servedSerial)
            {
                InterlockedExchange(&g_injectedLastConsoleSerialServed, serial);
                return serveScriptedInput(block, block->consoleInput, sourceTag, "primary", serial, false);
            }

            const LONG auxSerial = block->consoleAuxSerial;
            if (auxSerial > 0 && serial > 0 && serial == servedSerial && block->consoleInputAux[0] != '\0')
            {
                const int roleMode = block->initParams[0];
                const LONG promptSerial = InterlockedCompareExchange(&g_injectedDelayPromptSerial, 0, 0);
                const LONG promptServed = InterlockedCompareExchange(&g_injectedDelayPromptServedSerial, 0, 0);
                const bool delayPromptPending = promptSerial > 0 && promptSerial != promptServed;
                const LONG servedAuxSerial = InterlockedCompareExchange(&g_injectedLastConsoleAuxSerialServed, 0, 0);
                const bool auxNotServedYet = servedAuxSerial != auxSerial;
                bool allowAuxNow = (roleMode == kLocalRoleLocalPlay) || delayPromptPending;
                // Online/spectate can require one hidden follow-up read before
                // prompt text is visible. Allow a single aux line in that case.
                if (!allowAuxNow && auxNotServedYet && (roleMode == kLocalRoleOnline || roleMode == kLocalRoleSpectate))
                {
                    allowAuxNow = true;
                }
                if (!allowAuxNow)
                {
                    goto maybe_default_input_w;
                }

                if (auxSerial != g_injectedActiveConsoleAuxSerial)
                {
                    InterlockedExchange(&g_injectedActiveConsoleAuxSerial, auxSerial);
                    InterlockedExchange(&g_injectedConsoleAuxScriptOffset, 0);
                    InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, 0);
                }

                if (servedAuxSerial != auxSerial)
                {
                    char auxLine[128] = {};
                    LONG auxOffset = InterlockedCompareExchange(&g_injectedConsoleAuxScriptOffset, 0, 0);
                    bool auxHasMore = false;
                    if (ExtractConsoleScriptLine(
                            block->consoleInputAux,
                            &auxOffset,
                            auxLine,
                            sizeof(auxLine),
                            &auxHasMore))
                    {
                        InterlockedExchange(&g_injectedConsoleAuxScriptOffset, auxOffset);
                        if (!auxHasMore)
                        {
                            InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, auxSerial);
                        }
                        return serveScriptedInput(block, auxLine, sourceTag, "aux_line", auxSerial, true);
                    }

                    InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, auxSerial);
                }
            }

        maybe_default_input_w:
            if (serial > 0 && serial == servedSerial && nNumberOfCharsToRead <= 0x80u)
            {
                const LONG promptSerial = InterlockedCompareExchange(&g_injectedDelayPromptSerial, 0, 0);
                const LONG promptServed = InterlockedCompareExchange(&g_injectedDelayPromptServedSerial, 0, 0);
                if (promptSerial > 0 && promptSerial != promptServed)
                {
                    const LONG delayInputSerial = InterlockedCompareExchange(&block->delayInputSerial, 0, 0);
                    const LONG delayInputServedSerial = InterlockedCompareExchange(&block->delayInputServedSerial, 0, 0);
                    if (delayInputSerial > delayInputServedSerial)
                    {
                        const int delayValue = block->delayInputValue;
                        char delayLine[16] = {};
                        std::snprintf(delayLine, sizeof(delayLine), "%d\r\n", delayValue);
                        InterlockedExchange(&block->delayInputServedSerial, delayInputSerial);
                        InterlockedExchange(&g_injectedDelayPromptServedSerial, promptSerial);
                        InterlockedExchange(&block->delayPromptServedSerial, promptSerial);
                        g_injectedDelayPromptWaitStartTick = 0;
                        return serveScriptedInput(block, delayLine, sourceTag, "prompt_delay_selected", delayInputSerial, true);
                    }

                    const DWORD nowTick = GetTickCount();
                    if (g_injectedDelayPromptWaitStartTick == 0)
                    {
                        g_injectedDelayPromptWaitStartTick = nowTick;
                        mod::Log(
                            "Takeover: delay prompt waiting for overlay selection promptSerial=%ld",
                            static_cast<long>(promptSerial));
                    }
                    if (nowTick - g_injectedDelayPromptWaitStartTick < kPromptDelayInputWaitTimeoutMs)
                    {
                        if (consoleEvent != nullptr)
                        {
                            const DWORD wait = WaitForSingleObject(consoleEvent, 200);
                            if (wait == WAIT_FAILED)
                            {
                                break;
                            }
                        }
                        else
                        {
                            Sleep(10);
                        }
                        continue;
                    }

                    InterlockedExchange(&g_injectedDelayPromptServedSerial, promptSerial);
                    InterlockedExchange(&block->delayPromptServedSerial, promptSerial);
                    g_injectedDelayPromptWaitStartTick = 0;
                    mod::Log(
                        "Takeover: delay prompt timed out; falling back to default promptSerial=%ld",
                        static_cast<long>(promptSerial));
                    return serveScriptedInput(block, "\r\n", sourceTag, "prompt_delay_default", promptSerial, true);
                }
                // No more scripted lines: fall through to native ReadConsoleW,
                // which blocks naturally instead of spinning on synthetic input.
                return FALSE;
            }

            if (consoleEvent != nullptr)
            {
                const DWORD wait = WaitForSingleObject(consoleEvent, 200);
                if (wait == WAIT_FAILED)
                {
                    break;
                }
                continue;
            }

            Sleep(10);
        }

        return FALSE;
    };

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (HasInjectedContext() && waitAndServe(g_injectedBlock, g_injectedConsoleEvent, "injected"))
    {
        return TRUE;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, true))
    {
        const BOOL ok = waitAndServe(temp.block, temp.consoleEvent, "fallback");
        CloseTempIpcContext(&temp);
        if (ok)
        {
            return TRUE;
        }
    }

    SetLastError(NO_ERROR);
    const BOOL nativeResult = ReadConsoleW(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, pInputControl);
    const DWORD nativeError = GetLastError();
    const DWORD nativeRead = (lpNumberOfCharsRead != nullptr) ? *lpNumberOfCharsRead : 0;
    mod::Log(
        "nb_stub_ReadConsoleW: passthrough (ready=%d result=%d read=%lu err=%lu)",
        HasInjectedContext() ? 1 : 0,
        nativeResult ? 1 : 0,
        static_cast<unsigned long>(nativeRead),
        static_cast<unsigned long>(nativeError));
    return nativeResult;
}

BOOL StubWriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
    const BOOL result = WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
    MaybeLogConsoleOutputChunk(hFile, lpBuffer, nNumberOfBytesToWrite);
    return result;
}

BOOL StubWriteConsoleA(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved)
{
    const BOOL result = WriteConsoleA(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved);
    MaybeLogConsoleWriteAChunk(lpBuffer, nNumberOfCharsToWrite);
    return result;
}

BOOL StubWriteConsoleW(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved)
{
    const BOOL result = WriteConsoleW(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved);
    MaybeLogConsoleWriteWChunk(lpBuffer, nNumberOfCharsToWrite);
    return result;
}

BOOL StubWriteConsoleOutputCharacterA(HANDLE hConsoleOutput, LPCSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten)
{
    const BOOL result = WriteConsoleOutputCharacterA(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
    MaybeLogConsoleOutputCharacterAChunk(lpCharacter, nLength);
    return result;
}

BOOL StubWriteConsoleOutputCharacterW(HANDLE hConsoleOutput, LPCWSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten)
{
    const BOOL result = WriteConsoleOutputCharacterW(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
    MaybeLogConsoleOutputCharacterWChunk(lpCharacter, nLength);
    return result;
}

VOID StubOutputDebugStringA(LPCSTR lpOutputString)
{
    OutputDebugStringA(lpOutputString);
    MaybeLogOutputDebugStringA(lpOutputString);
}

VOID StubOutputDebugStringW(LPCWSTR lpOutputString)
{
    OutputDebugStringW(lpOutputString);
    MaybeLogOutputDebugStringW(lpOutputString);
}

DWORD StubWaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    DWORD fakeExit = 0;
    if (LookupFakeThread(hHandle, &fakeExit))
    {
        (void)fakeExit;
        return WAIT_OBJECT_0;
    }
    return WaitForSingleObject(hHandle, dwMilliseconds);
}

BOOL StubGetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode)
{
    DWORD fakeExit = 0;
    if (LookupFakeThread(hThread, &fakeExit))
    {
        if (lpExitCode != nullptr)
        {
            *lpExitCode = fakeExit;
        }
        return TRUE;
    }
    return GetExitCodeThread(hThread, lpExitCode);
}

DWORD StubResumeThread(HANDLE hThread)
{
    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (HasInjectedContext())
    {
        DWORD fakeExit = 0;
        if (hThread == g_fakeProcessThreadHandle || LookupFakeThread(hThread, &fakeExit))
        {
            return 1;
        }
    }
    return ResumeThread(hThread);
}

} // namespace netplay::bridge::takeover

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_CreateProcessA(
    LPCSTR lpApplicationName,
    LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation)
{
    return netplay::bridge::takeover::StubCreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead)
{
    return netplay::bridge::takeover::StubReadProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead);
}

extern "C" __declspec(dllexport) LPVOID WINAPI nb_stub_VirtualAllocEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    return netplay::bridge::takeover::StubVirtualAllocEx(hProcess, lpAddress, dwSize, flAllocationType, flProtect);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_VirtualFreeEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    return netplay::bridge::takeover::StubVirtualFreeEx(hProcess, lpAddress, dwSize, dwFreeType);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesWritten)
{
    return netplay::bridge::takeover::StubWriteProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesWritten);
}

extern "C" __declspec(dllexport) HANDLE WINAPI nb_stub_CreateRemoteThread(HANDLE hProcess, LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId)
{
    return netplay::bridge::takeover::StubCreateRemoteThread(hProcess, lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_TerminateProcess(HANDLE hProcess, UINT uExitCode)
{
    return netplay::bridge::takeover::StubTerminateProcess(hProcess, uExitCode);
}

extern "C" __declspec(dllexport) HANDLE WINAPI nb_stub_OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId)
{
    return netplay::bridge::takeover::StubOpenProcess(dwDesiredAccess, bInheritHandle, dwProcessId);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadConsoleA(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl)
{
    return netplay::bridge::takeover::StubReadConsoleA(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, pInputControl);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadConsoleW(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl)
{
    return netplay::bridge::takeover::StubReadConsoleW(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, pInputControl);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
    return netplay::bridge::takeover::StubWriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleA(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved)
{
    return netplay::bridge::takeover::StubWriteConsoleA(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleW(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved)
{
    return netplay::bridge::takeover::StubWriteConsoleW(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleOutputCharacterA(HANDLE hConsoleOutput, LPCSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten)
{
    return netplay::bridge::takeover::StubWriteConsoleOutputCharacterA(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleOutputCharacterW(HANDLE hConsoleOutput, LPCWSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten)
{
    return netplay::bridge::takeover::StubWriteConsoleOutputCharacterW(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
}

extern "C" __declspec(dllexport) VOID WINAPI nb_stub_OutputDebugStringA(LPCSTR lpOutputString)
{
    netplay::bridge::takeover::StubOutputDebugStringA(lpOutputString);
}

extern "C" __declspec(dllexport) VOID WINAPI nb_stub_OutputDebugStringW(LPCWSTR lpOutputString)
{
    netplay::bridge::takeover::StubOutputDebugStringW(lpOutputString);
}

extern "C" __declspec(dllexport) DWORD WINAPI nb_stub_WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    return netplay::bridge::takeover::StubWaitForSingleObject(hHandle, dwMilliseconds);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode)
{
    return netplay::bridge::takeover::StubGetExitCodeThread(hThread, lpExitCode);
}

extern "C" __declspec(dllexport) DWORD WINAPI nb_stub_ResumeThread(HANDLE hThread)
{
    return netplay::bridge::takeover::StubResumeThread(hThread);
}


















