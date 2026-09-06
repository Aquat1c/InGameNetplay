// Narrow parent-process guard for sessions started by an external
// EfzRevival.exe.  This translation unit intentionally has no logger, console,
// or ordinary injected-helper bootstrap dependencies beyond InjectSelf().

#include "netplay/bridge/external_launcher_guard.h"
#include "netplay/bridge/takeover_internal.h"

#include <cstdint>
#include <cstring>

#include <tlhelp32.h>
#include <windows.h>

extern "C" __declspec(dllexport) BOOL WINAPI
nb_stub_TerminateProcess(HANDLE, UINT);
extern "C" __declspec(dllexport) DWORD WINAPI
nb_activate_external_launcher_guard(LPVOID);

namespace netplay::bridge::takeover
{
namespace
{
constexpr uint32_t kGuardMagic = 0x47584C45u; // "ELXG"
constexpr uint32_t kGuardVersion = 2u;
constexpr DWORD kGuardActivationTimeoutMs = 5000u;
constexpr LONG kGuardPending = 0;
constexpr LONG kGuardActive = 1;
constexpr LONG kGuardCancelled = 2;
constexpr LONG kGuardFailed = 3;
constexpr LONG kGuardRoleNone = 0;
constexpr LONG kGuardRoleChild = 1;
constexpr LONG kGuardRoleParent = 2;
constexpr LONG kGuardRoleChildClosing = 3;

struct ExternalLauncherGuardMarker
{
    uint32_t magic;
    uint32_t version;
    uint32_t structureSize;
    DWORD parentProcessId;
    DWORD parentCreationTimeLow;
    DWORD parentCreationTimeHigh;
    DWORD childProcessId;
    uint32_t launcherExeTimestamp;
    uint32_t launcherExeSizeOfImage;
    uint32_t launcherExeEntryPointRva;
    uint32_t revivalDllTimestamp;
    uint32_t expectedCleanupReturnRva;
    volatile LONG activationState;
    volatile LONG terminateBlockedSerial;
};

bool ValidateMarkerHeader(const ExternalLauncherGuardMarker* marker)
{
    return marker != nullptr
        && marker->magic == kGuardMagic
        && marker->version == kGuardVersion
        && marker->structureSize == sizeof(*marker)
        && marker->parentProcessId != 0
        && marker->childProcessId != 0
        && marker->launcherExeTimestamp != 0
        && marker->launcherExeSizeOfImage != 0
        && marker->launcherExeEntryPointRva != 0
        && marker->revivalDllTimestamp != 0
        && marker->expectedCleanupReturnRva >= 6u;
}

volatile LONG g_guardRole = kGuardRoleNone;
HANDLE g_guardMapHandle = nullptr;
HANDLE g_guardReadyEvent = nullptr;
ExternalLauncherGuardMarker* g_guardMarker = nullptr;
volatile LONG g_childConsumedTerminateSerial = 0;
volatile LONG g_childSignalDeliveryEnabled = 0;
volatile PVOID g_childExactParentProcess = nullptr;
volatile PVOID g_childLiveMarker = nullptr;
volatile LONG g_childMarkerReaders = 0;

constexpr LONG kDeferredChildCleanupNone = 0;
constexpr LONG kDeferredChildCleanupReady = 1;
constexpr LONG kDeferredChildCleanupClaimed = 2;

struct ChildGuardResources
{
    ExternalLauncherGuardMarker* marker = nullptr;
    HANDLE mapHandle = nullptr;
    HANDLE readyEvent = nullptr;
    HANDLE exactParentProcess = nullptr;
};

ChildGuardResources g_deferredChildResources = {};
volatile LONG g_deferredChildCleanupState = kDeferredChildCleanupNone;

uintptr_t g_guardExeBase = 0;
uint32_t* g_guardTerminateThunk = nullptr;
uint32_t g_guardOriginalTerminateTarget = 0;

bool SameFileTime(const FILETIME& left, const FILETIME& right)
{
    return left.dwLowDateTime == right.dwLowDateTime
        && left.dwHighDateTime == right.dwHighDateTime;
}

bool QueryCreationTime(HANDLE process, FILETIME* creationTime)
{
    if (process == nullptr || creationTime == nullptr)
    {
        return false;
    }

    FILETIME exitTime = {};
    FILETIME kernelTime = {};
    FILETIME userTime = {};
    return GetProcessTimes(
               process,
               creationTime,
               &exitTime,
               &kernelTime,
               &userTime)
        != FALSE;
}

ExternalLauncherGuardMarker* AcquireChildMarker(bool requireDelivery)
{
    InterlockedIncrement(&g_childMarkerReaders);
    MemoryBarrier();

    ExternalLauncherGuardMarker* marker = nullptr;
    if (InterlockedCompareExchange(
            &g_guardRole,
            kGuardRoleNone,
            kGuardRoleNone) == kGuardRoleChild
        && (!requireDelivery
            || InterlockedCompareExchange(
                   &g_childSignalDeliveryEnabled,
                   0,
                   0) != 0))
    {
        marker = static_cast<ExternalLauncherGuardMarker*>(
            InterlockedCompareExchangePointer(
                &g_childLiveMarker,
                nullptr,
                nullptr));
    }

    if (marker == nullptr
        || (requireDelivery
            && InterlockedCompareExchange(
                   &g_childSignalDeliveryEnabled,
                   0,
                   0) == 0))
    {
        InterlockedDecrement(&g_childMarkerReaders);
        return nullptr;
    }

    return marker;
}

void ReleaseChildMarker()
{
    InterlockedDecrement(&g_childMarkerReaders);
}

bool IsExactParentAlive(
    ExternalLauncherGuardMarker* marker,
    HANDLE exactParentProcess,
    bool requireActiveMarker)
{
    if (marker == nullptr || exactParentProcess == nullptr)
    {
        return false;
    }
    if (requireActiveMarker
        && InterlockedCompareExchange(
               &marker->activationState,
               kGuardPending,
               kGuardPending) != kGuardActive)
    {
        return false;
    }

    FILETIME expectedCreationTime = {};
    expectedCreationTime.dwLowDateTime =
        marker->parentCreationTimeLow;
    expectedCreationTime.dwHighDateTime =
        marker->parentCreationTimeHigh;
    FILETIME actualCreationTime = {};
    return GetProcessId(exactParentProcess) == marker->parentProcessId
        && WaitForSingleObject(exactParentProcess, 0) == WAIT_TIMEOUT
        && QueryCreationTime(
            exactParentProcess,
            &actualCreationTime)
        && SameFileTime(actualCreationTime, expectedCreationTime);
}

void BuildGuardObjectName(
    char* output,
    size_t outputSize,
    const char* kind,
    DWORD parentProcessId)
{
    if (output == nullptr || outputSize == 0)
    {
        return;
    }

    output[0] = '\0';
    if (kind == nullptr)
    {
        return;
    }

    // This also runs from DllMain in the parent.  Keep it entirely within
    // this module instead of calling wsprintfA/User32 while the loader lock is
    // held.
    constexpr char prefix[] =
        "Local\\EFZNetbridge_ExternalLauncherGuard_";
    constexpr char hex[] = "0123456789ABCDEF";
    size_t prefixLength = 0;
    while (prefix[prefixLength] != '\0')
    {
        ++prefixLength;
    }
    size_t kindLength = 0;
    while (kind[kindLength] != '\0')
    {
        ++kindLength;
    }
    const size_t required = prefixLength + kindLength + 1u + 8u + 1u;
    if (outputSize < required)
    {
        return;
    }

    size_t cursor = 0;
    for (size_t index = 0; index < prefixLength; ++index)
    {
        output[cursor++] = prefix[index];
    }
    for (size_t index = 0; index < kindLength; ++index)
    {
        output[cursor++] = kind[index];
    }
    output[cursor++] = '_';
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        output[cursor++] = hex[(parentProcessId >> shift) & 0x0Fu];
    }
    output[cursor] = '\0';
}

bool IsKernelImportProvider(const char* name)
{
    if (name == nullptr)
    {
        return false;
    }

    return _stricmp(name, "KERNEL32.dll") == 0
        || _stricmp(name, "KERNELBASE.dll") == 0
        || _strnicmp(name, "api-ms-win-core-", 16) == 0
        || _strnicmp(name, "ext-ms-win-", 11) == 0;
}

bool ReadRemoteExact(
    HANDLE process,
    uintptr_t address,
    void* output,
    size_t size)
{
    if (process == nullptr || address == 0 || output == nullptr || size == 0)
    {
        return false;
    }

    SIZE_T bytesRead = 0;
    return ReadProcessMemory(
               process,
               reinterpret_cast<const void*>(address),
               output,
               size,
               &bytesRead)
            != FALSE
        && bytesRead == size;
}

bool FindRemoteLauncherBase(DWORD processId, uintptr_t* outputBase)
{
    if (processId == 0 || outputBase == nullptr)
    {
        return false;
    }

    *outputBase = 0;
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            processId);
        if (snapshot != INVALID_HANDLE_VALUE
            || GetLastError() != ERROR_BAD_LENGTH)
        {
            break;
        }
        Sleep(1);
    }
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    MODULEENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    // Module32First is the target's main executable.  Admission already
    // exact-hashed that process, so keying on a mutable filename here would
    // unnecessarily reject an otherwise exact renamed launcher.
    const bool found = Module32First(snapshot, &entry) != FALSE
        && (*outputBase = reinterpret_cast<uintptr_t>(entry.modBaseAddr)) != 0;

    CloseHandle(snapshot);
    return found;
}

