
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
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadConsoleA(
    HANDLE hConsoleInput,
    LPVOID lpBuffer,
    DWORD nNumberOfCharsToRead,
    LPDWORD lpNumberOfCharsRead,
    PCONSOLE_READCONSOLE_CONTROL pInputControl);
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
constexpr uint32_t kEfzFingerprint = 0x438F578F;
constexpr uint32_t kDefaultRevivalImageBase = 0x10000000;
constexpr uintptr_t kRevivalRoleFlagOffsets[] = {0x00A05D0u, 0x00A05F0u, 0x00A15FCu};
constexpr uintptr_t kRevivalSessionPtrOffsets[] = {0x00A02CCu, 0x00A02ECu};
constexpr uintptr_t kSessionOffsetInputDelay = 688u;
constexpr uintptr_t kSessionOffsetPingMs = 936u;
constexpr int kLocalRoleHost = 0;
constexpr int kLocalRoleSpectate = 1;
constexpr int kLocalRolePlay = 2;
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
    char consoleInput[8] = {};
    volatile LONG dbgReadConsoleHits = 0;
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
uintptr_t g_injectedInitAddress = 0;
bool g_injectedLazyBound = false;
volatile LONG g_remoteThreadCallIndex = 0;
volatile LONG g_startAbortRequested = 0;
HANDLE g_fakeProcessThreadHandle = nullptr;
bool g_initCapturedFromWrite = false;
std::mutex g_fakeThreadMutex;
std::vector<FakeThreadInfo> g_fakeThreads;

uintptr_t ResolveHostRevivalBase();
void PublishHostRevivalBase();
uintptr_t ResolveInjectedExpectedRevivalBase();

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

        const DWORD prot = mbi.Protect & 0xFFu;
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

        const DWORD prot = mbi.Protect & 0xFFu;
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
    *outValue = *reinterpret_cast<const int*>(address);
    return true;
}

bool SafeReadPtr(const void* address, uintptr_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uintptr_t)))
    {
        return false;
    }
    *outValue = *reinterpret_cast<const uintptr_t*>(address);
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

uintptr_t ReadSessionPointerFromRevival()
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return 0;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    for (uintptr_t offset : kRevivalSessionPtrOffsets)
    {
        uintptr_t sessionPtr = 0;
        if (SafeReadPtr(reinterpret_cast<const void*>(base + offset), &sessionPtr)
            && sessionPtr != 0)
        {
            return sessionPtr;
        }
    }
    return 0;
}

