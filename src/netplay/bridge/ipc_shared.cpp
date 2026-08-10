// IPC shared memory, config loading, module resolution, and session status helpers.

#include "netplay/bridge/takeover_internal.h"
#include "netplay/bridge/console_handoff_policy.h"
#include "netplay/bridge/external_launcher_guard.h"
#include "netplay/bridge/gameplay_exit_recovery.h"
#include "netplay/bridge/revival_launcher_probe.h"
#include "netplay/bridge/session_lifecycle.h"
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#include <windows.h>

namespace netplay::bridge::takeover
{

namespace
{
std::mutex g_peerProcessExitWatchMutex;
HANDLE g_peerProcessExitWatchThread = nullptr;
HANDLE g_peerProcessExitWatchStopEvent = nullptr;
HANDLE g_peerProcessExitWatchHandle = nullptr;
volatile LONG g_peerProcessExitSignaled = 0;

struct PeerProcessExitWatchContext
{
    HANDLE stopEvent = nullptr;
    HANDLE processHandle = nullptr;
};

PeerProcessExitWatchContext g_peerProcessExitWatchContext = {};

DWORD WINAPI PeerProcessExitWatchMain(LPVOID rawContext)
{
    const auto context =
        *static_cast<const PeerProcessExitWatchContext*>(rawContext);
    const HANDLE stopEvent = context.stopEvent;
    const HANDLE processHandle = context.processHandle;
    const HANDLE handles[] = {stopEvent, processHandle};
    const DWORD waitResult =
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (waitResult == WAIT_OBJECT_0 + 1)
    {
        // The active tick consumes only this atomic edge. No process API,
        // allocation, logging, or lock is needed on its steady-state path.
        InterlockedExchange(&g_peerProcessExitSignaled, 1);
    }
    return 0;
}

constexpr DWORD kProtocolRecoveryMarkerMagic = 0x50524645u; // "EFRP"
constexpr DWORD kProtocolRecoveryMarkerVersion = 1u;
constexpr size_t kProtocolRecoveryExpectedCapacity = 16u;
constexpr size_t kProtocolRecoveryOriginalCapacity = 1024u;
constexpr wchar_t kProtocolRecoveryMarkerSuffix[] =
    L".efz_netplay_mod.host_protocol_recovery";

// This fixed-size sidecar is flushed and atomically renamed into place before
// Network.Protocol is changed. Encoding the prior value as UTF-16 avoids
// losing empty, whitespace, or punctuation-bearing INI values after a crash.
struct ProtocolRecoveryMarkerDisk
{
    DWORD magic = kProtocolRecoveryMarkerMagic;
    DWORD version = kProtocolRecoveryMarkerVersion;
    DWORD recordSize = 0;
    DWORD originalValueExisted = 0;
    DWORD expectedCharCount = 0;
    DWORD originalCharCount = 0;
    wchar_t expectedValue[kProtocolRecoveryExpectedCapacity] = {};
    wchar_t originalValue[kProtocolRecoveryOriginalCapacity] = {};
    DWORD checksum = 0;
};

struct TemporaryHostProtocolOverride
{
    bool active = false;
    bool previousValueExisted = false;
    std::wstring iniPath;
    std::wstring previousValue;
    std::wstring writtenValue;
    network::NetworkFamily writtenFamily = network::NetworkFamily::IPv4;
    LONG listenerCandidateSerial = 0;
    DWORD listenerCandidateProcessId = 0;
    network::NetworkFamily listenerCandidateFamily =
        network::NetworkFamily::IPv4;
    uint16_t listenerCandidatePort = 0;
    DWORD listenerCandidateFirstTick = 0;
    LONG lastRejectedListenerSerial = 0;
};

TemporaryHostProtocolOverride g_temporaryHostProtocolOverride = {};
std::recursive_mutex g_temporaryHostProtocolMutex;

bool ReadProtocolValue(
    const std::wstring& iniPath,
    bool* outExists,
    std::wstring* outValue)
{
    if (outExists == nullptr || outValue == nullptr || iniPath.empty())
    {
        return false;
    }

    static constexpr wchar_t kMissingSentinel[] =
        L"{EFZ_NETPLAY_PROTOCOL_VALUE_MISSING}";
    wchar_t value[1024] = {};
    const DWORD length = GetPrivateProfileStringW(
        L"Network",
        L"Protocol",
        kMissingSentinel,
        value,
        static_cast<DWORD>(sizeof(value) / sizeof(value[0])),
        iniPath.c_str());
    if (length >= (sizeof(value) / sizeof(value[0])) - 1)
    {
        return false;
    }

    const std::wstring readValue(value, value + length);
    *outExists = readValue != kMissingSentinel;
    *outValue = *outExists ? readValue : std::wstring();
    return true;
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

std::wstring ProtocolRecoveryMarkerPath(const std::wstring& iniPath)
{
    return iniPath + kProtocolRecoveryMarkerSuffix;
}

DWORD ComputeProtocolRecoveryMarkerChecksum(
    const ProtocolRecoveryMarkerDisk& marker)
{
    constexpr DWORD kFnvOffset = 2166136261u;
    constexpr DWORD kFnvPrime = 16777619u;
    const auto* bytes =
        reinterpret_cast<const unsigned char*>(&marker);
    DWORD hash = kFnvOffset;
    for (size_t index = 0;
         index < offsetof(ProtocolRecoveryMarkerDisk, checksum);
         ++index)
    {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}

bool RemoveProtocolRecoveryMarker(
    const std::wstring& markerPath,
    const char* reason)
{
    if (DeleteFileW(markerPath.c_str()) != FALSE)
    {
        return true;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND
        || error == ERROR_PATH_NOT_FOUND)
    {
        return true;
    }

    mod::Log(
        "Takeover: Host Protocol recovery marker removal failed "
        "reason='%s' path='%s' err=%s",
        reason != nullptr ? reason : "",
        WideToUtf8(markerPath).c_str(),
        ErrorString(error).c_str());
    return false;
}

bool ReadProtocolRecoveryMarker(
    const std::wstring& markerPath,
    bool* outFound,
    ProtocolRecoveryMarkerDisk* outMarker)
{
    if (outFound == nullptr || outMarker == nullptr)
    {
        return false;
    }

    *outFound = false;
    *outMarker = {};
    HANDLE file = CreateFileW(
        markerPath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND
            || error == ERROR_PATH_NOT_FOUND)
        {
            return true;
        }

        mod::Log(
            "Takeover: Host Protocol recovery marker open failed "
            "path='%s' err=%s",
            WideToUtf8(markerPath).c_str(),
            ErrorString(error).c_str());
        return false;
    }

    *outFound = true;
    LARGE_INTEGER fileSize = {};
    DWORD bytesRead = 0;
    ProtocolRecoveryMarkerDisk marker = {};
    const bool readOk =
        GetFileSizeEx(file, &fileSize) != FALSE
        && fileSize.QuadPart
            == static_cast<LONGLONG>(sizeof(marker))
        && ReadFile(
               file,
               &marker,
               static_cast<DWORD>(sizeof(marker)),
               &bytesRead,
               nullptr)
            != FALSE
        && bytesRead == sizeof(marker);
    const DWORD readError = readOk ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!readOk)
    {
        mod::Log(
            "Takeover: Host Protocol recovery marker read failed "
            "path='%s' size=%lld bytesRead=%lu err=%s",
            WideToUtf8(markerPath).c_str(),
            static_cast<long long>(fileSize.QuadPart),
            static_cast<unsigned long>(bytesRead),
            ErrorString(readError).c_str());
        return false;
    }

    const bool lengthsValid =
        marker.expectedCharCount
            < kProtocolRecoveryExpectedCapacity
        && marker.originalCharCount
            < kProtocolRecoveryOriginalCapacity;
    const bool originalStateValid =
        marker.originalValueExisted <= 1u
        && (marker.originalValueExisted != 0u
            || marker.originalCharCount == 0u);
    if (marker.magic != kProtocolRecoveryMarkerMagic
        || marker.version != kProtocolRecoveryMarkerVersion
        || marker.recordSize != sizeof(marker)
        || !lengthsValid
        || !originalStateValid
        || marker.checksum
            != ComputeProtocolRecoveryMarkerChecksum(marker))
    {
        mod::Log(
            "Takeover: Host Protocol recovery marker validation failed "
            "path='%s' magic=0x%08lX version=%lu size=%lu",
            WideToUtf8(markerPath).c_str(),
            static_cast<unsigned long>(marker.magic),
            static_cast<unsigned long>(marker.version),
            static_cast<unsigned long>(marker.recordSize));
        return false;
    }

    if (marker.expectedValue[marker.expectedCharCount] != L'\0'
        || marker.originalValue[marker.originalCharCount] != L'\0')
    {
        mod::Log(
            "Takeover: Host Protocol recovery marker string validation "
            "failed path='%s'",
            WideToUtf8(markerPath).c_str());
        return false;
    }

    const std::wstring expected(
        marker.expectedValue,
        marker.expectedValue + marker.expectedCharCount);
    if (expected != L"IPv4" && expected != L"IPv6")
    {
        mod::Log(
            "Takeover: Host Protocol recovery marker expected value "
            "invalid path='%s' expected='%s'",
            WideToUtf8(markerPath).c_str(),
            WideToUtf8(expected).c_str());
        return false;
    }

    *outMarker = marker;
    return true;
}

bool WriteProtocolRecoveryMarker(
    const std::wstring& iniPath,
    bool originalValueExisted,
    const std::wstring& originalValue,
    const std::wstring& expectedValue)
{
    if (expectedValue.empty()
        || expectedValue.size()
            >= kProtocolRecoveryExpectedCapacity
        || originalValue.size()
            >= kProtocolRecoveryOriginalCapacity)
    {
        return false;
    }

    ProtocolRecoveryMarkerDisk marker = {};
    marker.recordSize = sizeof(marker);
    marker.originalValueExisted =
        originalValueExisted ? 1u : 0u;
    marker.expectedCharCount =
        static_cast<DWORD>(expectedValue.size());
    marker.originalCharCount =
        originalValueExisted
            ? static_cast<DWORD>(originalValue.size())
            : 0u;
    std::memcpy(
        marker.expectedValue,
        expectedValue.data(),
        expectedValue.size() * sizeof(wchar_t));
    if (originalValueExisted && !originalValue.empty())
    {
        std::memcpy(
            marker.originalValue,
            originalValue.data(),
            originalValue.size() * sizeof(wchar_t));
    }
    marker.checksum =
        ComputeProtocolRecoveryMarkerChecksum(marker);

    const std::wstring markerPath =
        ProtocolRecoveryMarkerPath(iniPath);
    const std::wstring temporaryPath = markerPath + L".tmp";
    HANDLE file = CreateFileW(
        temporaryPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        mod::Log(
            "Takeover: Host Protocol recovery marker temporary open "
            "failed path='%s' err=%s",
            WideToUtf8(temporaryPath).c_str(),
            ErrorString(GetLastError()).c_str());
        return false;
    }

    DWORD bytesWritten = 0;
    bool writeOk =
        WriteFile(
            file,
            &marker,
            static_cast<DWORD>(sizeof(marker)),
            &bytesWritten,
            nullptr)
            != FALSE
        && bytesWritten == sizeof(marker);
    DWORD writeError =
        writeOk ? ERROR_SUCCESS : GetLastError();
    if (writeOk && FlushFileBuffers(file) == FALSE)
    {
        writeOk = false;
        writeError = GetLastError();
    }
    CloseHandle(file);
    if (!writeOk)
    {
        (void)DeleteFileW(temporaryPath.c_str());
        mod::Log(
            "Takeover: Host Protocol recovery marker temporary write "
            "failed path='%s' bytesWritten=%lu err=%s",
            WideToUtf8(temporaryPath).c_str(),
            static_cast<unsigned long>(bytesWritten),
            ErrorString(writeError).c_str());
        return false;
    }

    if (MoveFileExW(
            temporaryPath.c_str(),
            markerPath.c_str(),
            MOVEFILE_REPLACE_EXISTING
                | MOVEFILE_WRITE_THROUGH)
        == FALSE)
    {
        const DWORD error = GetLastError();
        (void)DeleteFileW(temporaryPath.c_str());
        mod::Log(
            "Takeover: Host Protocol recovery marker commit failed "
            "path='%s' err=%s",
            WideToUtf8(markerPath).c_str(),
            ErrorString(error).c_str());
        return false;
    }

    mod::Log(
        "Takeover: Host Protocol recovery marker committed "
        "path='%s' originalExisted=%d original='%s' expected='%s'",
        WideToUtf8(markerPath).c_str(),
        originalValueExisted ? 1 : 0,
        WideToUtf8(originalValue).c_str(),
        WideToUtf8(expectedValue).c_str());
    return true;
}

bool WriteAndVerifyProtocolValue(
    const std::wstring& iniPath,
    bool valueExisted,
    const std::wstring& value)
{
    const wchar_t* writeValue =
        valueExisted ? value.c_str() : nullptr;
    if (WritePrivateProfileStringW(
            L"Network",
            L"Protocol",
            writeValue,
            iniPath.c_str())
        == FALSE)
    {
        return false;
    }

    // Force any profile API cache to disk before validating and before the
    // recovery marker is removed.
    (void)WritePrivateProfileStringW(
        nullptr,
        nullptr,
        nullptr,
        iniPath.c_str());

    bool verifiedExists = false;
    std::wstring verifiedValue;
    return ReadProtocolValue(
               iniPath,
               &verifiedExists,
               &verifiedValue)
        && verifiedExists == valueExisted
        && (!valueExisted || verifiedValue == value);
}

bool RecoverProtocolOverrideFromMarkerUnlocked(
    const std::wstring& iniPath,
    const char* reason)
{
    const std::wstring markerPath =
        ProtocolRecoveryMarkerPath(iniPath);
    bool markerFound = false;
    ProtocolRecoveryMarkerDisk marker = {};
    if (!ReadProtocolRecoveryMarker(
            markerPath,
            &markerFound,
            &marker))
    {
        return false;
    }
    if (!markerFound)
    {
        return true;
    }

    const std::wstring expectedValue(
        marker.expectedValue,
        marker.expectedValue + marker.expectedCharCount);
    const std::wstring originalValue(
        marker.originalValue,
        marker.originalValue + marker.originalCharCount);
    bool currentExists = false;
    std::wstring currentValue;
    if (!ReadProtocolValue(
            iniPath,
            &currentExists,
            &currentValue))
    {
        mod::Log(
            "Takeover: Host Protocol crash recovery deferred "
            "reason='%s' path='%s' readCurrent=0",
            reason != nullptr ? reason : "",
            WideToUtf8(iniPath).c_str());
        return false;
    }

    if (currentExists && currentValue == expectedValue)
    {
        const bool originalExisted =
            marker.originalValueExisted != 0u;
        if (!WriteAndVerifyProtocolValue(
                iniPath,
                originalExisted,
                originalValue))
        {
            mod::Log(
                "Takeover: Host Protocol crash recovery restore failed "
                "reason='%s' path='%s' originalExisted=%d "
                "original='%s' expected='%s' err=%s",
                reason != nullptr ? reason : "",
                WideToUtf8(iniPath).c_str(),
                originalExisted ? 1 : 0,
                WideToUtf8(originalValue).c_str(),
                WideToUtf8(expectedValue).c_str(),
                ErrorString(GetLastError()).c_str());
            return false;
        }

        if (!RemoveProtocolRecoveryMarker(markerPath, reason))
        {
            return false;
        }

        mod::Log(
            "Takeover: Host Protocol crash recovery restored preference "
            "reason='%s' originalExisted=%d original='%s' temporary='%s'",
            reason != nullptr ? reason : "",
            originalExisted ? 1 : 0,
            WideToUtf8(originalValue).c_str(),
            WideToUtf8(expectedValue).c_str());
        return true;
    }

    // A user or external editor changed Protocol after the marker was
    // committed. Preserve that newer state and retire only our marker.
    if (!RemoveProtocolRecoveryMarker(markerPath, reason))
    {
        return false;
    }

    mod::Log(
        "Takeover: Host Protocol crash recovery preserved newer value "
        "reason='%s' currentExisted=%d current='%s' expectedTemporary='%s'",
        reason != nullptr ? reason : "",
        currentExists ? 1 : 0,
        WideToUtf8(currentValue).c_str(),
        WideToUtf8(expectedValue).c_str());
    return true;
}

void CopyHostProtocolOverrideStateUnlocked(
    HostProtocolOverrideState* outState)
{
    if (outState == nullptr)
    {
        return;
    }

    HostProtocolOverrideState state = {};
    state.active = g_temporaryHostProtocolOverride.active;
    state.originalValueExisted =
        g_temporaryHostProtocolOverride.previousValueExisted;
    state.effectiveFamily =
        g_temporaryHostProtocolOverride.writtenFamily;
    state.originalValue = WideToUtf8(
        g_temporaryHostProtocolOverride.previousValue);

    network::NetworkFamily originalFamily =
        network::NetworkFamily::IPv4;
    if (!state.originalValueExisted
        || !network::TryParseFamilyName(
            state.originalValue,
            &originalFamily))
    {
        originalFamily = network::NetworkFamily::IPv4;
    }
    state.originalFamily = originalFamily;
    *outState = std::move(state);
}

bool IsActiveRevivalVersionTag(const char* versionTag)
{
    return versionTag != nullptr
        && g_activeRevival != nullptr
        && g_activeRevival->versionTag != nullptr
        && std::strcmp(g_activeRevival->versionTag, versionTag) == 0;
}

std::wstring ModuleDirectoryWide(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return std::wstring();
    }