bool FindRemoteTerminateThunk(
    HANDLE process,
    uintptr_t imageBase,
    const IMAGE_NT_HEADERS32& nt,
    uintptr_t* outputThunk)
{
    if (outputThunk == nullptr)
    {
        return false;
    }
    *outputThunk = 0;

    const DWORD importRva =
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
            .VirtualAddress;
    if (importRva == 0 || importRva >= nt.OptionalHeader.SizeOfImage)
    {
        return false;
    }

    for (DWORD descriptorIndex = 0; descriptorIndex < 256;
         ++descriptorIndex)
    {
        IMAGE_IMPORT_DESCRIPTOR descriptor = {};
        if (!ReadRemoteExact(
                process,
                imageBase + importRva
                    + descriptorIndex * sizeof(descriptor),
                &descriptor,
                sizeof(descriptor)))
        {
            return false;
        }
        if (descriptor.Name == 0)
        {
            return false;
        }
        if (descriptor.Name >= nt.OptionalHeader.SizeOfImage
            || descriptor.FirstThunk >= nt.OptionalHeader.SizeOfImage)
        {
            return false;
        }

        char provider[64] = {};
        if (!ReadRemoteString(
                process,
                imageBase + descriptor.Name,
                provider,
                sizeof(provider)))
        {
            return false;
        }
        if (!IsKernelImportProvider(provider))
        {
            continue;
        }

        const DWORD namesRva = descriptor.OriginalFirstThunk != 0
            ? descriptor.OriginalFirstThunk
            : descriptor.FirstThunk;
        if (namesRva >= nt.OptionalHeader.SizeOfImage)
        {
            return false;
        }

        for (DWORD thunkIndex = 0; thunkIndex < 4096; ++thunkIndex)
        {
            IMAGE_THUNK_DATA32 nameThunk = {};
            if (!ReadRemoteExact(
                    process,
                    imageBase + namesRva
                        + thunkIndex * sizeof(nameThunk),
                    &nameThunk,
                    sizeof(nameThunk)))
            {
                return false;
            }
            if (nameThunk.u1.AddressOfData == 0)
            {
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL32(nameThunk.u1.Ordinal))
            {
                continue;
            }
            if (nameThunk.u1.AddressOfData >= nt.OptionalHeader.SizeOfImage)
            {
                return false;
            }

            char importName[64] = {};
            if (!ReadRemoteString(
                    process,
                    imageBase + nameThunk.u1.AddressOfData
                        + offsetof(IMAGE_IMPORT_BY_NAME, Name),
                    importName,
                    sizeof(importName)))
            {
                return false;
            }
            if (_stricmp(importName, "TerminateProcess") == 0)
            {
                const uintptr_t thunk = imageBase + descriptor.FirstThunk
                    + thunkIndex * sizeof(uint32_t);
                if (thunk < imageBase
                    || thunk + sizeof(uint32_t)
                        > imageBase + nt.OptionalHeader.SizeOfImage)
                {
                    return false;
                }
                *outputThunk = thunk;
                return true;
            }
        }
    }

    return false;
}