void RefreshRuntimeStatus(NetbridgeStatus* ioStatus)
{
    if (ioStatus == nullptr)
    {
        return;
    }

    ioStatus->pingMs = -1;
    ioStatus->rollbackFrames = -1;
    ioStatus->p1Name[0] = '\0';
    ioStatus->p2Name[0] = '\0';

    int roleFlag = ReadRoleFlagFromRevival();
    if (roleFlag < 0)
    {
        roleFlag = g_localRoleFlag;
    }
    ioStatus->roleFlag = roleFlag;

    const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
    if (sessionPtr == 0)
    {
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

        std::memcpy(wide, reinterpret_cast<const void*>(baseAddress), kMaxChars * sizeof(wchar_t));
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

    tryReadInlineName(sessionPtr + 740u, ioStatus->p1Name, sizeof(ioStatus->p1Name));
    tryReadInlineName(sessionPtr + 764u, ioStatus->p2Name, sizeof(ioStatus->p2Name));
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
    block->dbgReadConsoleHits = 0;
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

void SetPhase(NetbridgeStatus* status, NetbridgePhase phase, const char* error)
{
    if (status == nullptr)
    {
        return;
    }
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
}

void CloseProcessHandle(NetbridgeStatus* status)
{
    if (g_revivalProcess != nullptr)
    {
        CloseHandle(g_revivalProcess);
        g_revivalProcess = nullptr;
    }
    g_revivalProcessId = 0;
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
        if (g_localRoleFlag < 0)
        {
            g_localRoleFlag = kLocalRolePlay;
        }
        PublishHostRevivalBase();
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

    g_localInitFn = reinterpret_cast<RevivalInitFn>(GetProcAddress(g_localRevivalModule, "init"));
    if (g_localInitFn == nullptr)
    {
        mod::Log("Takeover: GetProcAddress(init) failed");
        return false;
    }

    int localParams[2] = {2, 102};
    const int initResult = g_localInitFn(localParams);
    g_localRoleFlag = kLocalRolePlay;
    mod::Log("Takeover: local init(2,102) result=%d", initResult);
    return true;
}

void ReinitLocalPlay()
{
    if (g_localInitFn != nullptr)
    {
        int localParams[2] = {2, 102};
        const int result = g_localInitFn(localParams);
        g_localRoleFlag = kLocalRolePlay;
        mod::Log("Takeover: local re-init(2,102) result=%d", result);
    }
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

bool PatchIatModule(
    HANDLE process,
    uintptr_t imageBase,
    const char* moduleName,
    const std::unordered_map<std::string, uint32_t>& patchMap,
    int* outPatchedCount)
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
        mod::Log("Takeover: module '%s' has no import directory", moduleName);
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

        if (_stricmp(dllName, "KERNEL32.dll") != 0 && _stricmp(dllName, "KERNELBASE.dll") != 0)
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
            SIZE_T written = 0;
            if (WriteProcessMemory(process, reinterpret_cast<LPVOID>(ftAddress), &newAddress, sizeof(newAddress), &written) == FALSE || written != sizeof(newAddress))
            {
                return false;
            }
            DWORD ignored = 0;
            (void)VirtualProtectEx(process, reinterpret_cast<LPVOID>(ftAddress), sizeof(uint32_t), oldProtect, &ignored);

            ++patched;
            mod::Log(
                "Takeover: module '%s' patched import %s old=0x%08lX new=0x%08lX",
                moduleName,
                importName,
                static_cast<unsigned long>(oldAddress),
                static_cast<unsigned long>(newAddress));
        }
    }

    if (outPatchedCount != nullptr)
    {
        *outPatchedCount = patched;
    }
    mod::Log("Takeover: module '%s' patched import count=%d", moduleName, patched);
    return true;
}