    std::wstring dir(path, path + n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return std::wstring();
    }

    dir.resize(slash);
    return dir;
}

bool RemoveDirectoryTreeBestEffortW(const std::wstring& directory, DWORD* outError)
{
    DWORD lastError = ERROR_SUCCESS;
    const std::wstring searchPath = directory + L"\\*";
    WIN32_FIND_DATAW findData = {};
    HANDLE find = FindFirstFileW(searchPath.c_str(), &findData);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (lstrcmpW(findData.cFileName, L".") == 0
                || lstrcmpW(findData.cFileName, L"..") == 0)
            {
                continue;
            }

            const std::wstring childPath = directory + L"\\" + findData.cFileName;
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                if (!RemoveDirectoryTreeBestEffortW(childPath, &lastError))
                {
                    FindClose(find);
                    if (outError != nullptr)
                    {
                        *outError = lastError;
                    }
                    return false;
                }
                continue;
            }

            (void)SetFileAttributesW(childPath.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(childPath.c_str()))
            {
                lastError = GetLastError();
                if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND)
                {
                    FindClose(find);
                    if (outError != nullptr)
                    {
                        *outError = lastError;
                    }
                    return false;
                }
            }
        }
        while (FindNextFileW(find, &findData) != FALSE);

        lastError = GetLastError();
        FindClose(find);
        if (lastError != ERROR_NO_MORE_FILES)
        {
            if (outError != nullptr)
            {
                *outError = lastError;
            }
            return false;
        }
    }
    else
    {
        lastError = GetLastError();
        if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND)
        {
            if (outError != nullptr)
            {
                *outError = lastError;
            }
            return false;
        }
    }

    if (!RemoveDirectoryW(directory.c_str()))
    {
        lastError = GetLastError();
        if (lastError != ERROR_PATH_NOT_FOUND)
        {
            if (outError != nullptr)
            {
                *outError = lastError;
            }
            return false;
        }
    }

    if (outError != nullptr)
    {
        *outError = ERROR_SUCCESS;
    }
    return true;
}

void ClearPeerQuitDiagnosticBlock(SharedBlock* block)
{
    if (block == nullptr)
    {
        return;
    }

    block->peerQuitDiagnosticText[0] = '\0';
    InterlockedIncrement(&block->peerQuitDiagnosticSerial);
}

void AppendPeerQuitDiagnosticBlock(SharedBlock* block, const char* text)
{
    if (block == nullptr || text == nullptr || text[0] == '\0')
    {
        return;
    }

    const size_t capacity = sizeof(block->peerQuitDiagnosticText);
    size_t used = std::strlen(block->peerQuitDiagnosticText);
    if (used >= capacity - 1u)
    {
        return;
    }

    if (used > 0 && block->peerQuitDiagnosticText[used - 1] != '\n')
    {
        block->peerQuitDiagnosticText[used++] = '\n';
        block->peerQuitDiagnosticText[used] = '\0';
    }

    strncpy_s(
        block->peerQuitDiagnosticText + used,
        capacity - used,
        text,
        _TRUNCATE);
    InterlockedIncrement(&block->peerQuitDiagnosticSerial);
}

bool IsCompatibleTempPeerQuitDiagnosticBlock(const SharedBlock* block)
{
    if (block == nullptr
        || block->magic != kIpcMagic
        || block->version != kIpcVersion)
    {
        return false;
    }

    if (!IsExternalLauncherGuardProcess())
    {
        return true;
    }

    DWORD guardedChildPid = 0;
    return GetExternalLauncherGuardChildProcessId(&guardedChildPid)
        && guardedChildPid != 0
        && block->hostPid == guardedChildPid;
}
} // namespace

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

void ClearDelayPromptState(const char* reason)
{
    const LONG oldPrompt =
        InterlockedExchange(&g_injectedDelayPromptSerial, 0);
    const LONG oldServed =
        InterlockedExchange(&g_injectedDelayPromptServedSerial, 0);
    const LONG oldConnected =
        InterlockedExchange(&g_injectedConnectedFromDelayPromptSerial, 0);

    LONG oldSharedPrompt = 0;
    LONG oldSharedServed = 0;
    LONG oldMetricsSerial = 0;
    LONG oldInputSerial = 0;
    LONG oldInputServed = 0;
    LONG oldTransitionWake = 0;
    LONG oldNativeTimeout = 0;
    LONG oldNativeTimeoutHandled = 0;
    if (g_hostBlock != nullptr)
    {
        oldSharedPrompt = InterlockedExchange(&g_hostBlock->delayPromptSerial, 0);
        oldSharedServed = InterlockedExchange(&g_hostBlock->delayPromptServedSerial, 0);
        oldMetricsSerial = InterlockedExchange(&g_hostBlock->delayMetricsSerial, 0);
        oldInputSerial = InterlockedExchange(&g_hostBlock->delayInputSerial, 0);
        oldInputServed = InterlockedExchange(&g_hostBlock->delayInputServedSerial, 0);
        oldTransitionWake =
            InterlockedExchange(
                &g_hostBlock->delayPromptTransitionWakeSerial,
                0);
        oldNativeTimeout =
            InterlockedExchange(&g_hostBlock->nativeDelayTimeoutSerial, 0);
        InterlockedExchange(
            &g_hostBlock->nativeDelayTimeoutRequiredWakeSerial,
            0);
        oldNativeTimeoutHandled =
            InterlockedExchange(&g_hostBlock->nativeDelayTimeoutHandledSerial, 0);
        g_hostBlock->delayInputValue = -1;
        g_hostBlock->delayAveragePingMs = -1;
        g_hostBlock->delayMinPingMs = -1;
        g_hostBlock->delayMaxPingMs = -1;
        g_hostBlock->delayRecommended = -1;
        g_hostBlock->delayRangeMin = 0;
        g_hostBlock->delayRangeMax = 20;
    }

    g_delayPromptMetrics = {};

    mod::Log(
        "DelayPromptState: cleared reason=%s oldPrompt=%ld oldServed=%ld oldConnected=%ld "
        "oldSharedPrompt=%ld oldSharedServed=%ld oldMetrics=%ld oldInput=%ld/%ld "
        "oldTransitionWake=%ld oldNativeTimeout=%ld/%ld",
        reason != nullptr ? reason : "",
        static_cast<long>(oldPrompt),
        static_cast<long>(oldServed),
        static_cast<long>(oldConnected),
        static_cast<long>(oldSharedPrompt),
        static_cast<long>(oldSharedServed),
        static_cast<long>(oldMetricsSerial),
        static_cast<long>(oldInputSerial),
        static_cast<long>(oldInputServed),
        static_cast<long>(oldTransitionWake),
        static_cast<long>(oldNativeTimeout),
        static_cast<long>(oldNativeTimeoutHandled));
}

void PublishDelayPromptSerial(
    LONG serial,
    LONG controlWakeRequestSerialSnapshot)
{
    if (serial <= 0)
    {
        return;
    }

    auto publish = [&](SharedBlock* block)
    {
        if (block == nullptr
            || block->magic != kIpcMagic
            || block->version != kIpcVersion
            || block->hostPid == 0)
        {
            return;
        }

        const LONG current =
            InterlockedCompareExchange(&block->delayPromptSerial, 0, 0);
        if (serial > current)
        {
            const LONG currentControlWakeRequest =
                InterlockedCompareExchange(
                    &block->consoleControlWakeRequestSerial,
                    0,
                    0);
            const LONG transitionWakeSerial =
                console_handoff::SelectPromptTransitionWakeSerial(
                    controlWakeRequestSerialSnapshot,
                    currentControlWakeRequest);
            InterlockedExchange(
                &block->delayPromptTransitionWakeSerial,
                transitionWakeSerial);
            InterlockedExchange(&block->delayPromptSerial, serial);
            MOD_LIFECYCLE_TRACE(
                "CONSOLE_DELAY_PROMPT_PUBLISH serial=%ld "
                "writeWakeSnapshot=%ld currentWakeRequest=%ld "
                "transitionWake=%ld",
                static_cast<long>(serial),
                static_cast<long>(controlWakeRequestSerialSnapshot),
                static_cast<long>(currentControlWakeRequest),
                static_cast<long>(transitionWakeSerial));
        }
    };

    if (g_injectedBlock != nullptr)
    {
        publish(g_injectedBlock);
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        publish(temp.block);
    }
    CloseTempIpcContext(&temp);
}

bool TryPublishHeldHostDelayTimeout(
    LONG controlWakeRequestSerialSnapshot)
{
    auto publish = [controlWakeRequestSerialSnapshot](
                       SharedBlock* block) -> bool
    {
        if (block == nullptr
            || block->magic != kIpcMagic
            || block->version != kIpcVersion
            || block->hostPid == 0
            || InterlockedCompareExchange(&block->isHostSession, 0, 0) == 0)
        {
            return false;
        }

        const LONG promptSerial =
            InterlockedCompareExchange(&block->delayPromptSerial, 0, 0);
        const LONG promptServedSerial =
            InterlockedCompareExchange(&block->delayPromptServedSerial, 0, 0);
        const LONG inputSerial =
            InterlockedCompareExchange(&block->delayInputSerial, 0, 0);
        const LONG inputServedSerial =
            InterlockedCompareExchange(&block->delayInputServedSerial, 0, 0);
        if (!console_handoff::IsHeldHostDelayReader(
                true,
                promptSerial,
                promptServedSerial,
                inputSerial,
                inputServedSerial))
        {
            return false;
        }

        const LONG published =
            InterlockedCompareExchange(&block->nativeDelayTimeoutSerial, 0, 0);
        if (published < promptSerial)
        {
            const LONG transitionWakeSerial =
                InterlockedCompareExchange(
                    &block->delayPromptTransitionWakeSerial,
                    0,
                    0);
            const LONG currentWakeRequest =
                InterlockedCompareExchange(
                    &block->consoleControlWakeRequestSerial,
                    0,
                    0);
            const LONG currentWakeServed =
                InterlockedCompareExchange(
                    &block->consoleControlWakeServedSerial,
                    0,
                    0);

            // A delayed/screen-derived parse can run after the timeout WCI.
            // The preserved raw-write snapshot is authoritative when present;
            // otherwise the first current request after the prompt transition
            // is the best available causal wake.
            const LONG requiredWakeSerial =
                console_handoff::SelectTimeoutRequiredWakeSerial(
                    controlWakeRequestSerialSnapshot,
                    transitionWakeSerial,
                    currentWakeRequest);

            InterlockedExchange(
                &block->nativeDelayTimeoutRequiredWakeSerial,
                requiredWakeSerial);
            InterlockedExchange(
                &block->nativeDelayTimeoutSerial,
                promptSerial);
            MOD_LIFECYCLE_TRACE(
                "NATIVE_DELAY_TIMEOUT_PUBLISH promptSerial=%ld "
                "promptServed=%ld input=%ld/%ld writeWakeSnapshot=%ld "
                "transitionWake=%ld requiredWake=%ld currentWake=%ld/%ld",
                static_cast<long>(promptSerial),
                static_cast<long>(promptServedSerial),
                static_cast<long>(inputSerial),
                static_cast<long>(inputServedSerial),
                static_cast<long>(controlWakeRequestSerialSnapshot),
                static_cast<long>(transitionWakeSerial),
                static_cast<long>(requiredWakeSerial),
                static_cast<long>(currentWakeRequest),
                static_cast<long>(currentWakeServed));
        }
        return true;
    };

    if (g_injectedBlock != nullptr)
    {
        return publish(g_injectedBlock);
    }
    if (g_hostBlock != nullptr)
    {
        return publish(g_hostBlock);
    }

    TempIpcContext temp = {};
    const bool held =
        OpenTempIpcContext(&temp, false, false)
        && publish(temp.block);
    CloseTempIpcContext(&temp);
    return held;
}