bool VerifyRemoteParent(
    HANDLE process,
    DWORD processId,
    const FILETIME& expectedCreationTime,
    const RevivalAddressProfile& profile)
{
    if (process == nullptr || processId == 0
        || profile.launcherExeTimestamp == 0
        || profile.launcherExeSizeOfImage == 0
        || profile.launcherExeEntryPointRva == 0
        || profile.launcherCleanupTerminateCallRva == 0)
    {
        return false;
    }

    if (GetProcessId(process) != processId)
    {
        return false;
    }

    FILETIME actualCreationTime = {};
    if (!QueryCreationTime(process, &actualCreationTime)
        || !SameFileTime(actualCreationTime, expectedCreationTime))
    {
        return false;
    }

    uintptr_t imageBase = 0;
    if (!FindRemoteLauncherBase(processId, &imageBase))
    {
        return false;
    }

    IMAGE_DOS_HEADER dos = {};
    if (!ReadRemoteExact(process, imageBase, &dos, sizeof(dos))
        || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0)
    {
        return false;
    }

    IMAGE_NT_HEADERS32 nt = {};
    if (!ReadRemoteExact(
            process,
            imageBase + static_cast<uint32_t>(dos.e_lfanew),
            &nt,
            sizeof(nt))
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
        || nt.FileHeader.TimeDateStamp != profile.launcherExeTimestamp
        || nt.OptionalHeader.SizeOfImage != profile.launcherExeSizeOfImage
        || nt.OptionalHeader.AddressOfEntryPoint
            != profile.launcherExeEntryPointRva)
    {
        return false;
    }

    const uintptr_t callRva = profile.launcherCleanupTerminateCallRva;
    if (callRva + 6u > nt.OptionalHeader.SizeOfImage)
    {
        return false;
    }

    uint8_t callBytes[6] = {};
    if (!ReadRemoteExact(
            process,
            imageBase + callRva,
            callBytes,
            sizeof(callBytes))
        || callBytes[0] != 0xFFu || callBytes[1] != 0x15u)
    {
        return false;
    }

    uint32_t callThunk = 0;
    std::memcpy(&callThunk, callBytes + 2, sizeof(callThunk));
    uintptr_t expectedThunk = 0;
    return FindRemoteTerminateThunk(process, imageBase, nt, &expectedThunk)
        && static_cast<uintptr_t>(callThunk) == expectedThunk;
}

bool LocalImageRangeContains(
    uintptr_t imageBase,
    uint32_t imageSize,
    uintptr_t address,
    size_t size)
{
    return imageBase != 0 && imageSize != 0 && address >= imageBase
        && size <= imageSize
        && address - imageBase <= imageSize - size;
}

bool FindLocalTerminateThunk(
    uintptr_t imageBase,
    const IMAGE_NT_HEADERS32* nt,
    uint32_t** outputThunk)
{
    if (nt == nullptr || outputThunk == nullptr)
    {
        return false;
    }
    *outputThunk = nullptr;

    const uint32_t imageSize = nt->OptionalHeader.SizeOfImage;
    const DWORD importRva =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT]
            .VirtualAddress;
    if (importRva == 0
        || !LocalImageRangeContains(
            imageBase,
            imageSize,
            imageBase + importRva,
            sizeof(IMAGE_IMPORT_DESCRIPTOR)))
    {
        return false;
    }

    auto* descriptor = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        imageBase + importRva);
    for (DWORD descriptorIndex = 0; descriptorIndex < 256;
         ++descriptorIndex, ++descriptor)
    {
        if (!LocalImageRangeContains(
                imageBase,
                imageSize,
                reinterpret_cast<uintptr_t>(descriptor),
                sizeof(*descriptor)))
        {
            return false;
        }
        if (descriptor->Name == 0)
        {
            return false;
        }
        if (!LocalImageRangeContains(
                imageBase,
                imageSize,
                imageBase + descriptor->Name,
                1))
        {
            return false;
        }

        const char* provider = reinterpret_cast<const char*>(
            imageBase + descriptor->Name);
        if (!IsKernelImportProvider(provider))
        {
            continue;
        }

        const DWORD namesRva = descriptor->OriginalFirstThunk != 0
            ? descriptor->OriginalFirstThunk
            : descriptor->FirstThunk;
        if (namesRva == 0 || descriptor->FirstThunk == 0)
        {
            return false;
        }

        for (DWORD thunkIndex = 0; thunkIndex < 4096; ++thunkIndex)
        {
            const uintptr_t nameThunkAddress = imageBase + namesRva
                + thunkIndex * sizeof(IMAGE_THUNK_DATA32);
            const uintptr_t iatThunkAddress = imageBase
                + descriptor->FirstThunk + thunkIndex * sizeof(uint32_t);
            if (!LocalImageRangeContains(
                    imageBase,
                    imageSize,
                    nameThunkAddress,
                    sizeof(IMAGE_THUNK_DATA32))
                || !LocalImageRangeContains(
                    imageBase,
                    imageSize,
                    iatThunkAddress,
                    sizeof(uint32_t)))
            {
                return false;
            }

            const auto* nameThunk =
                reinterpret_cast<const IMAGE_THUNK_DATA32*>(nameThunkAddress);
            if (nameThunk->u1.AddressOfData == 0)
            {
                break;
            }
            if (IMAGE_SNAP_BY_ORDINAL32(nameThunk->u1.Ordinal))
            {
                continue;
            }

            const uintptr_t importNameAddress = imageBase
                + nameThunk->u1.AddressOfData
                + offsetof(IMAGE_IMPORT_BY_NAME, Name);
            if (!LocalImageRangeContains(
                    imageBase,
                    imageSize,
                    importNameAddress,
                    1))
            {
                return false;
            }
            const char* importName =
                reinterpret_cast<const char*>(importNameAddress);
            if (_stricmp(importName, "TerminateProcess") == 0)
            {
                *outputThunk = reinterpret_cast<uint32_t*>(iatThunkAddress);
                return true;
            }
        }
    }

    return false;
}