bool PatchIat(HANDLE process, DWORD processId, const std::unordered_map<std::string, uint32_t>& patchMap)
{
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
    targets.reserve(4);
    for (const RemoteModuleRecord& module : modules)
    {
        if (module.moduleLower == "efzrevival.exe")
        {
            targets.push_back(module);
        }
    }

    for (const RemoteModuleRecord& module : modules)
    {
        if (module.moduleLower == "efzrevival.dll")
        {
            targets.push_back(module);
        }
    }

    if (targets.empty())
    {
        for (const RemoteModuleRecord& module : modules)
        {
            if (isRevivalTargetModule(module.moduleLower))
            {
                targets.push_back(module);
            }
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
            "Takeover: PatchIat refused - no EfzRevival target modules found (first='%s')",
            modules.front().moduleLower.c_str());
        return false;
    }

    int totalPatched = 0;
    for (const RemoteModuleRecord& module : targets)
    {
        if (module.moduleLower == "efz_netplay_mod.dll")
        {
            mod::Log("Takeover: skipping self module patch target '%s'", module.moduleLower.c_str());
            continue;
        }

        mod::Log(
            "Takeover: PatchIat target module='%s' base=0x%08lX",
            module.moduleLower.c_str(),
            static_cast<unsigned long>(module.base));

        int patched = 0;
        if (!PatchIatModule(process, module.base, module.moduleLower.c_str(), patchMap, &patched))
        {
            mod::Log(
                "Takeover: PatchIatModule failed module='%s' base=0x%08lX",
                module.moduleLower.c_str(),
                static_cast<unsigned long>(module.base));
            return false;
        }
        totalPatched += patched;
    }

    mod::Log(
        "Takeover: patched import total count=%d modules=%zu",
        totalPatched,
        static_cast<size_t>(targets.size()));
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

bool HasInjectedContext()
{
    return g_injectedReady && g_injectedBlock != nullptr && g_injectedInitEvent != nullptr && g_injectedConsoleEvent != nullptr;
}

bool EnsureInjectedContextFast()
{
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
        (void)SetLocalRoleFlag(kLocalRolePlay, "host_initialize");
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
    mod::Log("Takeover: host shutdown");
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
    g_fakeProcessThreadHandle = nullptr;
    g_initCapturedFromWrite = false;
    g_injectedInitAddress = 0;
    g_injectedLazyBound = false;
    ClearFakeThreads();
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
}

void ShutdownInjected()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ClearFakeThreads();

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
    g_fakeProcessThreadHandle = nullptr;
    g_initCapturedFromWrite = false;
    g_localRoleFlag = -1;
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

    const int desiredRole = (selection == 2) ? kLocalRoleTournament : kLocalRolePlay;
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

    std::unordered_map<std::string, uint32_t> patches;
    patches["CreateProcessA"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_CreateProcessA));
    patches["ReadProcessMemory"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_ReadProcessMemory));
    patches["VirtualAllocEx"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_VirtualAllocEx));
    patches["VirtualFreeEx"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_VirtualFreeEx));
    patches["WriteProcessMemory"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_WriteProcessMemory));
    patches["CreateRemoteThread"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_CreateRemoteThread));
    patches["ReadConsoleA"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_ReadConsoleA));
    patches["WaitForSingleObject"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_WaitForSingleObject));
    patches["GetExitCodeThread"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_GetExitCodeThread));
    patches["ResumeThread"] = RemoteExportAddress(remoteBase, reinterpret_cast<const void*>(&nb_stub_ResumeThread));

    if (!PatchIat(pi.hProcess, pi.dwProcessId, patches))
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "IAT patch failed");
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }

    g_hostBlock->initParams[0] = 0;
    g_hostBlock->initParams[1] = 102;
    ResetDebugCounters(g_hostBlock);
    InterlockedIncrement(&g_hostBlock->initSerial);

    const char* consoleInput = "2\n";
    if (role == NetbridgeRole::Host)
    {
        consoleInput = "1\n";
    }
    else if (role == NetbridgeRole::Spectate)
    {
        consoleInput = "3\n";
    }
    CopyString(g_hostBlock->consoleInput, sizeof(g_hostBlock->consoleInput), consoleInput);
    InterlockedIncrement(&g_hostBlock->consoleSerial);

    ResetEvent(g_hostInitEvent);
    ResetEvent(g_hostConsoleEvent);

    const DWORD resumeResult = ResumeThread(pi.hThread);
    mod::Log("Takeover: resumed main thread result=%lu", static_cast<unsigned long>(resumeResult));

    g_revivalProcess = pi.hProcess;
    g_revivalProcessId = pi.dwProcessId;
    if (ioStatus != nullptr)
    {
        ioStatus->processId = g_revivalProcessId;
    }

    CloseHandle(pi.hThread);

    SetEvent(g_hostConsoleEvent);
    mod::Log("Takeover: signaled console input '%s'", consoleInput);

    DWORD waitInit = WAIT_TIMEOUT;
    const DWORD waitBegin = GetTickCount();
    DWORD lastProgressLog = waitBegin;
    DWORD lastPatchRetryTick = waitBegin;
    bool sawCreatePath = false;
    for (;;)
    {
        if (InterlockedCompareExchange(&g_startAbortRequested, 0, 0) != 0)
        {
            waitInit = WAIT_ABANDONED;
            break;
        }

        waitInit = WaitForSingleObject(g_hostInitEvent, 100);
        if (waitInit == WAIT_OBJECT_0 || waitInit == WAIT_FAILED)
        {
            break;
        }

        const DWORD now = GetTickCount();
        if ((now - lastProgressLog) >= 1000)
        {
            DWORD exitCode = 0;
            BOOL hasExitCode = FALSE;
            if (g_revivalProcess != nullptr)
            {
                hasExitCode = GetExitCodeProcess(g_revivalProcess, &exitCode);
            }
            mod::Log(
                "Takeover: waiting init handshake elapsed=%lums initSerial=%ld consoleSerial=%ld hits(rc=%ld cp=%ld wpm=%ld crt=%ld) processExit=0x%08lX hasExit=%d",
                static_cast<unsigned long>(now - waitBegin),
                static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->initSerial : 0),
                static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->consoleSerial : 0),
                static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgReadConsoleHits : 0),
                static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgCreateProcessHits : 0),
                static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgWriteProcessHits : 0),
                static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgCreateRemoteThreadHits : 0),
                static_cast<unsigned long>(hasExitCode ? exitCode : 0),
                hasExitCode ? 1 : 0);
            lastProgressLog = now;
        }

        const LONG cpHits = static_cast<LONG>(g_hostBlock != nullptr ? g_hostBlock->dbgCreateProcessHits : 0);
        const LONG wpmHits = static_cast<LONG>(g_hostBlock != nullptr ? g_hostBlock->dbgWriteProcessHits : 0);
        const LONG crtHits = static_cast<LONG>(g_hostBlock != nullptr ? g_hostBlock->dbgCreateRemoteThreadHits : 0);
        if (!sawCreatePath && (cpHits > 0 || wpmHits > 0 || crtHits > 0))
        {
            sawCreatePath = true;
            mod::Log(
                "Takeover: observed takeover create path hits cp=%ld wpm=%ld crt=%ld",
                static_cast<long>(cpHits),
                static_cast<long>(wpmHits),
                static_cast<long>(crtHits));
        }

        // EfzRevival.dll can load after process resume. Keep retrying IAT patch
        // while waiting for the init handshake so takeover stubs are wired when
        // the DLL arrives.
        if ((now - lastPatchRetryTick) >= 500 && !sawCreatePath)
        {
            const bool patchedLate = PatchIat(pi.hProcess, pi.dwProcessId, patches);
            mod::Log(
                "Takeover: late PatchIat retry result=%d elapsed=%lums",
                patchedLate ? 1 : 0,
                static_cast<unsigned long>(now - waitBegin));
            lastPatchRetryTick = now;
        }

        if ((now - waitBegin) >= kStartTimeoutMs)
        {
            break;
        }
    }

    if (waitInit != WAIT_OBJECT_0)
    {
        DWORD exitCode = 0;
        BOOL hasExitCode = FALSE;
        if (g_revivalProcess != nullptr)
        {
            hasExitCode = GetExitCodeProcess(g_revivalProcess, &exitCode);
        }

        if (InterlockedCompareExchange(&g_startAbortRequested, 0, 0) != 0)
        {
            SetPhase(ioStatus, NetbridgePhase::Failed, "start canceled");
        }
        else if (hasExitCode && exitCode != STILL_ACTIVE)
        {
            char processEndedMsg[96] = {};
            std::snprintf(
                processEndedMsg,
                sizeof(processEndedMsg),
                "init handshake: process ended (0x%08lX)",
                static_cast<unsigned long>(exitCode));
            SetPhase(ioStatus, NetbridgePhase::Failed, processEndedMsg);
        }
        else
        {
            SetPhase(ioStatus, NetbridgePhase::Failed, "init handshake timeout");
        }

        mod::Log(
            "Takeover: init handshake failed wait=%lu elapsed=%lums hits(rc=%ld cp=%ld wpm=%ld crt=%ld) processExit=0x%08lX hasExit=%d",
            static_cast<unsigned long>(waitInit),
            static_cast<unsigned long>(GetTickCount() - waitBegin),
            static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgReadConsoleHits : 0),
            static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgCreateProcessHits : 0),
            static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgWriteProcessHits : 0),
            static_cast<long>(g_hostBlock != nullptr ? g_hostBlock->dbgCreateRemoteThreadHits : 0),
            static_cast<unsigned long>(hasExitCode ? exitCode : 0),
            hasExitCode ? 1 : 0);

        if (g_revivalProcess != nullptr)
        {
            TerminateProcess(g_revivalProcess, 0);
        }
        CloseProcessHandle(ioStatus);
        ReinitLocalPlay();
        return false;
    }

    int initParams[2] = {g_hostBlock->initParams[0], g_hostBlock->initParams[1]};
    if (initParams[1] == 0)
    {
        initParams[1] = 102;
    }
    mod::Log(
        "Takeover: init handshake observed mode=%d magic=%d serial=%ld",
        initParams[0],
        initParams[1],
        static_cast<long>(g_hostBlock->initSerial));

    const int initResult = g_localInitFn(initParams);
    g_localRoleFlag = initParams[0];
    mod::Log("Takeover: local init(mode=%d magic=%d) result=%d", initParams[0], initParams[1], initResult);

    SetPhase(ioStatus, NetbridgePhase::Connected, nullptr);
    RefreshRuntimeStatus(ioStatus);
    if (outConnectStartTick != nullptr)
    {
        *outConnectStartTick = GetTickCount();
    }
    return true;
}