bool HasPendingHeldHostDelayTimeout()
{
    auto pending = [](SharedBlock* block) -> bool
    {
        if (block == nullptr
            || block->magic != kIpcMagic
            || block->version != kIpcVersion
            || block->hostPid == 0)
        {
            return false;
        }

        const LONG serial =
            InterlockedCompareExchange(&block->nativeDelayTimeoutSerial, 0, 0);
        const LONG handled =
            InterlockedCompareExchange(&block->nativeDelayTimeoutHandledSerial, 0, 0);
        return serial > 0 && serial > handled;
    };

    if (g_injectedBlock != nullptr)
    {
        return pending(g_injectedBlock);
    }
    if (g_hostBlock != nullptr)
    {
        return pending(g_hostBlock);
    }

    TempIpcContext temp = {};
    const bool result =
        OpenTempIpcContext(&temp, false, false)
        && pending(temp.block);
    CloseTempIpcContext(&temp);
    return result;
}

void PublishSpectateConfirmPromptSerial(LONG serial, int promptKind)
{
    if (serial <= 0)
    {
        return;
    }

    if (g_injectedBlock != nullptr)
    {
        const LONG current = InterlockedCompareExchange(&g_injectedBlock->spectateConfirmPromptSerial, 0, 0);
        if (serial > current)
        {
            g_injectedBlock->spectateConfirmPromptKind = promptKind;
            InterlockedExchange(&g_injectedBlock->spectateConfirmPromptSerial, serial);
        }
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        const LONG current = InterlockedCompareExchange(&temp.block->spectateConfirmPromptSerial, 0, 0);
        if (serial > current)
        {
            temp.block->spectateConfirmPromptKind = promptKind;
            InterlockedExchange(&temp.block->spectateConfirmPromptSerial, serial);
        }
    }
    CloseTempIpcContext(&temp);
}

void ReadSpectateConfirmPromptSignal(LONG* outPromptSerial, LONG* outPromptServedSerial, int* outPromptKind)
{
    LONG promptSerial = InterlockedCompareExchange(&g_injectedSpectateConfirmPromptSerial, 0, 0);
    LONG promptServedSerial = InterlockedCompareExchange(&g_injectedSpectateConfirmPromptServedSerial, 0, 0);
    int promptKind = static_cast<int>(netplay::bridge::NetbridgeSpectatePromptKind::None);

    if (g_hostBlock != nullptr)
    {
        const LONG sharedPromptSerial = InterlockedCompareExchange(&g_hostBlock->spectateConfirmPromptSerial, 0, 0);
        const LONG sharedPromptServedSerial = InterlockedCompareExchange(&g_hostBlock->spectateConfirmPromptServedSerial, 0, 0);
        if (sharedPromptSerial > promptSerial)
        {
            promptSerial = sharedPromptSerial;
            promptKind = g_hostBlock->spectateConfirmPromptKind;
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
    if (outPromptKind != nullptr)
    {
        *outPromptKind = promptKind;
    }
}

void PublishConsoleError(const char* errorText)
{
    if (errorText == nullptr || errorText[0] == '\0')
    {
        return;
    }

    auto writeError = [&](SharedBlock* block)
    {
        CopyString(block->consoleErrorText, sizeof(block->consoleErrorText), errorText);
        InterlockedIncrement(&block->consoleErrorSerial);
    };

    if (g_injectedBlock != nullptr)
    {
        writeError(g_injectedBlock);
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        writeError(temp.block);
    }
    CloseTempIpcContext(&temp);
}

void PublishHostListenerObservation(
    network::NetworkFamily family,
    uint16_t port)
{
    auto publish = [&](SharedBlock* block)
    {
        InterlockedIncrement(&block->hostListenerSerial);
        InterlockedExchange(
            &block->hostListenerFamily,
            static_cast<LONG>(family));
        InterlockedExchange(
            &block->hostListenerPort,
            static_cast<LONG>(port));
        InterlockedExchange(
            &block->hostListenerProcessId,
            static_cast<LONG>(GetCurrentProcessId()));
        InterlockedIncrement(&block->hostListenerSerial);
    };

    if (g_injectedBlock != nullptr)
    {
        publish(g_injectedBlock);
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        publish(temp.block);
    }
    CloseTempIpcContext(&temp);
}

void ClearHostListenerObservation()
{
    auto clear = [](SharedBlock* block)
    {
        InterlockedExchange(&block->hostListenerSerial, 0);
        InterlockedExchange(&block->hostListenerFamily, 0);
        InterlockedExchange(&block->hostListenerPort, 0);
        InterlockedExchange(&block->hostListenerProcessId, 0);
    };

    if (g_hostBlock != nullptr)
    {
        clear(g_hostBlock);
        return;
    }
    if (g_injectedBlock != nullptr)
    {
        clear(g_injectedBlock);
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
    {
        clear(temp.block);
    }
    CloseTempIpcContext(&temp);
}

bool ReadHostListenerObservation(
    LONG* outSerial,
    network::NetworkFamily* outFamily,
    uint16_t* outPort,
    DWORD* outProcessId)
{
    if (g_hostBlock == nullptr)
    {
        return false;
    }

    const LONG serialBefore =
        InterlockedCompareExchange(&g_hostBlock->hostListenerSerial, 0, 0);
    if (serialBefore <= 0 || (serialBefore & 1) != 0)
    {
        return false;
    }
    const LONG family =
        InterlockedCompareExchange(&g_hostBlock->hostListenerFamily, 0, 0);
    const LONG port =
        InterlockedCompareExchange(&g_hostBlock->hostListenerPort, 0, 0);
    const LONG processId =
        InterlockedCompareExchange(
            &g_hostBlock->hostListenerProcessId,
            0,
            0);
    const LONG serialAfter =
        InterlockedCompareExchange(&g_hostBlock->hostListenerSerial, 0, 0);
    if (serialBefore != serialAfter
        || (serialAfter & 1) != 0
        || (family != static_cast<LONG>(network::NetworkFamily::IPv4)
            && family != static_cast<LONG>(network::NetworkFamily::IPv6))
        || port < 0
        || port > 65535
        || processId <= 0)
    {
        return false;
    }

    if (outSerial != nullptr)
    {
        *outSerial = serialAfter;
    }
    if (outFamily != nullptr)
    {
        *outFamily = static_cast<network::NetworkFamily>(family);
    }
    if (outPort != nullptr)
    {
        *outPort = static_cast<uint16_t>(port);
    }
    if (outProcessId != nullptr)
    {
        *outProcessId = static_cast<DWORD>(processId);
    }
    return true;
}

void ReadConsoleError(LONG* outSerial, char* outText, int outTextSize)
{
    LONG serial = 0;
    const char* text = nullptr;

    if (g_hostBlock != nullptr)
    {
        serial = InterlockedCompareExchange(&g_hostBlock->consoleErrorSerial, 0, 0);
        text = g_hostBlock->consoleErrorText;
    }

    if (outSerial != nullptr)
    {
        *outSerial = serial;
    }
    if (outText != nullptr && outTextSize > 0 && text != nullptr && serial > 0)
    {
        CopyString(outText, outTextSize, text);
    }
    else if (outText != nullptr && outTextSize > 0)
    {
        outText[0] = '\0';
    }
}

void ClearPeerQuitDiagnostic()
{
    if (g_injectedBlock != nullptr)
    {
        ClearPeerQuitDiagnosticBlock(g_injectedBlock);
        return;
    }
    if (g_hostBlock != nullptr)
    {
        ClearPeerQuitDiagnosticBlock(g_hostBlock);
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false)
        && IsCompatibleTempPeerQuitDiagnosticBlock(temp.block))
    {
        ClearPeerQuitDiagnosticBlock(temp.block);
    }
    CloseTempIpcContext(&temp);
}

void AppendPeerQuitDiagnostic(const char* text)
{
    if (text == nullptr || text[0] == '\0')
    {
        return;
    }

    if (g_injectedBlock != nullptr)
    {
        AppendPeerQuitDiagnosticBlock(g_injectedBlock, text);
        return;
    }
    if (g_hostBlock != nullptr)
    {
        AppendPeerQuitDiagnosticBlock(g_hostBlock, text);
        return;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, false)
        && IsCompatibleTempPeerQuitDiagnosticBlock(temp.block))
    {
        AppendPeerQuitDiagnosticBlock(temp.block, text);
    }
    CloseTempIpcContext(&temp);
}

void ReadPeerQuitDiagnostic(LONG* outSerial, char* outText, int outTextSize)
{
    LONG serial = 0;
    const char* text = nullptr;

    if (g_hostBlock != nullptr)
    {
        serial = InterlockedCompareExchange(&g_hostBlock->peerQuitDiagnosticSerial, 0, 0);
        text = g_hostBlock->peerQuitDiagnosticText;
    }
    else if (g_injectedBlock != nullptr)
    {
        serial = InterlockedCompareExchange(&g_injectedBlock->peerQuitDiagnosticSerial, 0, 0);
        text = g_injectedBlock->peerQuitDiagnosticText;
    }

    if (outSerial != nullptr)
    {
        *outSerial = serial;
    }
    if (outText != nullptr && outTextSize > 0 && text != nullptr && text[0] != '\0')
    {
        CopyString(outText, static_cast<size_t>(outTextSize), text);
    }
    else if (outText != nullptr && outTextSize > 0)
    {
        outText[0] = '\0';
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

void CleanupNativeHostShadowLogDirectory(const char* reason)
{
    if (IsCurrentProcessRevival())
    {
        return;
    }

    const std::wstring moduleDir = ModuleDirectoryWide(SelfModule());
    if (moduleDir.empty())
    {
        mod::Log(
            "CAPTURE_LOG: native_host cleanup skipped reason='%s' err=module_dir_unresolved",
            reason != nullptr ? reason : "");
        return;
    }

    // Pre-logs-folder builds used <mod>\native_host; sweep the legacy
    // location too so upgrades don't leave a stale copy behind.
    const std::wstring legacyShadowDir = moduleDir + L"\\native_host";
    if (GetFileAttributesW(legacyShadowDir.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        DWORD legacyErr = ERROR_SUCCESS;
        (void)RemoveDirectoryTreeBestEffortW(legacyShadowDir, &legacyErr);
    }

    const std::wstring shadowDir = moduleDir + L"\\logs\\native_host";
    const DWORD attrs = GetFileAttributesW(shadowDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND)
        {
            mod::Log(
                "CAPTURE_LOG: native_host cleanup stat failed reason='%s' err=%lu",
                reason != nullptr ? reason : "",
                static_cast<unsigned long>(err));
        }
        return;
    }

    DWORD cleanupErr = ERROR_SUCCESS;
    if (!RemoveDirectoryTreeBestEffortW(shadowDir, &cleanupErr))
    {
        mod::Log(
            "CAPTURE_LOG: native_host cleanup incomplete reason='%s' err=%lu",
            reason != nullptr ? reason : "",
            static_cast<unsigned long>(cleanupErr));
        return;
    }

    mod::Log(
        "CAPTURE_LOG: native_host cleanup complete reason='%s'",
        reason != nullptr ? reason : "");
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

std::wstring GameDirectoryWide()
{
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0)
    {
        return std::wstring();
    }

    std::wstring directory = path;
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return std::wstring();
    }

    directory.resize(slash);
    return directory;
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
        LogRevival102jDeepStep("Phase.takeover_transition", status);
    }
}

void CloseProcessHandle(
    NetbridgeStatus* status,
    bool waitForPeerWatcher)
{
    const bool externalLauncherParent =
        g_peerProcessOwnership
            == PeerProcessOwnership::ExternalLauncherParent;
    if (externalLauncherParent)
    {
        // Stop delivering this launcher's blocked-hard-close serials to the
        // session that is ending.  The guard retains its own exact process
        // handle and keeps the parent's narrow TerminateProcess protection
        // active until that launcher actually exits.
        RetireExternalLauncherGuardSignalDelivery();
    }
    StopPeerProcessExitWatch(waitForPeerWatcher);
    ResetInjectedPeerQuitBroadcastState();
    if (g_revivalProcess != nullptr)
    {
        CloseHandle(g_revivalProcess);
        g_revivalProcess = nullptr;
    }
    g_revivalProcessId = 0;
    g_peerProcessOwnership = PeerProcessOwnership::None;
    g_remoteInjectedSelfBase = 0;
    g_externalLauncherGuardRemoteBase = 0;
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
    if (externalLauncherParent)
    {
        (void)ReapExternalLauncherGuardIfParentExited();
    }
}

bool ProcessAlive(NetbridgeStatus* status)
{
    if (g_revivalProcess == nullptr)
    {
        return false;
    }
    DWORD exitCode = 0;
    const BOOL exitCodeRead = GetExitCodeProcess(g_revivalProcess, &exitCode);
    const bool externalLauncherParent =
        g_peerProcessOwnership
            == PeerProcessOwnership::ExternalLauncherParent;
    if (!exitCodeRead && externalLauncherParent)
    {
        // Query failure is not proof of exit. Preserve the exact terminating
        // handle and make StartSession report the previous generation busy.
        mod::Log(
            "Takeover: retaining exact external Revival parent after liveness query failure pid=%lu err=%lu",
            static_cast<unsigned long>(g_revivalProcessId),
            static_cast<unsigned long>(GetLastError()));
        return true;
    }
    if (!exitCodeRead || exitCode != STILL_ACTIVE)
    {
        if (externalLauncherParent)
        {
            const DWORD waitResult = WaitForSingleObject(g_revivalProcess, 0);
            if (waitResult != WAIT_OBJECT_0)
            {
                mod::Log(
                    "Takeover: retaining exact external Revival parent until process object signals pid=%lu exitCode=%lu wait=%lu",
                    static_cast<unsigned long>(g_revivalProcessId),
                    static_cast<unsigned long>(exitCode),
                    static_cast<unsigned long>(waitResult));
                return true;
            }
            // A retained termination-timeout handle has now signaled. Normalize
            // the launcher-origin state before the next StartSession performs
            // direct-session preflight; clean precommit failures never reach
            // this path because they release their peer slot immediately.
            g_launchDisposition =
                revival_launch::LaunchDisposition::DirectGameHost;
        }
        CloseProcessHandle(status);
        return false;
    }
    return true;
}

bool IsSyncReadyForVsHuman(const NetbridgeStatus* status)
{
    if (status == nullptr)
    {
        return false;
    }

    // Player sync ready: EFZ game mode 3 (online play), connection flag 4,
    // rollback session byte in a known-good state.
    if (status->syncGameMode == 3
        && status->syncMode0Flag1084 == 4
        && (status->syncSessionByte == 0 || status->syncSessionByte == 1 || status->syncSessionByte == 2))
    {
        return true;
    }

    // Spectator sync ready: EFZ game mode 8 (spectate), connection flag 4.
    // Spectators don't use the same session object layout so we don't check
    // sessionByte here - the lightweight spectator wrapper has different
    // offsets and a smaller object (160 bytes vs 688 for players).
    if (status->syncGameMode == 8 && status->syncMode0Flag1084 == 4)
    {
        return true;
    }

    return false;
}

bool RequiresNativeVsHumanSyncForHandoff(const NetbridgeStatus* status)
{
    // 1.02j's native "sync ready" fields describe the post-handoff EFZ VS
    // state: game mode 3 plus the mode-0 flag set by PrepareVsHumanGameState.
    // The infinite-sync capture showed those fields stay 0 while both native
    // rollback sessions already exist and wait at frame 0 for remote input.
    // Waiting for them before handoff deadlocks the startup path; the safe
    // input hook protects the early remote-empty window instead.
    if (IsActiveRevivalVersionTag("1.02j")
        && (g_localRoleFlag == kLocalRoleOnline
            || (status != nullptr && status->roleFlag == kLocalRoleOnline)))
    {
        return false;
    }

    return false;
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

RuntimeReadyProbe EvaluateRuntimeReadyProbe(const NetbridgeStatus* status)
{
    RuntimeReadyProbe probe = {};
    probe.nativeSyncReady = IsSyncReadyForVsHuman(status);

    probe.localInitApplied = g_localInitAppliedForSession;
    if (!probe.localInitApplied)
    {
        return probe;
    }

    LONG promptSerial = 0;
    ReadDelayPromptSignal(&promptSerial, nullptr);
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
        console_handoff::IsExplicitDelayInputServed(
            inputSerial,
            inputServedSerial);
    if (!probe.delayInputApplied)
    {
        return probe;
    }

    if (probe.nativeSyncReady)
    {
        probe.ready = true;
        probe.source = "native_sync_after_explicit_delay";
        return probe;
    }

    if (RequiresNativeVsHumanSyncForHandoff(status))
    {
        probe.source = "await_native_sync_102j";
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
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperPid), &helperPidField);
    (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperHandle), &helperHandleFieldRaw);

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
    g_hostBlock->hostRevivalTimestamp =
        (g_hostRevivalBase != 0 && g_activeRevival != nullptr)
            ? g_activeRevival->peTimestamp
            : 0;
    LogRevival102jDeepStep("HostIpc.ensure_complete");
    return true;
}