bool VerifyLocalParentAndLocateThunk(uint32_t** outputThunk)
{
    if (outputThunk == nullptr || g_guardMarker == nullptr)
    {
        return false;
    }
    *outputThunk = nullptr;

    const DWORD currentPid = GetCurrentProcessId();
    if (currentPid != g_guardMarker->parentProcessId)
    {
        return false;
    }

    FILETIME expectedCreationTime = {};
    expectedCreationTime.dwLowDateTime =
        g_guardMarker->parentCreationTimeLow;
    expectedCreationTime.dwHighDateTime =
        g_guardMarker->parentCreationTimeHigh;
    FILETIME actualCreationTime = {};
    if (!QueryCreationTime(GetCurrentProcess(), &actualCreationTime)
        || !SameFileTime(actualCreationTime, expectedCreationTime))
    {
        return false;
    }

    const uintptr_t imageBase =
        reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (imageBase == 0)
    {
        return false;
    }

    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        {
            return false;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            imageBase + static_cast<uint32_t>(dos->e_lfanew));
        if (nt->Signature != IMAGE_NT_SIGNATURE
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
            || nt->FileHeader.TimeDateStamp
                != g_guardMarker->launcherExeTimestamp
            || nt->OptionalHeader.SizeOfImage
                != g_guardMarker->launcherExeSizeOfImage
            || nt->OptionalHeader.AddressOfEntryPoint
                != g_guardMarker->launcherExeEntryPointRva
            || g_guardMarker->expectedCleanupReturnRva < 6u
            || g_guardMarker->expectedCleanupReturnRva
                > nt->OptionalHeader.SizeOfImage)
        {
            return false;
        }

        const uintptr_t callAddress = imageBase
            + g_guardMarker->expectedCleanupReturnRva - 6u;
        if (!LocalImageRangeContains(
                imageBase,
                nt->OptionalHeader.SizeOfImage,
                callAddress,
                6u))
        {
            return false;
        }
        const auto* callBytes = reinterpret_cast<const uint8_t*>(callAddress);
        if (callBytes[0] != 0xFFu || callBytes[1] != 0x15u)
        {
            return false;
        }

        uint32_t encodedThunk = 0;
        std::memcpy(&encodedThunk, callBytes + 2, sizeof(encodedThunk));
        uint32_t* terminateThunk = nullptr;
        if (!FindLocalTerminateThunk(imageBase, nt, &terminateThunk)
            || terminateThunk == nullptr
            || encodedThunk
                != static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(terminateThunk)))
        {
            return false;
        }

        const FARPROC nativeTerminate = GetProcAddress(
            GetModuleHandleA("kernel32.dll"),
            "TerminateProcess");
        if (nativeTerminate == nullptr
            || *terminateThunk
                != static_cast<uint32_t>(
                    reinterpret_cast<uintptr_t>(nativeTerminate)))
        {
            return false;
        }

        g_guardExeBase = imageBase;
        *outputThunk = terminateThunk;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool PatchLocalTerminateThunk(uint32_t* thunk)
{
    if (thunk == nullptr)
    {
        return false;
    }

    DWORD oldProtect = 0;
    if (VirtualProtect(thunk, sizeof(*thunk), PAGE_READWRITE, &oldProtect)
        == FALSE)
    {
        return false;
    }

    const uint32_t replacement = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&nb_stub_TerminateProcess));
    g_guardOriginalTerminateTarget = static_cast<uint32_t>(
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(thunk),
            static_cast<LONG>(replacement)));

    DWORD ignoredProtect = 0;
    const BOOL protectRestored = VirtualProtect(
        thunk,
        sizeof(*thunk),
        oldProtect,
        &ignoredProtect);
    if (protectRestored == FALSE)
    {
        DWORD writableProtect = 0;
        if (VirtualProtect(
                thunk,
                sizeof(*thunk),
                PAGE_READWRITE,
                &writableProtect)
            != FALSE)
        {
            InterlockedExchange(
                reinterpret_cast<volatile LONG*>(thunk),
                static_cast<LONG>(g_guardOriginalTerminateTarget));
            DWORD ignored = 0;
            VirtualProtect(
                thunk,
                sizeof(*thunk),
                writableProtect,
                &ignored);
        }
        g_guardOriginalTerminateTarget = 0;
        return false;
    }

    g_guardTerminateThunk = thunk;
    return true;
}