void Tick(NetbridgeStatus* ioStatus, uint32_t* ioConnectStartTick)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (ioStatus == nullptr)
    {
        return;
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(ioStatus->phase);
    if (phase != NetbridgePhase::Connecting && phase != NetbridgePhase::Connected)
    {
        RefreshRuntimeStatus(ioStatus);
        return;
    }

    if (!ProcessAlive(ioStatus))
    {
        if (phase == NetbridgePhase::Connecting)
        {
            SetPhase(ioStatus, NetbridgePhase::Failed, "EfzRevival process ended during connect");
        }
        else
        {
            SetPhase(ioStatus, NetbridgePhase::SessionEnded, nullptr);
        }
        ReinitLocalPlay();
        RefreshRuntimeStatus(ioStatus);
        return;
    }

    const DWORD now = GetTickCount();
    const uint32_t startTick = (ioConnectStartTick != nullptr) ? *ioConnectStartTick : 0;
    if (phase == NetbridgePhase::Connecting && startTick != 0 && (now - startTick) > kStartTimeoutMs)
    {
        SetPhase(ioStatus, NetbridgePhase::Failed, "connect timeout");
    }

    RefreshRuntimeStatus(ioStatus);
}

void CancelSession(const char* reason, NetbridgeStatus* ioStatus)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    InterlockedExchange(&g_startAbortRequested, 1);

    const bool hadProcess = ProcessAlive(ioStatus);
    if (hadProcess && g_revivalProcess != nullptr)
    {
        TerminateProcess(g_revivalProcess, 0);
    }
    CloseProcessHandle(ioStatus);
    ReinitLocalPlay();

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

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (!HasInjectedContext() || lpProcessInformation == nullptr)
    {
        TempIpcContext temp = {};
        if (lpProcessInformation != nullptr && OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
        {
            InterlockedIncrement(&temp.block->dbgCreateProcessHits);
            HANDLE processHandle = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE,
                FALSE,
                temp.block->hostPid);
            HANDLE threadHandle = CreateEventA(nullptr, TRUE, TRUE, nullptr);
            if (processHandle != nullptr && threadHandle != nullptr)
            {
                g_fakeProcessThreadHandle = threadHandle;
                std::memset(lpProcessInformation, 0, sizeof(PROCESS_INFORMATION));
                lpProcessInformation->hProcess = processHandle;
                lpProcessInformation->hThread = threadHandle;
                lpProcessInformation->dwProcessId = temp.block->hostPid;
                lpProcessInformation->dwThreadId = GetCurrentThreadId();
                mod::Log("nb_stub_CreateProcessA: fallback redirected to host pid=%lu", static_cast<unsigned long>(lpProcessInformation->dwProcessId));
                CloseTempIpcContext(&temp);
                return TRUE;
            }
            if (threadHandle != nullptr)
            {
                CloseHandle(threadHandle);
            }
            if (processHandle != nullptr)
            {
                CloseHandle(processHandle);
            }
        }
        CloseTempIpcContext(&temp);

        mod::Log("nb_stub_CreateProcessA: passthrough (ready=%d)", HasInjectedContext() ? 1 : 0);
        return CreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
    }

    HANDLE processHandle = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE,
        FALSE,
        g_injectedBlock->hostPid);
    if (processHandle == nullptr)
    {
        mod::Log("nb_stub_CreateProcessA: OpenProcess(hostPid=%lu) failed: %s", static_cast<unsigned long>(g_injectedBlock->hostPid), ErrorString(GetLastError()).c_str());
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
    lpProcessInformation->dwProcessId = g_injectedBlock->hostPid;
    lpProcessInformation->dwThreadId = GetCurrentThreadId();
    mod::Log("nb_stub_CreateProcessA: redirected to host pid=%lu", static_cast<unsigned long>(lpProcessInformation->dwProcessId));
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
            mod::Log("nb_stub_VirtualAllocEx: fallback local alloc size=%zu addr=0x%p", static_cast<size_t>(dwSize), local);
        }
        return local;
    }

    const LPVOID local = VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
    if (local == nullptr)
    {
        mod::Log("nb_stub_VirtualAllocEx: VirtualAlloc failed size=%zu err=%s", static_cast<size_t>(dwSize), ErrorString(GetLastError()).c_str());
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
            if (IsWritableRange(lpBaseAddress, nSize))
            {
                std::memcpy(lpBaseAddress, lpBuffer, nSize);
            }
            else
            {
                copied = false;
                SetLastError(ERROR_NOACCESS);
                mod::Log(
                    "nb_stub_WriteProcessMemory: fallback write blocked addr=0x%p size=%zu",
                    lpBaseAddress,
                    static_cast<size_t>(nSize));
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
        if (IsWritableRange(lpBaseAddress, nSize))
        {
            std::memcpy(lpBaseAddress, lpBuffer, nSize);
        }
        else
        {
            copied = false;
            SetLastError(ERROR_NOACCESS);
            mod::Log(
                "nb_stub_WriteProcessMemory: write blocked addr=0x%p size=%zu",
                lpBaseAddress,
                static_cast<size_t>(nSize));
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

BOOL StubReadConsoleA(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl)
{
    (void)hConsoleInput;
    (void)pInputControl;

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
        }
        if (lpNumberOfCharsRead != nullptr)
        {
            *lpNumberOfCharsRead = copy;
        }
        return TRUE;
    };

    auto tryFallbackFromSharedBlock = [&]() -> BOOL {
        TempIpcContext temp = {};
        if (!OpenTempIpcContext(&temp, false, false) || temp.block == nullptr)
        {
            CloseTempIpcContext(&temp);
            return FALSE;
        }
        InterlockedIncrement(&temp.block->dbgReadConsoleHits);
        InterlockedIncrement(&temp.block->consoleSerial);
        const BOOL ok = copyInputOut(temp.block->consoleInput);
        mod::Log("nb_stub_ReadConsoleA: fallback served input='%s'", temp.block->consoleInput);
        CloseTempIpcContext(&temp);
        return ok;
    };

    if (g_injectedBlock != nullptr)
    {
        InterlockedIncrement(&g_injectedBlock->dbgReadConsoleHits);
    }

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }

    // Always prefer IPC-provided input; this avoids startup races where the
    // injected events are not ready yet while Revival already reads stdin.
    if (tryFallbackFromSharedBlock())
    {
        return TRUE;
    }

    if (HasInjectedContext() && g_injectedBlock != nullptr)
    {
        InterlockedIncrement(&g_injectedBlock->consoleSerial);
        return copyInputOut(g_injectedBlock->consoleInput);
    }

    mod::Log("nb_stub_ReadConsoleA: passthrough (ready=%d)", HasInjectedContext() ? 1 : 0);
    return ReadConsoleA(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, pInputControl);
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

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadConsoleA(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl)
{
    return netplay::bridge::takeover::StubReadConsoleA(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, pInputControl);
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


