void CloseHostIpc()
{
    LogRevival102jDeepStep("HostIpc.close_begin");
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

bool ResetHostSharedBlockForSession(
    bool isHostSession,
    uint16_t expectedListenerPort,
    const char* reason)
{
    if (g_hostBlock == nullptr)
    {
        return false;
    }

    mod::Log(
        "Takeover: resetting SharedBlock reason='%s' initSerial=%ld consoleSerial=%ld",
        reason != nullptr ? reason : "unknown",
        static_cast<long>(g_hostBlock->initSerial),
        static_cast<long>(g_hostBlock->consoleSerial));
    const uint32_t savedMagic = g_hostBlock->magic;
    const uint32_t savedVersion = g_hostBlock->version;
    const uint32_t savedPid = g_hostBlock->hostPid;
    const uint32_t savedBase = g_hostBlock->hostRevivalBase;
    const uint32_t savedTimestamp = g_hostBlock->hostRevivalTimestamp;
    std::memset(g_hostBlock, 0, sizeof(SharedBlock));
    g_hostBlock->magic = savedMagic;
    g_hostBlock->version = savedVersion;
    g_hostBlock->hostPid = savedPid;
    g_hostBlock->hostRevivalBase = savedBase;
    g_hostBlock->hostRevivalTimestamp = savedTimestamp;
    g_hostBlock->delayInputValue = -1;
    g_hostBlock->delayAveragePingMs = -1;
    g_hostBlock->delayMinPingMs = -1;
    g_hostBlock->delayMaxPingMs = -1;
    g_hostBlock->delayRecommended = -1;
    g_hostBlock->delayRangeMax = 20;
    g_hostBlock->hostExpectedListenerPort =
        isHostSession ? static_cast<LONG>(expectedListenerPort) : 0;
    g_hostBlock->isHostSession = isHostSession ? 1 : 0;
    return true;
}

bool EnsureLocalRevivalLoaded(bool prepareManagedSession)
{
    static bool launcherStartupResolved = false;
    static bool launcherStartupAllowed = true;

    // Control-plane reap only. A retired launcher guard may outlive the
    // session whose process slot was released; collect it once that exact
    // parent has exited without re-enabling old signal delivery.
    (void)ReapExternalLauncherGuardIfParentExited();

    if (g_localInitFn != nullptr)
    {
        EnsureHostLogEfzIatPatched(true);
        PublishHostRevivalBase();
        if (prepareManagedSession
            && g_launchDisposition
                == revival_launch::LaunchDisposition::AttachExistingPractice)
        {
            // The first launcher-first Practice observation is deliberately
            // passive.  A later call reaches here only when the user starts a
            // mod-managed session from that surviving EFZ process.  Convert
            // it to the ordinary direct-host baseline now: never call init a
            // second time, but install the same recovery boundaries the
            // direct efz.exe startup path would already own.
            const bool nullGuardReady = PatchRevivalErrorCodeNullGuard();
            bool frameHookReady = InstallNetplayFrameHook();
            if (!frameHookReady
                && HasNetplayPerFrameTickHookInstalled())
            {
                // Tick-first publication makes a dispatcher failure safely
                // resumable without losing the active recovery boundary.
                frameHookReady = InstallNetplayFrameHook();
            }
            const bool tickHookReady =
                HasNetplayPerFrameTickHookInstalled();
            const bool exitIatReady = tickHookReady
                && PatchRevivalDllExitProcess();
            if (!nullGuardReady || !tickHookReady || !exitIatReady)
            {
                mod::Log(
                    "LauncherBootstrap: Practice-to-managed conversion failed nullGuard=%d frame=%d tick=%d iat=%d",
                    nullGuardReady ? 1 : 0,
                    frameHookReady ? 1 : 0,
                    tickHookReady ? 1 : 0,
                    exitIatReady ? 1 : 0);
                return false;
            }
            if (!frameHookReady)
            {
                mod::Log(
                    "LauncherBootstrap: Practice conversion retained active tick-only recovery; init-only dispatcher unavailable");
            }
            g_launchDisposition =
                revival_launch::LaunchDisposition::DirectGameHost;
            mod::Log(
                "LauncherBootstrap: Practice process converted to managed direct-session baseline initCalls=0");
        }
        const bool launcherOwnedObject =
            g_launchDisposition
                == revival_launch::LaunchDisposition::AttachExistingPractice
            || g_launchDisposition
                == revival_launch::LaunchDisposition::AdoptExternalOnline
            || g_launchDisposition
                == revival_launch::LaunchDisposition::AdoptExternalSpectator
            || g_launchDisposition
                == revival_launch::LaunchDisposition::AttachExistingTournament;
        if (!launcherOwnedObject && !PatchRevivalErrorCodeNullGuard())
        {
            mod::Log("Takeover: warning failed to verify EfzRevival null-guard");
        }
        if (prepareManagedSession
            && g_launchDisposition
                == revival_launch::LaunchDisposition::DirectGameHost)
        {
            bool frameHookReady = InstallNetplayFrameHook();
            if (!frameHookReady
                && HasNetplayPerFrameTickHookInstalled())
            {
                frameHookReady = InstallNetplayFrameHook();
            }
            const bool tickHookReady =
                HasNetplayPerFrameTickHookInstalled();
            const bool exitIatReady = tickHookReady
                && PatchRevivalDllExitProcess();
            if (!tickHookReady || !exitIatReady)
            {
                mod::Log(
                    "Takeover: managed-session recovery preflight failed frame=%d tick=%d iat=%d",
                    frameHookReady ? 1 : 0,
                    tickHookReady ? 1 : 0,
                    exitIatReady ? 1 : 0);
                return false;
            }
        }
        if (g_localRoleFlag < 0)
        {
            g_localRoleFlag = kLocalRoleLocalPlay;
        }
        return true;
    }

    if (!launcherStartupResolved)
    {
        RevivalLauncherProbe probe =
            ProbeRevivalLauncherStartup(10000u);
        g_launchDisposition = probe.disposition;
        launcherStartupResolved = true;

        const bool attachPractice =
            probe.disposition
                == revival_launch::LaunchDisposition::AttachExistingPractice;
        const bool adoptOnline =
            probe.disposition
                == revival_launch::LaunchDisposition::AdoptExternalOnline;
        const bool adoptSpectator =
            probe.disposition
                == revival_launch::LaunchDisposition::AdoptExternalSpectator;
        const bool attachTournament =
            probe.disposition
                == revival_launch::LaunchDisposition::AttachExistingTournament;
        const bool externalActive = adoptOnline || adoptSpectator;
        const bool managedExisting = externalActive || attachTournament;

        if (probe.disposition
                == revival_launch::LaunchDisposition::PassiveFailClosed
            || probe.disposition
                == revival_launch::LaunchDisposition::AwaitExternalSession)
        {
            launcherStartupAllowed = false;
            mod::Log(
                "LauncherBootstrap: passive/fail-closed disposition=%d; "
                "leaving native Revival state untouched",
                static_cast<int>(probe.disposition));
            ReleaseRevivalLauncherProbe(&probe);
            return false;
        }

        if (attachPractice || managedExisting)
        {
            g_localRevivalModule = GetModuleHandleA("EfzRevival.dll");
            const RevivalInitFn existingInitFn = g_localRevivalModule != nullptr
                ? reinterpret_cast<RevivalInitFn>(
                    GetProcAddress(g_localRevivalModule, "init"))
                : nullptr;
            if (g_localRevivalModule == nullptr
                || existingInitFn == nullptr
                || probe.profile == nullptr
                || g_activeRevival == nullptr
                || g_activeRevival->peTimestamp
                    != probe.profile->peTimestamp
                || g_activeRevival->launcherExeTimestamp
                    != probe.profile->launcherExeTimestamp)
            {
                launcherStartupAllowed = false;
                mod::Log(
                    "LauncherBootstrap: exact session changed before attach; fail closed");
                ReleaseRevivalLauncherProbe(&probe);
                return false;
            }

            if (attachPractice)
            {
                if (!RevalidateRevivalLauncherStartup(probe))
                {
                    launcherStartupAllowed = false;
                    g_launchDisposition =
                        revival_launch::LaunchDisposition::PassiveFailClosed;
                    mod::Log(
                        "LauncherBootstrap: Practice session changed during attach; fail closed");
                    ReleaseRevivalLauncherProbe(&probe);
                    return false;
                }

                // Practice remains native/passive: record the already-loaded
                // module so later title UI can coexist, but install no
                // simulation, ExitProcess, parent, or lifecycle hooks and do
                // not call exported init a second time.
                g_localInitFn = existingInitFn;
                g_localRoleFlag = probe.role;
                g_lastValidatedSessionPtr = probe.sessionPtr;
                PublishHostRevivalBase();
                mod::Log(
                    "LauncherBootstrap: attached exact Practice session version=%s role=%d session=0x%08lX initCalls=0 policy=passive",
                    probe.profile->versionTag,
                    probe.role,
                    static_cast<unsigned long>(probe.sessionPtr));
                ReleaseRevivalLauncherProbe(&probe);
                return true;
            }

            if (attachTournament
                && (!RevalidateRevivalLauncherStartup(probe)
                    || !AdoptExistingTournamentExePatchState()))
            {
                launcherStartupAllowed = false;
                g_launchDisposition =
                    revival_launch::LaunchDisposition::PassiveFailClosed;
                mod::Log(
                    "LauncherBootstrap: Tournament object/patch state changed before managed attach; fail closed");
                ReleaseRevivalLauncherProbe(&probe);
                return false;
            }

            uintptr_t externalRemoteBase = 0;
            bool renderContextSaved = false;

            // Snapshot the mod-only publication state before either attach
            // path reaches its first live code write.  Online/spectator still
            // publish this state at their existing point below.  Tournament
            // publishes only the identity/recovery subset immediately after
            // exact object/EXE-patch admission so the first tick detour can be
            // installed before any renderer, IPC, or logging setup runs.
            const RevivalInitFn priorInitFn = g_localInitFn;
            const int priorLocalRole = g_localRoleFlag;
            const uintptr_t priorSessionPtr = g_lastValidatedSessionPtr;
            const int priorNetplayRole = g_netplayRole;
            const bool priorLocalInitApplied = g_localInitAppliedForSession;
            const bool priorSpectatorAttempted =
                g_spectatorPostInitAttemptedForSession;
            const bool priorSpectatorSucceeded =
                g_spectatorPostInitSucceededForSession;
            const bool priorClientSwapApplied = IsClientInputSwapApplied();
            const uintptr_t priorExternalRemoteBase =
                g_externalLauncherGuardRemoteBase;
            const LONG priorTournamentInitialTitleLeft =
                InterlockedCompareExchange(
                    &g_externalTournamentInitialTitleLeft, 0, 0);
            const LONG priorRevivalExitIntercepted =
                InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0);
            const LONG priorRevivalExitMode =
                InterlockedCompareExchange(&g_revivalExitMode, 0, 0);

            const auto restoreStagedModState = [&]() {
                g_localInitFn = priorInitFn;
                g_localRoleFlag = priorLocalRole;
                g_lastValidatedSessionPtr = priorSessionPtr;
                g_netplayRole = priorNetplayRole;
                g_localInitAppliedForSession = priorLocalInitApplied;
                g_spectatorPostInitAttemptedForSession =
                    priorSpectatorAttempted;
                g_spectatorPostInitSucceededForSession =
                    priorSpectatorSucceeded;
                SetClientInputSwapApplied(priorClientSwapApplied);
                g_externalLauncherGuardRemoteBase = priorExternalRemoteBase;
                InterlockedExchange(
                    &g_externalTournamentInitialTitleLeft,
                    priorTournamentInitialTitleLeft);
                InterlockedExchange(
                    &g_revivalExitMode,
                    priorRevivalExitMode);
                InterlockedExchange(
                    &g_revivalExitIntercepted,
                    priorRevivalExitIntercepted);
            };
            const auto requestManagedAttachRecovery = [&](const char* reason) {
                InterlockedExchange(
                    &g_revivalExitMode,
                    static_cast<LONG>(probe.role));
                if (g_hostBlock != nullptr)
                {
                    CopyString(
                        g_hostBlock->consoleErrorText,
                        sizeof(g_hostBlock->consoleErrorText),
                        reason != nullptr
                            ? reason
                            : "External launcher attachment failed");
                    InterlockedIncrement(&g_hostBlock->consoleErrorSerial);
                }
                // Publish the fatal/recovery latch last.  A quarantined tick
                // can therefore never observe an exit mode that still belongs
                // to the prior generation, even when host IPC does not exist.
                InterlockedExchange(&g_revivalExitIntercepted, 1);
            };

            bool recoveryArmed = false;
            bool frameHookReady = false;
            bool tickHookReady = false;
            bool attachBoundaryObserved = false;
            bool exitCallsitesReady = !attachTournament;
            bool exitIatReady = false;

            if (attachTournament)
            {
                // Role 3 may begin with only one queued title input.  Native
                // Revival can consume that input and call ExitProcess on its
                // very next tick, so publish the smallest coherent recovery
                // identity and park that tick before any control-plane setup.
                (void)BeginManagedSessionBoundary(
                    "ExternalTournamentAdoption");
                InterlockedExchange(&g_revivalExitIntercepted, 0);
                InterlockedExchange(&g_revivalExitMode, -1);
                InterlockedExchange(&g_startAbortRequested, 0);
                ResetInjectedPeerQuitBroadcastState();
                ResetNativeWorkflowFlags();
                g_delayPromptMetrics = {};
                g_injectedSpectateConfirmPromptWaitStartTick = 0;
                g_localInitFn = existingInitFn;
                g_localRoleFlag = probe.role;
                g_lastValidatedSessionPtr = probe.sessionPtr;
                g_netplayRole = kNetplayRoleNone;
                g_localInitAppliedForSession = true;
                g_spectatorPostInitAttemptedForSession = false;
                g_spectatorPostInitSucceededForSession = false;
                g_externalLauncherGuardRemoteBase = 0;
                SetClientInputSwapApplied(false);
                int currentMode = 0;
                const bool alreadyLeftTitle =
                    g_activeRevival != nullptr
                    && SafeReadInt(
                        reinterpret_cast<const void*>(
                            g_activeRevival->addrGameModeCurrentIndex),
                        &currentMode)
                    && currentMode != 0;
                InterlockedExchange(
                    &g_externalTournamentInitialTitleLeft,
                    alreadyLeftTitle ? 1 : 0);

                SetExternalLauncherAttachPending(true);
                recoveryArmed = true;
                frameHookReady = InstallNetplayFrameHook();
                if (!frameHookReady
                    && HasNetplayPerFrameTickHookInstalled())
                {
                    // Tick-first publication makes a dispatcher failure
                    // resumable while the acknowledged tick stays parked.
                    frameHookReady = InstallNetplayFrameHook();
                }
                tickHookReady = HasNetplayPerFrameTickHookInstalled();
                if (!tickHookReady && !HasAnyNetplayFrameHookInstalled())
                {
                    SetExternalLauncherAttachPending(false);
                    (void)RestoreDllExitProcessPatches();
                    restoreStagedModState();
                    launcherStartupAllowed = false;
                    g_launchDisposition =
                        revival_launch::LaunchDisposition::PassiveFailClosed;
                    mod::Log(
                        "LauncherBootstrap: Tournament tick-boundary precommit failed cleanly; native role-3 session remains passive");
                    ReleaseRevivalLauncherProbe(&probe);
                    return false;
                }

                // The acknowledgement proves every native tick that entered
                // before the prologue transaction has returned.  While this
                // one invocation is parked, install both exact child-side
                // ExitProcess barriers without dropping a native tick.
                attachBoundaryObserved = tickHookReady
                    && WaitForExternalLauncherAttachBoundary(5000u);
                exitCallsitesReady = attachBoundaryObserved
                    && SaveAndApplyExternalTournamentExitGuard();
                exitIatReady =
                    attachBoundaryObserved && tickHookReady
                    && exitCallsitesReady && PatchRevivalDllExitProcess();
                if (!frameHookReady || !tickHookReady
                    || !attachBoundaryObserved || !exitCallsitesReady
                    || !exitIatReady)
                {
                    mod::Log(
                        "LauncherBootstrap: early Tournament guard publication failed frame=%d tick=%d boundary=%d callsites=%d iat=%d; quarantining parked generation",
                        frameHookReady ? 1 : 0,
                        tickHookReady ? 1 : 0,
                        attachBoundaryObserved ? 1 : 0,
                        exitCallsitesReady ? 1 : 0,
                        exitIatReady ? 1 : 0);
                    requestManagedAttachRecovery(
                        "External Tournament recovery-guard installation failed");
                    MarkRevivalSyncDiagnosticsSessionStart(
                        "ExternalTournamentAdoption_guard_quarantined");
                    // Keep the acknowledged invocation parked while DllMain's
                    // worker performs the multi-byte UI patch transaction.
                    // CompleteExternalLauncherUiAttachment observes the
                    // fatal latch and performs the single quarantine/unpark.
                    ReleaseRevivalLauncherProbe(&probe);
                    return true;
                }

                if (!RevalidateRevivalLauncherSessionSnapshot(probe))
                {
                    mod::Log(
                        "LauncherBootstrap: Tournament session changed after early guard publication; quarantining parked generation");
                    requestManagedAttachRecovery(
                        "External Tournament session changed during guard publication");
                    MarkRevivalSyncDiagnosticsSessionStart(
                        "ExternalTournamentAdoption_guard_revalidate_failed");
                    ReleaseRevivalLauncherProbe(&probe);
                    return true;
                }
            }
            if (externalActive)
            {
                // Protect the exact native launcher before child-side setup.
                // Its queue-full disconnect fallback calls TerminateProcess;
                // this narrow guard blocks only that verified cleanup site and
                // remains pass-through if the child attach later fails.
                if (!RevalidateRevivalLauncherStartup(probe)
                    || !InstallExternalLauncherGuard(
                        probe.parentProcess,
                        probe.parentPid,
                        probe.parentCreationTime,
                        *probe.profile,
                        &externalRemoteBase))
                {
                    launcherStartupAllowed = false;
                    g_launchDisposition =
                        revival_launch::LaunchDisposition::PassiveFailClosed;
                    mod::Log(
                        "LauncherBootstrap: exact parent guard admission failed; leaving native online session untouched");
                    CleanupExternalLauncherGuard();
                    ReleaseRevivalLauncherProbe(&probe);
                    return false;
                }

                if (!RevalidateRevivalLauncherSessionSnapshot(probe))
                {
                    launcherStartupAllowed = false;
                    g_launchDisposition =
                        revival_launch::LaunchDisposition::PassiveFailClosed;
                    mod::Log(
                        "LauncherBootstrap: exact session changed during parent-guard handshake; fail closed");
                    CleanupExternalLauncherGuard();
                    ReleaseRevivalLauncherProbe(&probe);
                    return false;
                }

                // Keep the probe's exact parent handle alive through the final
                // parked-session revalidation.  That validator deliberately
                // brackets the mutable object snapshot with this same process
                // object/creation-time witness; moving the handle here made
                // every otherwise-valid Online/Spectator attach fail its final
                // check and enter managed title recovery.  The session/watcher
                // owns an independent duplicate instead.
                HANDLE sessionParentProcess = nullptr;
                if (!DuplicateHandle(
                        GetCurrentProcess(),
                        probe.parentProcess,
                        GetCurrentProcess(),
                        &sessionParentProcess,
                        0,
                        FALSE,
                        DUPLICATE_SAME_ACCESS))
                {
                    launcherStartupAllowed = false;
                    g_launchDisposition =
                        revival_launch::LaunchDisposition::PassiveFailClosed;
                    mod::Log(
                        "LauncherBootstrap: external parent handle duplication failed err=%lu; leaving native online session untouched",
                        static_cast<unsigned long>(GetLastError()));
                    CleanupExternalLauncherGuard();
                    ReleaseRevivalLauncherProbe(&probe);
                    return false;
                }
                g_revivalProcess = sessionParentProcess;
                g_revivalProcessId = probe.parentPid;
                g_peerProcessOwnership =
                    PeerProcessOwnership::ExternalLauncherParent;
                if (!StartPeerProcessExitWatch())
                {
                    launcherStartupAllowed = false;
                    g_launchDisposition =
                        revival_launch::LaunchDisposition::PassiveFailClosed;
                    mod::Log(
                        "LauncherBootstrap: external parent watcher failed; "
                        "leaving native online session untouched");
                    CloseProcessHandle(nullptr);
                    CleanupExternalLauncherGuard();
                    ReleaseRevivalLauncherProbe(&probe);
                    return false;
                }
            }

            // Do not hold the transaction open waiting for the renderer.
            // Capture immediately when available; cleanup helpers retry the
            // capture lazily if EFZ has not published it yet.
            renderContextSaved = SaveRenderContext();
            if (!renderContextSaved)
            {
                mod::Log(
                    "LauncherBootstrap: renderer not published yet; continuing with lazy recovery capture");
            }

            const bool externalHost =
                adoptOnline && probe.activePlayer == 0;
            if (!EnsureHostIpc()
                || !ResetHostSharedBlockForSession(
                    externalHost,
                    0,
                    attachTournament
                        ? "ExternalTournamentAdoption"
                        : "ExternalLauncherAdoption"))
            {
                if (attachTournament)
                {
                    // The active tick and both child exit barriers are already
                    // published.  Never roll this generation back to native
                    // execution merely because optional control-plane setup
                    // failed; hostBlock may legitimately still be null here.
                    mod::Log(
                        "LauncherBootstrap: host IPC unavailable after Tournament guard publication; quarantining parked generation");
                    requestManagedAttachRecovery(
                        "External Tournament host IPC setup failed");
                    MarkRevivalSyncDiagnosticsSessionStart(
                        "ExternalTournamentAdoption_ipc_quarantined");
                    ReleaseRevivalLauncherProbe(&probe);
                    return true;
                }
                launcherStartupAllowed = false;
                g_launchDisposition =
                    revival_launch::LaunchDisposition::PassiveFailClosed;
                mod::Log(
                    "LauncherBootstrap: host IPC unavailable before existing-session attach; no child hooks published; parent guard cancelled/pass-through");
                CloseProcessHandle(nullptr);
                CleanupExternalLauncherGuard();
                ReleaseRevivalLauncherProbe(&probe);
                return false;
            }

            // Tournament already owns its coherent hook/recovery identity and
            // is parked behind both exit barriers.  Finish the nonessential
            // per-session control latches now.  Online/spectator retain their
            // original publication order after parent/render/IPC setup.
            if (!attachTournament)
            {
                (void)BeginManagedSessionBoundary(
                    "ExternalLauncherAdoption");
                InterlockedExchange(&g_revivalExitIntercepted, 0);
                InterlockedExchange(&g_revivalExitMode, -1);
            }
            if (!attachTournament)
            {
                InterlockedExchange(&g_startAbortRequested, 0);
                ResetInjectedPeerQuitBroadcastState();
                ResetNativeWorkflowFlags();
                g_delayPromptMetrics = {};
                g_injectedSpectateConfirmPromptWaitStartTick = 0;
                g_externalLauncherGuardRemoteBase = externalRemoteBase;
                g_spectatorPostInitAttemptedForSession = false;
                g_spectatorPostInitSucceededForSession = false;
                g_localInitFn = existingInitFn;
                g_localRoleFlag = probe.role;
                g_lastValidatedSessionPtr = probe.sessionPtr;
                if (adoptSpectator)
                {
                    g_netplayRole = kNetplayRoleSpectator;
                    g_localInitAppliedForSession = true;
                    g_spectatorPostInitAttemptedForSession = true;
                    g_spectatorPostInitSucceededForSession = true;
                    SetClientInputSwapApplied(false);
                }
                else
                {
                    g_netplayRole = probe.activePlayer == 1
                        ? kNetplayRoleClient
                        : kNetplayRoleHost;
                    SetClientInputSwapApplied(probe.activePlayer == 1);
                    g_localInitAppliedForSession = true;
                }
            }

            PublishHostRevivalBase();
            g_hostBlock->initParams[0] = probe.role;
            g_hostBlock->initParams[1] = 102;
            InterlockedIncrement(&g_hostBlock->initSerial);
            EnsureHostLogEfzIatPatched(true);
            PrimeGracefulQuitRingForSession();

            if (!attachTournament)
            {
                // Preserve the established Online/Spectator order: parent,
                // renderer, IPC, and coherent mod publication all precede the
                // tick/ExitProcess boundary for those already-live sessions.
                SetExternalLauncherAttachPending(true);
                recoveryArmed = ArmExternalLauncherExitRecovery();
                frameHookReady =
                    recoveryArmed && InstallNetplayFrameHook();
                if (!frameHookReady
                    && HasNetplayPerFrameTickHookInstalled())
                {
                    // A dispatcher write can fail transiently after the active
                    // tick boundary is live.  Its exact tick-only state is
                    // resumable; retry once before quarantining the generation.
                    frameHookReady = InstallNetplayFrameHook();
                }
                tickHookReady = HasNetplayPerFrameTickHookInstalled();
                if (!tickHookReady && !HasAnyNetplayFrameHookInstalled())
                {
                    SetExternalLauncherAttachPending(false);
                    (void)RestoreDllExitProcessPatches();
                    restoreStagedModState();
                    CloseProcessHandle(nullptr);
                    CleanupExternalLauncherGuard();
                    launcherStartupAllowed = false;
                    g_launchDisposition =
                        revival_launch::LaunchDisposition::PassiveFailClosed;
                    mod::Log(
                        "LauncherBootstrap: frame-hook precommit failed cleanly; no child simulation/ExitProcess hook remains and parent guard is cancelled/pass-through");
                    ReleaseRevivalLauncherProbe(&probe);
                    return false;
                }

                // The acknowledgement can only be emitted by an entry through
                // the new tick detour.  Waiting for it proves that any native
                // tick which entered before the prologue write has returned.
                attachBoundaryObserved = tickHookReady
                    && WaitForExternalLauncherAttachBoundary(5000u);
                exitIatReady =
                    attachBoundaryObserved && tickHookReady && recoveryArmed
                    && PatchRevivalDllExitProcess();
                if (!frameHookReady || !tickHookReady
                    || !attachBoundaryObserved || !recoveryArmed
                    || !exitCallsitesReady || !exitIatReady)
                {
                    mod::Log(
                        "LauncherBootstrap: post-publication attach failure frame=%d tick=%d boundary=%d arm=%d callsites=%d iat=%d; retaining coherent managed state and scheduling title recovery",
                        frameHookReady ? 1 : 0,
                        tickHookReady ? 1 : 0,
                        attachBoundaryObserved ? 1 : 0,
                        recoveryArmed ? 1 : 0,
                        exitCallsitesReady ? 1 : 0,
                        exitIatReady ? 1 : 0);
                    if (tickHookReady)
                    {
                        requestManagedAttachRecovery(
                            "External launcher recovery-hook installation failed");
                    }
                    MarkRevivalSyncDiagnosticsSessionStart(
                        "ExternalLauncherAdoption_quarantined");
                    // Keep an observed invocation parked through DllMain's
                    // title/UI patch transaction even on failure.  The
                    // post-InstallHooks finalizer sees the fatal latch and
                    // performs the sole quarantine/release; unblocking here
                    // would let recovery race multi-byte title patch writes.
                    ReleaseRevivalLauncherProbe(&probe);
                    return true;
                }
            }

            // Revalidate after both live boundaries are installed. At this
            // point publication is intentionally one-way: a changed/destroyed
            // native object is recovered without running one more native tick,
            // rather than racing a live hook rollback.
            if (!RevalidateRevivalLauncherSessionSnapshot(probe))
            {
                mod::Log(
                    "LauncherBootstrap: session changed after hook publication; scheduling managed title recovery");
                requestManagedAttachRecovery(
                    "External Revival session changed during attachment");
                MarkRevivalSyncDiagnosticsSessionStart(
                    "ExternalLauncherAdoption_revalidate_failed");
                // Tournament and Online/Spectator now share one UI-safe
                // release point after InstallHooks.  Leave the acknowledged
                // invocation parked and let that finalizer quarantine it.
                ReleaseRevivalLauncherProbe(&probe);
                return true;
            }

            MarkRevivalSyncDiagnosticsSessionStart(
                attachTournament
                    ? "ExternalTournamentAdoption_commit"
                    : "ExternalLauncherAdoption_commit");
            if (attachTournament)
            {
                // Keep the one acknowledged game-thread invocation parked
                // until DllMain's worker has installed every title/UI patch.
                // The title patcher writes multi-byte instructions, so this
                // boundary is the transaction that prevents torn live code.
                mod::Log(
                    "LauncherBootstrap: exact Tournament session ready; holding native tick until title/UI hook installation completes");
                ReleaseRevivalLauncherProbe(&probe);
                return true;
            }
            // Online/Spectator use the same parked publication boundary as
            // Tournament.  State-export initialization and multi-byte title
            // hook installation still happen after this function returns;
            // releasing here let native title navigation race those writes
            // and left the exporter active throughout simulation.  The
            // post-InstallHooks finalizer performs the hooks-layer suspension
            // first, then commits this exact invocation once.
            mod::Log(
                "LauncherBootstrap: exact %s session ready; holding native tick until title/UI installation and simulation handoff complete version=%s role=%d session=0x%08lX parentPid=%lu initCalls=0 renderSaved=%d",
                adoptSpectator ? "Spectator" : "Online",
                probe.profile->versionTag,
                probe.role,
                static_cast<unsigned long>(probe.sessionPtr),
                static_cast<unsigned long>(probe.parentPid),
                renderContextSaved ? 1 : 0);
            ReleaseRevivalLauncherProbe(&probe);
            return true;
        }

        ReleaseRevivalLauncherProbe(&probe);
    }

    if (!launcherStartupAllowed)
    {
        return false;
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

    // Detect the DLL version BEFORE any profile-dependent operations.
    // Without this, InstallNetplayFrameHook() (called below) would use
    // the default 1.02e profile addresses which are wrong for 1.02g+.
    DetectRevivalVersion();

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

    // Host log interception is deliberately disabled: Revival's game-process
    // init and simulation paths keep their native IAT/cadence. The helper
    // process owns raw console/log capture instead. EnsureHostLogEfzIatPatched
    // remains as an explicit no-op contract at this legacy call site.
    EnsureHostLogEfzIatPatched(true);
    // Each Revival init() should start a fresh logical session section in
    // the managed root logEfz.txt. Closing here lets the first native log
    // line from this init reopen the file and emit a new SESSION header.
    CloseMirrorLogFiles();

    int localParams[2] = {2, 102};
    const int initResult = g_localInitFn(localParams);
    g_localRoleFlag = kLocalRoleLocalPlay;
    mod::Log("Takeover: local init(2,102) result=%d", initResult);

    // Publish the active tick recovery boundary before redirecting the DLL's
    // noreturn ExitProcess import.  A tick-only state can safely retry the
    // init-only dispatcher; the inverse ordering could expose an interceptor
    // with no valid longjmp owner.
    bool frameHookReady = InstallNetplayFrameHook();
    if (!frameHookReady && HasNetplayPerFrameTickHookInstalled())
    {
        frameHookReady = InstallNetplayFrameHook();
    }
    const bool tickHookReady = HasNetplayPerFrameTickHookInstalled();
    if (!tickHookReady)
    {
        mod::Log(
            "Takeover: required per-frame recovery hook unavailable; direct startup staying passive");
        return false;
    }
    if (!frameHookReady)
    {
        mod::Log(
            "Takeover: warning - init-only dispatcher hook unavailable; active tick recovery remains installed");
    }

    if (!PatchRevivalDllExitProcess())
    {
        mod::Log("Takeover: warning - failed to patch EfzRevival ExitProcess IAT");
    }

    return true;
}

bool NeedsExternalLauncherSimulationHandoff()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool externalOnline =
        g_launchDisposition
            == revival_launch::LaunchDisposition::AdoptExternalOnline;
    const bool externalSpectator =
        g_launchDisposition
            == revival_launch::LaunchDisposition::AdoptExternalSpectator;
    return (externalOnline || externalSpectator)
        && InterlockedCompareExchange(
               &g_revivalExitIntercepted, 0, 0) == 0
        && IsExternalLauncherAttachBoundaryPending();
}