void RestoreLocalTerminateThunk()
{
    uint32_t* const thunk = g_guardTerminateThunk;
    const uint32_t original = g_guardOriginalTerminateTarget;
    if (thunk == nullptr || original == 0)
    {
        return;
    }

    __try
    {
        const uint32_t replacement = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(&nb_stub_TerminateProcess));
        if (*thunk == replacement)
        {
            DWORD oldProtect = 0;
            if (VirtualProtect(
                    thunk,
                    sizeof(*thunk),
                    PAGE_READWRITE,
                    &oldProtect)
                != FALSE)
            {
                InterlockedExchange(
                    reinterpret_cast<volatile LONG*>(thunk),
                    static_cast<LONG>(original));
                DWORD ignored = 0;
                VirtualProtect(
                    thunk,
                    sizeof(*thunk),
                    oldProtect,
                    &ignored);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    g_guardTerminateThunk = nullptr;
    g_guardOriginalTerminateTarget = 0;
}

DWORD WINAPI ExternalLauncherGuardThread(LPVOID)
{
    uint32_t* terminateThunk = nullptr;
    if (g_guardMarker == nullptr
        || InterlockedCompareExchange(
               &g_guardMarker->activationState,
               kGuardPending,
               kGuardPending)
            != kGuardPending
        || !VerifyLocalParentAndLocateThunk(&terminateThunk)
        || !PatchLocalTerminateThunk(terminateThunk))
    {
        if (g_guardMarker != nullptr)
        {
            InterlockedCompareExchange(
                &g_guardMarker->activationState,
                kGuardFailed,
                kGuardPending);
        }
        return 0;
    }

    // Cancellation can race with validation/patching.  Until this CAS succeeds
    // the stub treats every call as pass-through.  If the child already timed
    // out, restore the IAT immediately and leave no half-active guard behind.
    if (InterlockedCompareExchange(
            &g_guardMarker->activationState,
            kGuardActive,
            kGuardPending)
        != kGuardPending)
    {
        RestoreLocalTerminateThunk();
        return 0;
    }

    MemoryBarrier();
    if (g_guardReadyEvent != nullptr)
    {
        SetEvent(g_guardReadyEvent);
    }
    return 0;
}

void CloseGuardObjects()
{
    if (g_guardMarker != nullptr)
    {
        UnmapViewOfFile(g_guardMarker);
        g_guardMarker = nullptr;
    }
    if (g_guardReadyEvent != nullptr)
    {
        CloseHandle(g_guardReadyEvent);
        g_guardReadyEvent = nullptr;
    }
    if (g_guardMapHandle != nullptr)
    {
        CloseHandle(g_guardMapHandle);
        g_guardMapHandle = nullptr;
    }
}

void CloseChildGuardResources(ChildGuardResources* resources)
{
    if (resources == nullptr)
    {
        return;
    }
    if (resources->marker != nullptr)
    {
        UnmapViewOfFile(resources->marker);
        resources->marker = nullptr;
    }
    if (resources->readyEvent != nullptr)
    {
        CloseHandle(resources->readyEvent);
        resources->readyEvent = nullptr;
    }
    if (resources->mapHandle != nullptr)
    {
        CloseHandle(resources->mapHandle);
        resources->mapHandle = nullptr;
    }
    if (resources->exactParentProcess != nullptr)
    {
        CloseHandle(resources->exactParentProcess);
        resources->exactParentProcess = nullptr;
    }
}

bool DrainDeferredChildGuardResources()
{
    if (InterlockedCompareExchange(&g_childMarkerReaders, 0, 0) != 0)
    {
        return false;
    }

    const LONG state = InterlockedCompareExchange(
        &g_deferredChildCleanupState,
        kDeferredChildCleanupClaimed,
        kDeferredChildCleanupReady);
    if (state == kDeferredChildCleanupNone)
    {
        return true;
    }
    if (state != kDeferredChildCleanupReady)
    {
        return false;
    }

    MemoryBarrier();
    ChildGuardResources resources = g_deferredChildResources;
    g_deferredChildResources = {};
    CloseChildGuardResources(&resources);
    InterlockedExchange(
        &g_deferredChildCleanupState,
        kDeferredChildCleanupNone);
    return true;
}

void ReleaseOrDeferChildGuardResources(ChildGuardResources resources)
{
    if (resources.marker == nullptr && resources.mapHandle == nullptr
        && resources.readyEvent == nullptr
        && resources.exactParentProcess == nullptr)
    {
        return;
    }

    if (InterlockedCompareExchange(&g_childMarkerReaders, 0, 0) == 0)
    {
        CloseChildGuardResources(&resources);
        return;
    }

    // New installation is forbidden while this slot is occupied, so one
    // deferred bundle is sufficient.  Never overwrite a prior bundle: in the
    // impossible overlap case leaking is safer than unmapping a live reader.
    if (InterlockedCompareExchange(
            &g_deferredChildCleanupState,
            kDeferredChildCleanupNone,
            kDeferredChildCleanupNone)
        != kDeferredChildCleanupNone)
    {
        return;
    }

    g_deferredChildResources = resources;
    MemoryBarrier();
    InterlockedExchange(
        &g_deferredChildCleanupState,
        kDeferredChildCleanupReady);

    // The last reader may have left between the first count read and bundle
    // publication.  This control-plane cleanup can reclaim immediately in
    // that case, but it never waits for a reader (critical for process detach).
    if (InterlockedCompareExchange(&g_childMarkerReaders, 0, 0) == 0)
    {
        (void)DrainDeferredChildGuardResources();
    }
}
} // namespace

bool InstallExternalLauncherGuard(
    HANDLE parentProcess,
    DWORD parentProcessId,
    const FILETIME& parentCreationTime,
    const RevivalAddressProfile& profile,
    uintptr_t* outRemoteBase)
{
    if (outRemoteBase != nullptr)
    {
        *outRemoteBase = 0;
    }

    // A previous cleanup may have detached its marker while a wait-free
    // signal reader was still active.  Never publish a second generation over
    // that bundle; drain it now or fail closed and let a later control-plane
    // attempt retry after the reader has left.
    if (!DrainDeferredChildGuardResources())
    {
        return false;
    }

    if (InterlockedCompareExchange(
            &g_guardRole,
            kGuardRoleNone,
            kGuardRoleNone)
            != kGuardRoleNone
        || InterlockedCompareExchangePointer(
               &g_childLiveMarker,
               nullptr,
               nullptr) != nullptr
        || parentProcess == nullptr || parentProcessId == 0
        || !VerifyRemoteParent(
            parentProcess,
            parentProcessId,
            parentCreationTime,
            profile))
    {
        return false;
    }

    char mapName[96] = {};
    char eventName[96] = {};
    BuildGuardObjectName(
        mapName,
        sizeof(mapName),
        "Map",
        parentProcessId);
    BuildGuardObjectName(
        eventName,
        sizeof(eventName),
        "Ready",
        parentProcessId);

    HANDLE mapHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(ExternalLauncherGuardMarker),
        mapName);
    if (mapHandle == nullptr)
    {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mapHandle);
        SetLastError(ERROR_ALREADY_EXISTS);
        return false;
    }

    HANDLE readyEvent = CreateEventA(
        nullptr,
        TRUE,
        FALSE,
        eventName);
    if (readyEvent == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        const DWORD error = readyEvent == nullptr
            ? GetLastError()
            : ERROR_ALREADY_EXISTS;
        if (readyEvent != nullptr)
        {
            CloseHandle(readyEvent);
        }
        CloseHandle(mapHandle);
        SetLastError(error);
        return false;
    }

    auto* marker = static_cast<ExternalLauncherGuardMarker*>(
        MapViewOfFile(
            mapHandle,
            FILE_MAP_READ | FILE_MAP_WRITE,
            0,
            0,
            sizeof(ExternalLauncherGuardMarker)));
    if (marker == nullptr)
    {
        const DWORD error = GetLastError();
        CloseHandle(readyEvent);
        CloseHandle(mapHandle);
        SetLastError(error);
        return false;
    }

    HANDLE exactParentProcess = nullptr;
    if (DuplicateHandle(
            GetCurrentProcess(),
            parentProcess,
            GetCurrentProcess(),
            &exactParentProcess,
            PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
            FALSE,
            0)
        == FALSE)
    {
        const DWORD error = GetLastError();
        UnmapViewOfFile(marker);
        CloseHandle(readyEvent);
        CloseHandle(mapHandle);
        SetLastError(error);
        return false;
    }

    std::memset(marker, 0, sizeof(*marker));
    marker->magic = kGuardMagic;
    marker->version = kGuardVersion;
    marker->structureSize = sizeof(*marker);
    marker->parentProcessId = parentProcessId;
    marker->parentCreationTimeLow = parentCreationTime.dwLowDateTime;
    marker->parentCreationTimeHigh = parentCreationTime.dwHighDateTime;
    marker->childProcessId = GetCurrentProcessId();
    marker->launcherExeTimestamp = profile.launcherExeTimestamp;
    marker->launcherExeSizeOfImage = profile.launcherExeSizeOfImage;
    marker->launcherExeEntryPointRva = profile.launcherExeEntryPointRva;
    marker->revivalDllTimestamp = profile.peTimestamp;
    marker->expectedCleanupReturnRva = static_cast<uint32_t>(
        profile.launcherCleanupTerminateCallRva + 6u);
    marker->activationState = kGuardPending;
    marker->terminateBlockedSerial = 0;
    MemoryBarrier();

    g_guardMapHandle = mapHandle;
    g_guardReadyEvent = readyEvent;
    g_guardMarker = marker;
    InterlockedExchangePointer(
        &g_childExactParentProcess,
        exactParentProcess);
    InterlockedExchange(&g_childConsumedTerminateSerial, 0);
    InterlockedExchange(&g_childSignalDeliveryEnabled, 1);
    InterlockedExchangePointer(&g_childLiveMarker, marker);
    InterlockedExchange(&g_guardRole, kGuardRoleChild);

    uintptr_t remoteBase = 0;
    if (!InjectSelf(parentProcess, &remoteBase) || remoteBase == 0)
    {
        InterlockedCompareExchange(
            &marker->activationState,
            kGuardCancelled,
            kGuardPending);
        CleanupExternalLauncherGuard();
        return false;
    }

    // LoadLibrary does not rerun DllMain when Wine or another loader already
    // preloaded this exact DLL into the launcher. Invoke one narrow,
    // idempotent export explicitly so that both newly loaded and preloaded
    // parents consume the marker. This export starts only the guard path; it
    // never enters generic injected-helper bootstrap.
    const uintptr_t localBase = reinterpret_cast<uintptr_t>(SelfModule());
    const uintptr_t localActivate = reinterpret_cast<uintptr_t>(
        &nb_activate_external_launcher_guard);
    if (localBase == 0 || localActivate < localBase)
    {
        CleanupExternalLauncherGuard();
        return false;
    }
    const uintptr_t remoteActivate =
        remoteBase + (localActivate - localBase);
    HANDLE activationThread = CreateRemoteThread(
        parentProcess,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteActivate),
        nullptr,
        0,
        nullptr);
    DWORD activationResult = 0;
    const bool activationOk = activationThread != nullptr
        && WaitForSingleObject(activationThread, kGuardActivationTimeoutMs)
            == WAIT_OBJECT_0
        && GetExitCodeThread(activationThread, &activationResult) != FALSE
        && activationResult != 0;
    if (activationThread != nullptr)
    {
        CloseHandle(activationThread);
    }
    if (!activationOk)
    {
        InterlockedCompareExchange(
            &marker->activationState,
            kGuardCancelled,
            kGuardPending);
        CleanupExternalLauncherGuard();
        return false;
    }

    const DWORD waitStart = GetTickCount();
    for (;;)
    {
        const LONG state = InterlockedCompareExchange(
            &marker->activationState,
            kGuardPending,
            kGuardPending);
        if (state == kGuardActive)
        {
            // ACTIVE authorizes the stub; the event completes the handshake.
            // Do not expose a half-ready installation during the few
            // instructions between the parent's CAS and SetEvent.
            if (WaitForSingleObject(readyEvent, 0) == WAIT_OBJECT_0)
            {
                if (outRemoteBase != nullptr)
                {
                    *outRemoteBase = remoteBase;
                }
                return true;
            }
        }
        if (state == kGuardFailed || state == kGuardCancelled)
        {
            CleanupExternalLauncherGuard();
            return false;
        }

        const DWORD elapsed = GetTickCount() - waitStart;
        if (elapsed >= kGuardActivationTimeoutMs)
        {
            LONG observed = InterlockedCompareExchange(
                &marker->activationState,
                kGuardCancelled,
                kGuardPending);
            if (observed == kGuardActive)
            {
                (void)InterlockedCompareExchange(
                    &marker->activationState,
                    kGuardCancelled,
                    kGuardActive);
            }
            CleanupExternalLauncherGuard();
            return false;
        }

        const DWORD remaining = kGuardActivationTimeoutMs - elapsed;
        const DWORD wait = WaitForSingleObject(
            readyEvent,
            remaining < 100u ? remaining : 100u);
        if (wait == WAIT_FAILED)
        {
            InterlockedCompareExchange(
                &marker->activationState,
                kGuardCancelled,
                kGuardPending);
            CleanupExternalLauncherGuard();
            return false;
        }
    }
}

bool HasActiveExactExternalLauncherParent()
{
    ExternalLauncherGuardMarker* marker = AcquireChildMarker(false);
    if (marker == nullptr)
    {
        return false;
    }

    const HANDLE exactParentProcess = static_cast<HANDLE>(
        InterlockedCompareExchangePointer(
            &g_childExactParentProcess,
            nullptr,
            nullptr));
    const bool exactAndAlive = IsExactParentAlive(
        marker,
        exactParentProcess,
        true);
    ReleaseChildMarker();
    return exactAndAlive;
}

void RetireExternalLauncherGuardSignalDelivery()
{
    // Disable first.  Has/Consume both recheck this gate, so serials published
    // concurrently with or after retirement cannot leak into the next session.
    InterlockedExchange(&g_childSignalDeliveryEnabled, 0);
    ExternalLauncherGuardMarker* marker = AcquireChildMarker(false);
    if (marker == nullptr)
    {
        return;
    }
    const LONG published = InterlockedCompareExchange(
        &marker->terminateBlockedSerial,
        0,
        0);
    InterlockedExchange(&g_childConsumedTerminateSerial, published);
    ReleaseChildMarker();
}

bool ReapExternalLauncherGuardIfParentExited()
{
    // Title/control-plane only: this may query a process handle and release a
    // deferred mapping.  Never call it from the rollback/per-frame fast path.
    const LONG deferredBefore = InterlockedCompareExchange(
        &g_deferredChildCleanupState,
        kDeferredChildCleanupNone,
        kDeferredChildCleanupNone);
    const bool drainedDeferred = DrainDeferredChildGuardResources();
    const bool reapedDeferred = drainedDeferred
        && deferredBefore == kDeferredChildCleanupReady;

    ExternalLauncherGuardMarker* marker = AcquireChildMarker(false);
    if (marker == nullptr)
    {
        return reapedDeferred;
    }

    const HANDLE exactParentProcess = static_cast<HANDLE>(
        InterlockedCompareExchangePointer(
            &g_childExactParentProcess,
            nullptr,
            nullptr));
    const bool exactParentAlive = IsExactParentAlive(
        marker,
        exactParentProcess,
        false);
    ReleaseChildMarker();

    if (exactParentAlive)
    {
        return reapedDeferred;
    }
    CleanupExternalLauncherGuard();
    return true;
}