bool CompleteExternalLauncherUiAttachment(
    bool hooksInstalled,
    bool simulationHandoffReady)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool externalOnline =
        g_launchDisposition
            == revival_launch::LaunchDisposition::AdoptExternalOnline;
    const bool externalSpectator =
        g_launchDisposition
            == revival_launch::LaunchDisposition::AdoptExternalSpectator;
    const bool externalSimulation = externalOnline || externalSpectator;
    const bool externalTournament =
        g_launchDisposition
            == revival_launch::LaunchDisposition::AttachExistingTournament;
    if (!externalSimulation && !externalTournament)
    {
        return true;
    }

    uintptr_t sessionPtr = 0;
    uintptr_t vtable = 0;
    const bool attachFailed = InterlockedCompareExchange(
        &g_revivalExitIntercepted, 0, 0) != 0;
    const int expectedRole = externalOnline
        ? kLocalRoleOnline
        : (externalSpectator
            ? kLocalRoleSpectate
            : kLocalRoleTournament);
    bool exactSession = hooksInstalled && !attachFailed
        && (!externalSimulation || simulationHandoffReady)
        && g_activeRevival != nullptr
        && g_localRoleFlag == expectedRole
        && ReadRoleFlagFromRevival() == expectedRole
        && IsExternalLauncherAttachBoundaryPending();
    if (exactSession)
    {
        sessionPtr = ReadSessionPointerFromRevival();
        const uintptr_t expectedVtableRva = externalOnline
            ? g_activeRevival->onlineSessionVtableRva
            : (externalSpectator
                ? g_activeRevival->spectatorSessionVtableRva
                : g_activeRevival->tournamentSessionVtableRva);
        exactSession = sessionPtr != 0
            && sessionPtr == g_lastValidatedSessionPtr
            && SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable)
            && vtable == reinterpret_cast<uintptr_t>(g_localRevivalModule)
                + expectedVtableRva
            && IsRevivalDllExitProcessIatPatched()
            && HasNetplayPerFrameTickHookInstalled();
        if (exactSession && externalSimulation)
        {
            exactSession = g_peerProcessOwnership
                    == PeerProcessOwnership::ExternalLauncherParent
                && g_revivalProcess != nullptr
                && !HasPeerProcessExitSignal();
        }
        else if (exactSession)
        {
            exactSession = AdoptExistingTournamentExePatchState()
                && IsExternalTournamentExitGuardOwned();
        }
    }

    if (exactSession)
    {
        const bool completed =
            CompleteExternalLauncherAttachBoundary(true);
        if (completed)
        {
            mod::Log(
                "LauncherBootstrap: attached exact %s session version=%s role=%d session=0x%08lX initCalls=0 uiHooks=1 simulationSuspended=%d boundaryCommit=1",
                externalOnline
                    ? "Online"
                    : (externalSpectator ? "Spectator" : "Tournament"),
                g_activeRevival->versionTag,
                expectedRole,
                static_cast<unsigned long>(sessionPtr),
                externalSimulation ? 1 : 0);
            return true;
        }
        exactSession = false;
    }

    if (g_hostBlock != nullptr && !attachFailed)
    {
        CopyString(
            g_hostBlock->consoleErrorText,
            sizeof(g_hostBlock->consoleErrorText),
            !hooksInstalled
                ? "Launcher title/UI hook installation failed"
                : (externalSimulation && !simulationHandoffReady
                    ? "Launcher online simulation handoff failed"
                    : "Launcher session changed during UI attachment"));
        InterlockedIncrement(&g_hostBlock->consoleErrorSerial);
    }
    InterlockedExchange(
        &g_revivalExitMode,
        static_cast<LONG>(expectedRole));
    InterlockedExchange(&g_revivalExitIntercepted, 1);
    MarkRevivalSyncDiagnosticsSessionStart(
        externalSimulation
            ? "ExternalLauncherAdoption_ui_quarantined"
            : "ExternalTournamentAdoption_ui_quarantined");
    const bool completed =
        CompleteExternalLauncherAttachBoundary(false);
    mod::Log(
        "LauncherBootstrap: %s UI attachment quarantined hooks=%d simulationHandoff=%d exactSession=%d priorAttachFailure=%d boundaryQuarantine=%d",
        externalOnline
            ? "Online"
            : (externalSpectator ? "Spectator" : "Tournament"),
        hooksInstalled ? 1 : 0,
        simulationHandoffReady ? 1 : 0,
        exactSession ? 1 : 0,
        attachFailed ? 1 : 0,
        completed ? 1 : 0);
    return completed;
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
    // Also restore the DLL flag globals so other mods see local play.
    mod::Log(
        "Takeover: local re-init deferred (from role=%d -> local play, no init call)",
        g_localRoleFlag);
    SetRoleFlagDirect(kLocalRoleLocalPlay, "reinit_local_play");
    const bool titleDispatchOk =
        RestoreExeDispatchHookForTitle("ReinitLocalPlay");
    if (titleDispatchOk)
    {
        mod::Log("Takeover: local re-init restored 1.02j title dispatch");
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

bool PatchRevivalErrorCodeNullGuard()
{
    const uintptr_t base = ResolveHostRevivalBase();
    if (base == 0)
    {
        return false;
    }

    if (g_activeRevival == nullptr
        || g_activeRevival->errorCodeIsZeroRva == 0
        || g_activeRevival->errorCodeIsZeroPatchSize == 0)
    {
        mod::Log(
            "Takeover: null-guard patch not required for Revival version=%s",
            (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
                ? g_activeRevival->versionTag
                : "unknown");
        return true;
    }

    void* const stub = EnsureRevivalErrorCodeNullGuardStub();
    if (stub == nullptr)
    {
        return false;
    }

    uint8_t* const target = reinterpret_cast<uint8_t*>(base + g_activeRevival->errorCodeIsZeroRva);
    std::array<uint8_t, kMaxErrorCodePatchBytes> patchBytes = {};
    patchBytes[0] = 0xE9;
    const intptr_t delta = reinterpret_cast<uint8_t*>(stub) - (target + 5);
    const int32_t rel = static_cast<int32_t>(delta);
    std::memcpy(&patchBytes[1], &rel, sizeof(rel));
    patchBytes[5] = 0x90;
    patchBytes[6] = 0x90;
    patchBytes[7] = 0x90;

    if (g_revivalErrorCodeNullGuardPatched && g_revivalErrorCodeNullGuardPatchedBase == base)
    {
        uint8_t verify[kMaxErrorCodePatchBytes] = {};
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

    uint8_t current[kMaxErrorCodePatchBytes] = {};
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
            static_cast<unsigned long>(g_activeRevival->errorCodeIsZeroRva),
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
            static_cast<unsigned long>(g_activeRevival->errorCodeIsZeroRva),
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
            static_cast<unsigned long>(g_activeRevival->errorCodeIsZeroRva),
            ErrorString(GetLastError()).c_str());
    }

    g_revivalErrorCodeNullGuardPatched = true;
    g_revivalErrorCodeNullGuardPatchedBase = base;
    mod::Log(
        "Takeover: patched EfzRevival.dll null-guard at +0x%04lX",
        static_cast<unsigned long>(g_activeRevival->errorCodeIsZeroRva));
    return true;
}

void PublishHostRevivalBase()
{
    const uintptr_t base = ResolveHostRevivalBase();
    const uint32_t timestamp =
        (base != 0 && g_activeRevival != nullptr)
            ? g_activeRevival->peTimestamp
            : 0;
    g_hostRevivalBase = base;
    if (g_hostBlock != nullptr)
    {
        g_hostBlock->hostRevivalBase = static_cast<uint32_t>(base);
        g_hostBlock->hostRevivalTimestamp = timestamp;
    }
    if (base != 0)
    {
        mod::Log(
            "Takeover: host revival base=0x%08lX version=%s timestamp=0x%08X",
            static_cast<unsigned long>(base),
            (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
                ? g_activeRevival->versionTag
                : "unknown",
            static_cast<unsigned>(timestamp));
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

bool RecoverTemporaryHostProtocolOverride(const char* reason)
{
    std::lock_guard<std::recursive_mutex> overrideLock(
        g_temporaryHostProtocolMutex);

    // An active record belongs to this process and retains the richer
    // in-memory listener state. Normal teardown/ack paths restore it.
    if (g_temporaryHostProtocolOverride.active)
    {
        return true;
    }

    std::wstring wideIniPath = GameDirectoryWide();
    if (!wideIniPath.empty())
    {
        wideIniPath += L"\\";
    }
    wideIniPath += L"EfzRevival.ini";
    return RecoverProtocolOverrideFromMarkerUnlocked(
        wideIniPath,
        reason);
}

bool PrepareTemporaryHostProtocolOverride(
    network::NetworkFamily effectiveFamily)
{
    std::lock_guard<std::recursive_mutex> overrideLock(
        g_temporaryHostProtocolMutex);

    if (effectiveFamily != network::NetworkFamily::IPv4
        && effectiveFamily != network::NetworkFamily::IPv6)
    {
        return false;
    }

    RestoreTemporaryHostProtocolOverride(
        "superseded by new Revival netplay session Host attempt");
    if (g_temporaryHostProtocolOverride.active)
    {
        mod::Log(
            "Takeover: Revival netplay session Protocol preparation rejected "
            "because prior temporary override could not be restored");
        return false;
    }

    std::wstring wideIniPath = GameDirectoryWide();
    if (!wideIniPath.empty())
    {
        wideIniPath += L"\\";
    }
    wideIniPath += L"EfzRevival.ini";

    if (!RecoverProtocolOverrideFromMarkerUnlocked(
            wideIniPath,
            "before new Revival netplay session Host override"))
    {
        mod::Log(
            "Takeover: Revival netplay session Protocol preparation rejected "
            "because crash recovery is unresolved");
        return false;
    }

    TemporaryHostProtocolOverride pending = {};
    pending.iniPath = wideIniPath;
    pending.writtenValue = Utf8ToWide(network::FamilyName(effectiveFamily));
    pending.writtenFamily = effectiveFamily;
    if (pending.writtenValue.empty()
        || !ReadProtocolValue(
            wideIniPath,
            &pending.previousValueExisted,
            &pending.previousValue))
    {
        mod::Log(
            "Takeover: Revival netplay session Protocol snapshot failed "
            "path='%s'",
            WideToUtf8(wideIniPath).c_str());
        return false;
    }

    if (!WriteProtocolRecoveryMarker(
            wideIniPath,
            pending.previousValueExisted,
            pending.previousValue,
            pending.writtenValue))
    {
        mod::Log(
            "Takeover: Revival netplay session Protocol preparation rejected "
            "because recovery marker could not be committed");
        return false;
    }

    if (!WriteAndVerifyProtocolValue(
            wideIniPath,
            true,
            pending.writtenValue))
    {
        mod::Log(
            "Takeover: Revival netplay session temporary Protocol write failed "
            "effective=%s err=%s",
            network::FamilyName(effectiveFamily),
            ErrorString(GetLastError()).c_str());
        (void)RecoverProtocolOverrideFromMarkerUnlocked(
            wideIniPath,
            "temporary Host Protocol write failed");
        return false;
    }

    pending.active = true;
    g_temporaryHostProtocolOverride = std::move(pending);
    mod::Log(
        "Takeover: Revival netplay session temporary Protocol written "
        "effective=%s previousExisted=%d previous='%s'",
        network::FamilyName(effectiveFamily),
        g_temporaryHostProtocolOverride.previousValueExisted ? 1 : 0,
        WideToUtf8(
            g_temporaryHostProtocolOverride.previousValue).c_str());
    return true;
}

void RestoreTemporaryHostProtocolOverride(const char* reason)
{
    std::lock_guard<std::recursive_mutex> overrideLock(
        g_temporaryHostProtocolMutex);

    if (!g_temporaryHostProtocolOverride.active)
    {
        return;
    }

    bool currentExists = false;
    std::wstring currentValue;
    if (!ReadProtocolValue(
            g_temporaryHostProtocolOverride.iniPath,
            &currentExists,
            &currentValue))
    {
        mod::Log(
            "Takeover: Revival netplay session Protocol restore deferred "
            "reason='%s' readCurrent=0",
            reason != nullptr ? reason : "");
        return;
    }

    // Do not overwrite a preference edited after this Host attempt started.
    if (!currentExists
        || currentValue != g_temporaryHostProtocolOverride.writtenValue)
    {
        mod::Log(
            "Takeover: Revival netplay session Protocol restore skipped "
            "reason='%s' currentChanged=1 current='%s' expected='%s'",
            reason != nullptr ? reason : "",
            WideToUtf8(currentValue).c_str(),
            WideToUtf8(g_temporaryHostProtocolOverride.writtenValue).c_str());
        if (!RemoveProtocolRecoveryMarker(
                ProtocolRecoveryMarkerPath(
                    g_temporaryHostProtocolOverride.iniPath),
                reason))
        {
            return;
        }
        g_temporaryHostProtocolOverride = {};
        return;
    }

    if (!WriteAndVerifyProtocolValue(
            g_temporaryHostProtocolOverride.iniPath,
            g_temporaryHostProtocolOverride.previousValueExisted,
            g_temporaryHostProtocolOverride.previousValue))
    {
        mod::Log(
            "Takeover: Revival netplay session Protocol restore or "
            "verification failed "
            "reason='%s' err=%s",
            reason != nullptr ? reason : "",
            ErrorString(GetLastError()).c_str());
        return;
    }

    if (!RemoveProtocolRecoveryMarker(
            ProtocolRecoveryMarkerPath(
                g_temporaryHostProtocolOverride.iniPath),
            reason))
    {
        return;
    }

    mod::Log(
        "Takeover: Revival netplay session Protocol preference restored "
        "reason='%s' previousExisted=%d previous='%s' temporary='%s'",
        reason != nullptr ? reason : "",
        g_temporaryHostProtocolOverride.previousValueExisted ? 1 : 0,
        WideToUtf8(g_temporaryHostProtocolOverride.previousValue).c_str(),
        WideToUtf8(g_temporaryHostProtocolOverride.writtenValue).c_str());
    g_temporaryHostProtocolOverride = {};
}

void HandleTemporaryHostProtocolListenerAck()
{
    const LONG expectedPort =
        g_hostBlock != nullptr
            ? InterlockedCompareExchange(
                  &g_hostBlock->hostExpectedListenerPort,
                  0,
                  0)
            : 0;
    HandleTemporaryHostProtocolListenerAck(
        g_revivalProcessId,
        expectedPort > 0 && expectedPort <= 65535
            ? static_cast<uint16_t>(expectedPort)
            : 0);
}

static void HandleTemporaryHostProtocolListenerAckImpl(
    DWORD expectedProcessId,
    uint16_t expectedPort,
    bool allowImmediateHandoffAck)
{
    std::lock_guard<std::recursive_mutex> overrideLock(
        g_temporaryHostProtocolMutex);

    if (!g_temporaryHostProtocolOverride.active)
    {
        return;
    }

    LONG serial = 0;
    network::NetworkFamily observedFamily = network::NetworkFamily::IPv4;
    uint16_t observedPort = 0;
    DWORD observedProcessId = 0;
    if (!ReadHostListenerObservation(
            &serial,
            &observedFamily,
            &observedPort,
            &observedProcessId))
    {
        return;
    }

    const bool familyMatches =
        observedFamily == g_temporaryHostProtocolOverride.writtenFamily;
    const bool processMatches =
        expectedProcessId != 0
        && observedProcessId == expectedProcessId;
    const bool portMatches =
        expectedPort != 0
        && observedPort == expectedPort;
    if (!familyMatches || !processMatches || !portMatches)
    {
        if (serial
            != g_temporaryHostProtocolOverride
                   .lastRejectedListenerSerial)
        {
            g_temporaryHostProtocolOverride
                .lastRejectedListenerSerial = serial;
            mod::Log(
                "Takeover: Revival netplay session listener Protocol "
                "candidate rejected serial=%ld observedFamily=%s "
                "observedPort=%u observedPid=%lu expectedFamily=%s "
                "expectedPort=%u expectedPid=%lu familyMatch=%d "
                "portMatch=%d processMatch=%d",
                static_cast<long>(serial),
                network::FamilyName(observedFamily),
                static_cast<unsigned>(observedPort),
                static_cast<unsigned long>(observedProcessId),
                network::FamilyName(
                    g_temporaryHostProtocolOverride.writtenFamily),
                static_cast<unsigned>(expectedPort),
                static_cast<unsigned long>(expectedProcessId),
                familyMatches ? 1 : 0,
                portMatches ? 1 : 0,
                processMatches ? 1 : 0);
        }
        g_temporaryHostProtocolOverride.listenerCandidateSerial = 0;
        g_temporaryHostProtocolOverride.listenerCandidateProcessId = 0;
        g_temporaryHostProtocolOverride.listenerCandidateFamily =
            network::NetworkFamily::IPv4;
        g_temporaryHostProtocolOverride.listenerCandidatePort = 0;
        g_temporaryHostProtocolOverride.listenerCandidateFirstTick = 0;
        return;
    }

    const DWORD nowTick = GetTickCount();
    const bool sameCandidate =
        g_temporaryHostProtocolOverride.listenerCandidateSerial
                == serial
        && g_temporaryHostProtocolOverride
               .listenerCandidateProcessId
                == observedProcessId
        && g_temporaryHostProtocolOverride.listenerCandidateFamily
                == observedFamily
        && g_temporaryHostProtocolOverride.listenerCandidatePort
                == observedPort;
    if (!sameCandidate)
    {
        g_temporaryHostProtocolOverride.listenerCandidateSerial =
            serial;
        g_temporaryHostProtocolOverride.listenerCandidateProcessId =
            observedProcessId;
        g_temporaryHostProtocolOverride.listenerCandidateFamily =
            observedFamily;
        g_temporaryHostProtocolOverride.listenerCandidatePort =
            observedPort;
        g_temporaryHostProtocolOverride.listenerCandidateFirstTick =
            nowTick;
        mod::Log(
            "Takeover: Revival netplay session listener Protocol "
            "candidate matched serial=%ld family=%s port=%u pid=%lu "
            "debounceMs=100",
            static_cast<long>(serial),
            network::FamilyName(observedFamily),
            static_cast<unsigned>(observedPort),
            static_cast<unsigned long>(observedProcessId));
        if (!allowImmediateHandoffAck)
        {
            return;
        }
    }

    if (!allowImmediateHandoffAck
        && nowTick
            - g_temporaryHostProtocolOverride
                  .listenerCandidateFirstTick
        < 100u)
    {
        return;
    }

    mod::Log(
        "Takeover: Revival netplay session listener Protocol ack accepted "
        "serial=%ld family=%s port=%u pid=%lu handoff=%d",
        static_cast<long>(serial),
        network::FamilyName(observedFamily),
        static_cast<unsigned>(observedPort),
        static_cast<unsigned long>(observedProcessId),
        allowImmediateHandoffAck ? 1 : 0);
    RestoreTemporaryHostProtocolOverride(
        "stable matching Revival netplay session listener acknowledgement");
}

bool CanTerminatePeerProcess()
{
    return g_peerProcessOwnership == PeerProcessOwnership::SpawnedHelper
        || g_peerProcessOwnership
            == PeerProcessOwnership::ExternalLauncherParent;
}

bool IsExternalLauncherPeerProcess()
{
    return g_peerProcessOwnership
        == PeerProcessOwnership::ExternalLauncherParent;
}

BOOL TerminatePeerProcessIfOwned(
    UINT exitCode,
    const char* context,
    bool waitForExit)
{
    if (g_revivalProcess == nullptr)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!CanTerminatePeerProcess())
    {
        mod::Log(
            "Takeover: refused to terminate unowned Revival process pid=%lu context=%s",
            static_cast<unsigned long>(g_revivalProcessId),
            context != nullptr ? context : "unknown");
        return TRUE;
    }

    const bool externalLauncherParent =
        g_peerProcessOwnership
            == PeerProcessOwnership::ExternalLauncherParent;
    if (!externalLauncherParent)
    {
        return TerminateProcess(g_revivalProcess, exitCode);
    }

    // The launcher owns Revival's native singleton for the lifetime of the
    // process.  Merely forgetting our HANDLE leaves that singleton live and
    // can make every later Host/Join/Spectate start fail.  Terminate only the
    // exact parent generation admitted by the launcher guard; never fall back
    // to a PID-only or window-name kill.
    const DWORD handlePid = GetProcessId(g_revivalProcess);
    if (g_revivalProcessId == 0
        || handlePid == 0
        || handlePid != g_revivalProcessId)
    {
        const DWORD error = handlePid == 0
            ? GetLastError()
            : ERROR_INVALID_PARAMETER;
        mod::Log(
            "Takeover: refused external launcher termination due to handle/PID mismatch expected=%lu actual=%lu context=%s err=%lu",
            static_cast<unsigned long>(g_revivalProcessId),
            static_cast<unsigned long>(handlePid),
            context != nullptr ? context : "unknown",
            static_cast<unsigned long>(error));
        SetLastError(error);
        return FALSE;
    }

    DWORD observedExitCode = STILL_ACTIVE;
    if (GetExitCodeProcess(g_revivalProcess, &observedExitCode)
        && observedExitCode != STILL_ACTIVE)
    {
        g_launchDisposition =
            revival_launch::LaunchDisposition::DirectGameHost;
        mod::Log(
            "Takeover: exact external Revival parent already exited pid=%lu code=%lu context=%s",
            static_cast<unsigned long>(g_revivalProcessId),
            static_cast<unsigned long>(observedExitCode),
            context != nullptr ? context : "unknown");
        return TRUE;
    }

    if (!HasActiveExactExternalLauncherParent())
    {
        mod::Log(
            "Takeover: refused external launcher termination without exact active guard pid=%lu context=%s",
            static_cast<unsigned long>(g_revivalProcessId),
            context != nullptr ? context : "unknown");
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    // Once session teardown owns this generation, no late blocked cleanup
    // serial from the dying launcher may be delivered to a later session.
    RetireExternalLauncherGuardSignalDelivery();

    if (!TerminateProcess(g_revivalProcess, exitCode))
    {
        const DWORD terminateError = GetLastError();
        if (GetExitCodeProcess(g_revivalProcess, &observedExitCode)
            && observedExitCode != STILL_ACTIVE)
        {
            g_launchDisposition =
                revival_launch::LaunchDisposition::DirectGameHost;
            return TRUE;
        }
        SetLastError(terminateError);
        return FALSE;
    }

    if (!waitForExit)
    {
        g_launchDisposition =
            revival_launch::LaunchDisposition::DirectGameHost;
        return TRUE;
    }

    // TerminateProcess is asynchronous.  Wait for the process object to be
    // signaled so the native singleton is actually released before the user
    // can queue the next online session.
    constexpr DWORD kExternalLauncherExitWaitMs = 2000u;
    const DWORD waitResult =
        WaitForSingleObject(g_revivalProcess, kExternalLauncherExitWaitMs);
    if (waitResult != WAIT_OBJECT_0)
    {
        const DWORD waitError = waitResult == WAIT_TIMEOUT
            ? ERROR_TIMEOUT
            : GetLastError();
        mod::Log(
            "Takeover: exact external Revival parent exit wait failed pid=%lu wait=%lu context=%s err=%lu",
            static_cast<unsigned long>(g_revivalProcessId),
            static_cast<unsigned long>(waitResult),
            context != nullptr ? context : "unknown",
            static_cast<unsigned long>(waitError));
        SetLastError(waitError);
        return FALSE;
    }

    mod::Log(
        "Takeover: exact external Revival parent terminated pid=%lu context=%s",
        static_cast<unsigned long>(g_revivalProcessId),
        context != nullptr ? context : "unknown");
    g_launchDisposition =
        revival_launch::LaunchDisposition::DirectGameHost;
    return TRUE;
}

bool ReleasePeerProcessAfterTerminationAttempt(
    BOOL terminationSucceeded,
    NetbridgeStatus* status,
    const char* context,
    bool waitForPeerWatcher)
{
    if (g_peerProcessOwnership
            == PeerProcessOwnership::ExternalLauncherParent
        && !terminationSucceeded)
    {
        // Keep the only PROCESS_TERMINATE-capable exact-generation handle and
        // the guard ownership live.  A later start must refuse/retry rather
        // than colliding with the singleton of a launcher that did not exit.
        mod::Log(
            "Takeover: retaining exact external Revival parent after failed termination pid=%lu context=%s",
            static_cast<unsigned long>(g_revivalProcessId),
            context != nullptr ? context : "unknown");
        return false;
    }

    CloseProcessHandle(status, waitForPeerWatcher);
    return true;
}

void StopPeerProcessExitWatch(bool waitForExit)
{
    HANDLE watcherThread = nullptr;
    HANDLE stopEvent = nullptr;
    HANDLE processHandle = nullptr;
    std::unique_lock<std::mutex> lock(
        g_peerProcessExitWatchMutex, std::defer_lock);
    if (waitForExit)
    {
        lock.lock();
    }
    else if (!lock.try_lock())
    {
        // Process-termination detach runs under loader lock. Never block on a
        // mutex another suspended thread may own; signal the current wait as a
        // best effort and let process teardown reclaim its HANDLEs.
        HANDLE observedStopEvent = g_peerProcessExitWatchStopEvent;
        if (observedStopEvent != nullptr)
        {
            (void)SetEvent(observedStopEvent);
        }
        InterlockedExchange(&g_peerProcessExitSignaled, 0);
        return;
    }

    watcherThread = g_peerProcessExitWatchThread;
    stopEvent = g_peerProcessExitWatchStopEvent;
    processHandle = g_peerProcessExitWatchHandle;
    if (stopEvent != nullptr)
    {
        (void)SetEvent(stopEvent);
    }
    g_peerProcessExitWatchThread = nullptr;
    g_peerProcessExitWatchStopEvent = nullptr;
    g_peerProcessExitWatchHandle = nullptr;
    lock.unlock();

    if (watcherThread != nullptr && waitForExit)
    {
        (void)WaitForSingleObject(watcherThread, INFINITE);
    }
    if (watcherThread != nullptr)
    {
        CloseHandle(watcherThread);
    }
    if (waitForExit)
    {
        if (processHandle != nullptr)
        {
            CloseHandle(processHandle);
        }
        if (stopEvent != nullptr)
        {
            CloseHandle(stopEvent);
        }
    }
    InterlockedExchange(&g_peerProcessExitSignaled, 0);
}

bool StartPeerProcessExitWatch()
{
    StopPeerProcessExitWatch(true);
    if (g_revivalProcess == nullptr)
    {
        return false;
    }

    HANDLE processHandle = nullptr;
    if (!DuplicateHandle(
            GetCurrentProcess(),
            g_revivalProcess,
            GetCurrentProcess(),
            &processHandle,
            SYNCHRONIZE,
            FALSE,
            0))
    {
        mod::Log(
            "Takeover: helper process-exit watch duplicate failed err=%lu",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    HANDLE stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent == nullptr)
    {
        const DWORD error = GetLastError();
        CloseHandle(processHandle);
        mod::Log(
            "Takeover: helper process-exit watch event failed err=%lu",
            static_cast<unsigned long>(error));
        return false;
    }

    // Clear before launching: an already-terminated process can wake the new
    // thread immediately, and clearing afterward would erase that edge.
    InterlockedExchange(&g_peerProcessExitSignaled, 0);
    HANDLE watcherThread = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_peerProcessExitWatchMutex);
        g_peerProcessExitWatchContext.stopEvent = stopEvent;
        g_peerProcessExitWatchContext.processHandle = processHandle;
        watcherThread = CreateThread(
            nullptr,
            0,
            PeerProcessExitWatchMain,
            &g_peerProcessExitWatchContext,
            0,
            nullptr);
        if (watcherThread == nullptr)
        {
            const DWORD error = GetLastError();
            CloseHandle(processHandle);
            CloseHandle(stopEvent);
            mod::Log(
                "Takeover: helper process-exit watch thread creation failed err=%lu",
                static_cast<unsigned long>(error));
            return false;
        }
        g_peerProcessExitWatchThread = watcherThread;
        g_peerProcessExitWatchStopEvent = stopEvent;
        g_peerProcessExitWatchHandle = processHandle;
    }

    return true;
}

bool HasPeerProcessExitSignal()
{
    return InterlockedCompareExchange(
               &g_peerProcessExitSignaled, 0, 0) != 0;
}

void HandleTemporaryHostProtocolListenerAck(
    DWORD expectedProcessId,
    uint16_t expectedPort)
{
    HandleTemporaryHostProtocolListenerAckImpl(
        expectedProcessId, expectedPort, false);
}

void ConfirmTemporaryHostProtocolListenerAckAtHandoff(
    DWORD expectedProcessId,
    uint16_t expectedPort)
{
    HandleTemporaryHostProtocolListenerAckImpl(
        expectedProcessId, expectedPort, true);
}

bool GetHostProtocolOverrideState(
    HostProtocolOverrideState* outState)
{
    if (outState == nullptr)
    {
        return false;
    }

    std::lock_guard<std::recursive_mutex> overrideLock(
        g_temporaryHostProtocolMutex);
    CopyHostProtocolOverrideStateUnlocked(outState);
    return true;
}

bool BeginOptionsIniAccess(
    bool writeAccess,
    HostProtocolOverrideState* outState)
{
    if (outState == nullptr)
    {
        return false;
    }

    g_temporaryHostProtocolMutex.lock();
    if (!g_temporaryHostProtocolOverride.active)
    {
        std::wstring wideIniPath = GameDirectoryWide();
        if (!wideIniPath.empty())
        {
            wideIniPath += L"\\";
        }
        wideIniPath += L"EfzRevival.ini";
        if (!RecoverProtocolOverrideFromMarkerUnlocked(
                wideIniPath,
                "Options EfzRevival.ini access"))
        {
            g_temporaryHostProtocolMutex.unlock();
            return false;
        }
    }

    CopyHostProtocolOverrideStateUnlocked(outState);
    if (writeAccess
        && g_temporaryHostProtocolOverride.active)
    {
        g_temporaryHostProtocolMutex.unlock();
        return false;
    }
    return true;
}

void EndOptionsIniAccess()
{
    g_temporaryHostProtocolMutex.unlock();
}

bool WriteIni(
    int role,
    uint16_t port,
    const char* address,
    const char* nickname,
    bool writeNicknameToIni,
    network::NetworkFamily sessionFamily,
    bool writeHostProtocol)
{
    std::lock_guard<std::recursive_mutex> overrideLock(
        g_temporaryHostProtocolMutex);

    std::wstring wideIniPath = GameDirectoryWide();
    if (!wideIniPath.empty())
    {
        wideIniPath += L"\\";
    }
    wideIniPath += L"EfzRevival.ini";

    std::string iniPath = WideToUtf8(wideIniPath);
    if (iniPath.empty())
    {
        iniPath = "EfzRevival.ini";
    }

    const DWORD existingAttrs = GetFileAttributesW(wideIniPath.c_str());
    const bool existed = (existingAttrs != INVALID_FILE_ATTRIBUTES);
    const char* safeAddress = (address != nullptr) ? address : "";
    const char* safeNickname = (nickname != nullptr) ? nickname : "";
    const bool hasNickname = (safeNickname[0] != '\0');
    bool wroteNickname = false;

    char portText[16] = {};
    std::snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));

    auto writeIniKeyUtf8 = [&wideIniPath, &iniPath](
                               const wchar_t* sectionW,
                               const wchar_t* keyW,
                               const char* sectionA,
                               const char* keyA,
                               const std::string& valueUtf8) -> bool {
        const std::wstring wideValue = Utf8ToWide(valueUtf8);
        if (!valueUtf8.empty() && wideValue.empty())
        {
            mod::Log(
                "Takeover: WriteIni key conversion failed section='%s' key='%s' value='%s' path='%s'",
                sectionA != nullptr ? sectionA : "",
                keyA != nullptr ? keyA : "",
                valueUtf8.c_str(),
                iniPath.c_str());
            return false;
        }

        if (WritePrivateProfileStringW(
                sectionW,
                keyW,
                wideValue.c_str(),
                wideIniPath.c_str())
            == FALSE)
        {
            mod::Log(
                "Takeover: WriteIni key failed section='%s' key='%s' value='%s' err=%s",
                sectionA != nullptr ? sectionA : "",
                keyA != nullptr ? keyA : "",
                valueUtf8.c_str(),
                ErrorString(GetLastError()).c_str());
            return false;
        }
        return true;
    };
    bool ok = true;
    bool wroteProtocol = false;

    if (writeHostProtocol)
    {
        if (role != static_cast<int>(NetbridgeRole::Host))
        {
            mod::Log(
                "Takeover: Revival netplay session Protocol write rejected "
                "for non-Host role=%d",
                role);
            return false;
        }

        wroteProtocol =
            g_temporaryHostProtocolOverride.active
            && g_temporaryHostProtocolOverride.writtenFamily
                == sessionFamily
            && g_temporaryHostProtocolOverride.iniPath == wideIniPath;
        if (!wroteProtocol)
        {
            mod::Log(
                "Takeover: Revival netplay session Protocol preflight missing "
                "or mismatched family=%s",
                network::FamilyName(sessionFamily));
            return false;
        }
    }

    if (writeNicknameToIni && hasNickname)
    {
        const bool nameWriteOk =
            writeIniKeyUtf8(L"Network", L"Name", "Network", "Name", safeNickname);
        wroteNickname = nameWriteOk;
        ok = nameWriteOk && ok;
    }
    else if (writeNicknameToIni)
    {
        mod::Log("Takeover: WriteIni preserving existing Network.Name (empty nickname supplied)");
    }
    else
    {
        mod::Log("Takeover: WriteIni preserving existing Network.Name (caller disabled nickname sync)");
    }

    // Keep user INI intact. Update only the settings currently supported by
    // InGameNetplay menu integration.
    bool wroteAddress = false;
    if (safeAddress[0] != '\0')
    {
        wroteAddress = writeIniKeyUtf8(
            L"Network",
            L"Address",
            "Network",
            "Address",
            safeAddress);
        ok = wroteAddress && ok;
    }
    ok = writeIniKeyUtf8(L"Network", L"Port", "Network", "Port", portText) && ok;

    if (!ok)
    {
        RestoreTemporaryHostProtocolOverride(
            "Revival netplay session INI write failed");
    }

    mod::Log(
        "Takeover: WriteIni path='%s' existed=%d role=%d port=%u nickname='%s' "
        "address='%s' result=%d writeNicknameToIni=%d wroteNickname=%d "
        "wroteAddress=%d wrotePort=1 wroteProtocol=%d protocol=%s",
        iniPath.c_str(),
        existed ? 1 : 0,
        role,
        static_cast<unsigned>(port),
        safeNickname,
        safeAddress,
        ok ? 1 : 0,
        writeNicknameToIni ? 1 : 0,
        wroteNickname ? 1 : 0,
        wroteAddress ? 1 : 0,
        wroteProtocol ? 1 : 0,
        writeHostProtocol ? network::FamilyName(sessionFamily) : "preserved");
    return ok;
}

bool IsCurrentProcessRevival()
{
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
    {
        return false;
    }
    return BaseLower(path) == "efzrevival.exe";
}

bool IsRunningUnderWine()
{
    // Wine exposes wine_get_version() from ntdll.dll.  Probing for this
    // export is the canonical way to detect Wine at runtime.  This does
    // not exist on native Windows, so GetProcAddress returns nullptr.
    static int cached = -1;
    if (cached >= 0)
    {
        return cached != 0;
    }
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    cached = (ntdll != nullptr
              && GetProcAddress(ntdll, "wine_get_version") != nullptr) ? 1 : 0;
    if (cached != 0)
    {
        mod::Log("Platform: running under Wine/Proton");
    }
    return cached != 0;
}

void InitializeInjected()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    DetectRevivalVersion();
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
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
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
    const bool ipcReady =
        g_injectedBlock != nullptr
        && g_injectedInitEvent != nullptr
        && g_injectedConsoleEvent != nullptr;
    g_injectedReady = false;
    if (ipcReady)
    {
        // IAT capture lives in this helper process. Its globals are not shared
        // with the host DLL, so start the local ring consumer before Revival
        // can publish listener, prompt, or error text through the stubs.
        g_injectedReady = StartConsoleCaptureWorker(true);
        if (g_injectedReady)
        {
            DetectRevivalVersion();
            HMODULE revival = GetModuleHandleA("EfzRevival.dll");
            if (revival != nullptr)
            {
                const FARPROC initProc = GetProcAddress(revival, "init");
                g_injectedInitAddress = reinterpret_cast<uintptr_t>(initProc);
            }
        }
        else
        {
            mod::Log(
                "Takeover: injected capture worker unavailable; helper context remains unready");
        }
        InterlockedExchange(
            &g_injectedBlock->helperCaptureReady,
            g_injectedReady ? 1 : 0);
    }
    mod::Log(
        "Takeover: injected initialized ready=%d block=0x%p init=0x%p console=0x%p initAddr=0x%p hostRevivalBase=0x%08lX hostRevivalTimestamp=0x%08X version=%s",
        g_injectedReady ? 1 : 0,
        g_injectedBlock,
        g_injectedInitEvent,
        g_injectedConsoleEvent,
        reinterpret_cast<void*>(g_injectedInitAddress),
        static_cast<unsigned long>(g_injectedBlock != nullptr ? g_injectedBlock->hostRevivalBase : 0),
        static_cast<unsigned>(g_injectedBlock != nullptr ? g_injectedBlock->hostRevivalTimestamp : 0),
        (g_activeRevival != nullptr && g_activeRevival->versionTag != nullptr)
            ? g_activeRevival->versionTag
            : "unknown");
    LogRevival102jDeepStep("HelperIpc.initialize_complete");
    InterlockedExchange(&g_injectedLazyBootstrapState, g_injectedReady ? 2 : 0);
}

void ShutdownInjected()
{
    // DllMain/process-detach path: stop new ring reservations and request an
    // emergency worker exit without waiting under loader lock.
    StopManagedLogEfzWorker(false);
    std::lock_guard<std::mutex> lock(g_mutex);
    LogRevival102jDeepStep("HelperIpc.shutdown_begin");
    ClearFakeThreads();
    ClearRedirectAllocations();

    if (g_injectedBlock != nullptr)
    {
        InterlockedExchange(&g_injectedBlock->helperCaptureReady, 0);
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
    g_injectedSpectateConfirmPromptWaitStartTick = 0;
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

} // namespace netplay::bridge::takeover