bool HasExternalLauncherTerminateBlockedSignal()
{
    ExternalLauncherGuardMarker* marker = AcquireChildMarker(true);
    if (marker == nullptr)
    {
        return false;
    }

    const LONG published = InterlockedCompareExchange(
        &marker->terminateBlockedSerial,
        0,
        0);
    const LONG consumed = InterlockedCompareExchange(
        &g_childConsumedTerminateSerial,
        0,
        0);
    const bool available = InterlockedCompareExchange(
               &g_childSignalDeliveryEnabled,
               0,
               0) != 0
        && published != consumed;
    ReleaseChildMarker();
    return available;
}

bool ConsumeExternalLauncherTerminateBlockedSignal()
{
    ExternalLauncherGuardMarker* marker = AcquireChildMarker(true);
    if (marker == nullptr)
    {
        return false;
    }

    bool consumedSignal = false;
    for (;;)
    {
        if (InterlockedCompareExchange(
                &g_childSignalDeliveryEnabled,
                0,
                0) == 0)
        {
            break;
        }

        const LONG published = InterlockedCompareExchange(
            &marker->terminateBlockedSerial,
            0,
            0);
        const LONG consumed = InterlockedCompareExchange(
            &g_childConsumedTerminateSerial,
            0,
            0);
        if (published == consumed)
        {
            break;
        }
        if (InterlockedCompareExchange(
                &g_childSignalDeliveryEnabled,
                0,
                0) == 0)
        {
            break;
        }
        if (InterlockedCompareExchange(
                &g_childConsumedTerminateSerial,
                published,
                consumed)
            == consumed)
        {
            if (InterlockedCompareExchange(
                    &g_childSignalDeliveryEnabled,
                    0,
                    0) != 0)
            {
                consumedSignal = true;
                break;
            }

            // Retirement won the race after our consume CAS.  Acknowledge any
            // serial that arrived in that window and keep delivery retired.
            const LONG latest = InterlockedCompareExchange(
                &marker->terminateBlockedSerial,
                0,
                0);
            InterlockedExchange(
                &g_childConsumedTerminateSerial,
                latest);
            break;
        }
    }

    ReleaseChildMarker();
    return consumedSignal;
}

void CleanupExternalLauncherGuard()
{
    // Claim this generation before touching any owning globals.  A detach and
    // a title/control-plane reap can arrive together; exactly one may detach
    // the mapping and handles, while the other only attempts deferred drain.
    if (InterlockedCompareExchange(
            &g_guardRole,
            kGuardRoleChildClosing,
            kGuardRoleChild)
        != kGuardRoleChild)
    {
        (void)DrainDeferredChildGuardResources();
        return;
    }

    // Readers increment before loading this pointer.  Closing admission first
    // and then atomically detaching it guarantees every reader that obtained
    // the old marker is represented in g_childMarkerReaders.  We never wait
    // here: process-termination detach may have already killed such a reader.
    InterlockedExchange(&g_childSignalDeliveryEnabled, 0);
    auto* marker = static_cast<ExternalLauncherGuardMarker*>(
        InterlockedExchangePointer(&g_childLiveMarker, nullptr));

    if (marker != nullptr)
    {
        const LONG published = InterlockedCompareExchange(
            &marker->terminateBlockedSerial,
            0,
            0);
        InterlockedExchange(&g_childConsumedTerminateSerial, published);

        LONG state = InterlockedCompareExchange(
            &marker->activationState,
            kGuardPending,
            kGuardPending);
        while (state == kGuardPending || state == kGuardActive)
        {
            const LONG observed = InterlockedCompareExchange(
                &marker->activationState,
                kGuardCancelled,
                state);
            if (observed == state)
            {
                break;
            }
            state = observed;
        }
    }

    ChildGuardResources resources = {};
    resources.marker = marker;
    resources.mapHandle = g_guardMapHandle;
    resources.readyEvent = g_guardReadyEvent;
    resources.exactParentProcess = static_cast<HANDLE>(
        InterlockedExchangePointer(
            &g_childExactParentProcess,
            nullptr));
    g_guardMarker = nullptr;
    g_guardMapHandle = nullptr;
    g_guardReadyEvent = nullptr;

    InterlockedExchange(&g_childConsumedTerminateSerial, 0);
    ReleaseOrDeferChildGuardResources(resources);
    InterlockedExchange(&g_guardRole, kGuardRoleNone);
}

bool TryStartExternalLauncherGuardProcess()
{
    if (InterlockedCompareExchange(
            &g_guardRole,
            kGuardRoleNone,
            kGuardRoleNone)
        != kGuardRoleNone)
    {
        return IsExternalLauncherGuardProcess();
    }

    const DWORD currentPid = GetCurrentProcessId();
    char mapName[96] = {};
    char eventName[96] = {};
    BuildGuardObjectName(mapName, sizeof(mapName), "Map", currentPid);
    BuildGuardObjectName(eventName, sizeof(eventName), "Ready", currentPid);

    HANDLE mapHandle = OpenFileMappingA(
        FILE_MAP_READ | FILE_MAP_WRITE,
        FALSE,
        mapName);
    if (mapHandle == nullptr)
    {
        return false;
    }

    auto* marker = static_cast<ExternalLauncherGuardMarker*>(
        MapViewOfFile(
            mapHandle,
            FILE_MAP_READ | FILE_MAP_WRITE,
            0,
            0,
            sizeof(ExternalLauncherGuardMarker)));
    if (marker == nullptr)
    {
        CloseHandle(mapHandle);
        return false;
    }

    HANDLE readyEvent = OpenEventA(
        EVENT_MODIFY_STATE | SYNCHRONIZE,
        FALSE,
        eventName);
    const bool markerValid = readyEvent != nullptr
        && marker->magic == kGuardMagic
        && marker->version == kGuardVersion
        && marker->structureSize == sizeof(*marker)
        && marker->parentProcessId == currentPid
        && marker->childProcessId != 0
        && marker->launcherExeTimestamp != 0
        && marker->launcherExeSizeOfImage != 0
        && marker->launcherExeEntryPointRva != 0
        && marker->revivalDllTimestamp != 0
        && marker->expectedCleanupReturnRva >= 6u
        && InterlockedCompareExchange(
               &marker->activationState,
               kGuardPending,
               kGuardPending)
            == kGuardPending;
    if (!markerValid)
    {
        if (readyEvent != nullptr)
        {
            CloseHandle(readyEvent);
        }
        UnmapViewOfFile(marker);
        CloseHandle(mapHandle);
        return false;
    }

    g_guardMapHandle = mapHandle;
    g_guardReadyEvent = readyEvent;
    g_guardMarker = marker;
    InterlockedExchange(&g_guardRole, kGuardRoleParent);

    HANDLE thread = CreateThread(
        nullptr,
        0,
        ExternalLauncherGuardThread,
        nullptr,
        0,
        nullptr);
    if (thread == nullptr)
    {
        InterlockedCompareExchange(
            &marker->activationState,
            kGuardFailed,
            kGuardPending);
    }
    else
    {
        CloseHandle(thread);
    }

    // Returning true suppresses the ordinary injected-helper path.  Even a
    // guard-thread failure must remain narrow and inert in this process.
    return true;
}

bool IsExternalLauncherGuardProcess()
{
    return InterlockedCompareExchange(
               &g_guardRole,
               kGuardRoleNone,
               kGuardRoleNone)
        == kGuardRoleParent;
}

bool GetExternalLauncherGuardChildProcessId(
    DWORD* outChildProcessId,
    uint32_t* outRevivalDllTimestamp)
{
    if (outChildProcessId == nullptr)
    {
        return false;
    }
    *outChildProcessId = 0;
    if (outRevivalDllTimestamp != nullptr)
    {
        *outRevivalDllTimestamp = 0;
    }
    if (!IsExternalLauncherGuardProcess()
        || !ValidateMarkerHeader(g_guardMarker))
    {
        return false;
    }
    const LONG state = InterlockedCompareExchange(
        &g_guardMarker->activationState,
        kGuardPending,
        kGuardPending);
    if (state != kGuardActive || g_guardMarker->childProcessId == 0)
    {
        return false;
    }
    *outChildProcessId = g_guardMarker->childProcessId;
    if (outRevivalDllTimestamp != nullptr)
    {
        *outRevivalDllTimestamp = g_guardMarker->revivalDllTimestamp;
    }
    return true;
}

void ShutdownExternalLauncherGuardProcess()
{
    if (!IsExternalLauncherGuardProcess())
    {
        return;
    }

    if (g_guardMarker != nullptr)
    {
        InterlockedCompareExchange(
            &g_guardMarker->activationState,
            kGuardCancelled,
            kGuardPending);
    }
    RestoreLocalTerminateThunk();
    CloseGuardObjects();
    g_guardExeBase = 0;
    InterlockedExchange(&g_guardRole, kGuardRoleNone);
}

ExternalLauncherTerminateDecision EvaluateExternalLauncherTerminate(
    HANDLE process,
    const void* callerReturnAddress)
{
    if (!IsExternalLauncherGuardProcess())
    {
        return ExternalLauncherTerminateDecision::NotGuardProcess;
    }
    if (g_guardMarker == nullptr
        || InterlockedCompareExchange(
               &g_guardMarker->activationState,
               kGuardPending,
               kGuardPending)
            != kGuardActive)
    {
        // A failed or explicitly cancelled narrow guard must remain a direct
        // passthrough.  Falling into the ordinary helper stub here would try
        // to lazily bind IPC/logger state that this parent intentionally never
        // initialized.
        return ExternalLauncherTerminateDecision::PassThrough;
    }

    const DWORD targetPid = process != nullptr ? GetProcessId(process) : 0;
    if (targetPid == 0 || targetPid != g_guardMarker->childProcessId
        || callerReturnAddress == nullptr || g_guardExeBase == 0)
    {
        return ExternalLauncherTerminateDecision::PassThrough;
    }

    const uintptr_t caller = reinterpret_cast<uintptr_t>(callerReturnAddress);
    if (caller < g_guardExeBase
        || caller - g_guardExeBase
            != g_guardMarker->expectedCleanupReturnRva)
    {
        return ExternalLauncherTerminateDecision::PassThrough;
    }

    InterlockedIncrement(&g_guardMarker->terminateBlockedSerial);
    return ExternalLauncherTerminateDecision::Block;
}
} // namespace netplay::bridge::takeover

extern "C" __declspec(dllexport) DWORD WINAPI
nb_activate_external_launcher_guard(LPVOID)
{
    return netplay::bridge::takeover::TryStartExternalLauncherGuardProcess()
        ? 1u
        : 0u;
}
