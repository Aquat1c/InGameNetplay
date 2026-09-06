#include "netplay/bridge/revival_launcher_probe.h"

#include "logger.h"
#include "netplay/bridge/revival_addresses.h"
#include "netplay/bridge/takeover_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include <tlhelp32.h>
#if defined(EFZ_EMBEDDED_TLS)
#include <mbedtls/sha256.h>
#else
#include <wincrypt.h>

// The XP-targeting Windows 7.1A SDK omits this identifier even though the
// Enhanced RSA/AES provider exposes SHA-256 on the supported XP baseline.
#ifndef CALG_SHA_256
#define CALG_SHA_256 static_cast<ALG_ID>(0x0000800cu)
#endif
#endif

namespace netplay::bridge::takeover
{
namespace
{
struct ParentCandidate
{
    DWORD pid = 0;
    HANDLE process = nullptr;
    FILETIME creationTime = {};
    uintptr_t imageBase = 0;
    std::string imagePath;
    std::string imageBaseNameLower;
    std::string sha256;
    uint32_t timestamp = 0;
    uint32_t sizeOfImage = 0;
    uint32_t entryPointRva = 0;
    uint32_t fileSize = 0;
    std::vector<const RevivalAddressProfile*> matchingProfiles;
};

struct LauncherSessionSnapshot
{
    revival_launch::LaunchDisposition disposition =
        revival_launch::LaunchDisposition::PassiveFailClosed;
    uintptr_t dllBase = 0;
    uintptr_t sessionPtr = 0;
    uintptr_t vtable = 0;
    uintptr_t helperHandleValue = 0;
    int helperPid = -1;
    uint32_t readinessValue = 0;
    bool readinessSatisfied = true;
    int role = -1;
    int activePlayer = -1;
};

bool SameFileTime(const FILETIME& lhs, const FILETIME& rhs)
{
    return lhs.dwLowDateTime == rhs.dwLowDateTime
        && lhs.dwHighDateTime == rhs.dwHighDateTime;
}

std::string LowerBaseName(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    std::string result = slash == std::string::npos
        ? path
        : path.substr(slash + 1);
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

bool GetCurrentParent(
    DWORD* outParentPid,
    std::string* outParentBaseNameLower)
{
    if (outParentPid == nullptr || outParentBaseNameLower == nullptr)
    {
        return false;
    }
    *outParentPid = 0;
    outParentBaseNameLower->clear();

    const DWORD selfPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    PROCESSENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32First(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == selfPid)
            {
                *outParentPid = entry.th32ParentProcessID;
                const DWORD parentPid = *outParentPid;
                PROCESSENTRY32 parentEntry = {};
                parentEntry.dwSize = sizeof(parentEntry);
                if (parentPid != 0
                    && Process32First(snapshot, &parentEntry))
                {
                    do
                    {
                        if (parentEntry.th32ProcessID == parentPid)
                        {
                            *outParentBaseNameLower =
                                LowerBaseName(parentEntry.szExeFile);
                            break;
                        }
                    }
                    while (Process32Next(snapshot, &parentEntry));
                }
                found = *outParentPid != 0;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool GetProcessMainModule(
    DWORD pid,
    uintptr_t* outImageBase,
    std::string* outImagePath)
{
    if (outImageBase == nullptr || outImagePath == nullptr)
    {
        return false;
    }
    *outImageBase = 0;
    outImagePath->clear();

    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        snapshot = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            pid);
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

    MODULEENTRY32 module = {};
    module.dwSize = sizeof(module);
    const bool ok = Module32First(snapshot, &module) != FALSE;
    if (ok)
    {
        *outImageBase = reinterpret_cast<uintptr_t>(module.modBaseAddr);
        *outImagePath = module.szExePath;
    }
    CloseHandle(snapshot);
    return ok && *outImageBase != 0 && !outImagePath->empty();
}

bool ReadRemotePeIdentity(
    HANDLE process,
    uintptr_t imageBase,
    uint32_t* outTimestamp,
    uint32_t* outSizeOfImage,
    uint32_t* outEntryPointRva)
{
    if (process == nullptr || imageBase == 0
        || outTimestamp == nullptr || outSizeOfImage == nullptr
        || outEntryPointRva == nullptr)
    {
        return false;
    }

    IMAGE_DOS_HEADER dos = {};
    SIZE_T read = 0;
    if (!ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(imageBase),
            &dos,
            sizeof(dos),
            &read)
        || read != sizeof(dos)
        || dos.e_magic != IMAGE_DOS_SIGNATURE
        || dos.e_lfanew <= 0
        || dos.e_lfanew > 0x1000)
    {
        return false;
    }

    IMAGE_NT_HEADERS32 nt = {};
    if (!ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(
                imageBase + static_cast<uintptr_t>(dos.e_lfanew)),
            &nt,
            sizeof(nt),
            &read)
        || read != sizeof(nt)
        || nt.Signature != IMAGE_NT_SIGNATURE
        || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
        return false;
    }

    *outTimestamp = nt.FileHeader.TimeDateStamp;
    *outSizeOfImage = nt.OptionalHeader.SizeOfImage;
    *outEntryPointRva = nt.OptionalHeader.AddressOfEntryPoint;
    return true;
}

bool ReadFileSha256(
    const std::string& path,
    uint32_t* outFileSize,
    std::string* outHex)
{
    if (outFileSize == nullptr || outHex == nullptr)
    {
        return false;
    }
    *outFileSize = 0;
    outHex->clear();

    HANDLE file = CreateFileA(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size)
        || size.QuadPart < 0
        || size.QuadPart > 0xFFFFFFFFLL)
    {
        CloseHandle(file);
        return false;
    }

    std::array<BYTE, 64 * 1024> buffer = {};
#if defined(EFZ_EMBEDDED_TLS)
    mbedtls_sha256_context hash = {};
    mbedtls_sha256_init(&hash);
    bool ok = mbedtls_sha256_starts_ret(&hash, 0) == 0;
#else
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextA(
        &provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE;
    if (!ok)
    {
        ok = CryptAcquireContextA(
            &provider,
            nullptr,
            MS_ENH_RSA_AES_PROV_A,
            PROV_RSA_AES,
            CRYPT_VERIFYCONTEXT) != FALSE;
    }
    if (ok)
    {
        ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE;
    }
#endif
    while (ok)
    {
        DWORD bytesRead = 0;
        if (!ReadFile(
                file,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytesRead,
                nullptr))
        {
            ok = false;
            break;
        }
        if (bytesRead == 0)
        {
            break;
        }
#if defined(EFZ_EMBEDDED_TLS)
        if (mbedtls_sha256_update_ret(
                &hash, buffer.data(), bytesRead) != 0)
#else
        if (!CryptHashData(hash, buffer.data(), bytesRead, 0))
#endif
        {
            ok = false;
            break;
        }
    }

    BYTE digest[32] = {};
#if defined(EFZ_EMBEDDED_TLS)
    if (ok)
    {
        ok = mbedtls_sha256_finish_ret(&hash, digest) == 0;
    }
    mbedtls_sha256_free(&hash);
#else
    DWORD digestSize = sizeof(digest);
    if (ok)
    {
        ok = CryptGetHashParam(
            hash, HP_HASHVAL, digest, &digestSize, 0) != FALSE
            && digestSize == sizeof(digest);
    }

    if (hash != 0)
    {
        CryptDestroyHash(hash);
    }
    if (provider != 0)
    {
        CryptReleaseContext(provider, 0);
    }
#endif
    CloseHandle(file);

    if (!ok)
    {
        return false;
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    outHex->resize(sizeof(digest) * 2);
    for (size_t i = 0; i < sizeof(digest); ++i)
    {
        (*outHex)[i * 2] = kHex[digest[i] >> 4];
        (*outHex)[i * 2 + 1] = kHex[digest[i] & 0x0F];
    }
    *outFileSize = static_cast<uint32_t>(size.QuadPart);
    return true;
}

bool ProfileMatchesLauncherPe(
    const RevivalAddressProfile& profile,
    uint32_t timestamp,
    uint32_t sizeOfImage,
    uint32_t entryPointRva)
{
    return profile.launcherExeTimestamp == timestamp
        && profile.launcherExeSizeOfImage == sizeOfImage
        && profile.launcherExeEntryPointRva == entryPointRva;
}

bool ProfileMatchesLauncherFile(
    const RevivalAddressProfile& profile,
    uint32_t fileSize,
    const std::string& sha256)
{
    return profile.launcherExeFileSize == fileSize
        && profile.launcherExeSha256 != nullptr
        && _stricmp(profile.launcherExeSha256, sha256.c_str()) == 0;
}

bool ProfileMatchesDllFile(
    const RevivalAddressProfile& profile,
    HMODULE module)
{
    char path[MAX_PATH] = {};
    const DWORD pathLength = module != nullptr
        ? GetModuleFileNameA(module, path, MAX_PATH)
        : 0;
    // XP reports truncation as exactly nSize and does not guarantee a NUL.
    if (pathLength == 0 || pathLength >= MAX_PATH)
    {
        return false;
    }

    uint32_t fileSize = 0;
    std::string sha256;
    const bool revivalMatches = ReadFileSha256(path, &fileSize, &sha256)
        && fileSize == profile.revivalDllFileSize
        && profile.revivalDllSha256 != nullptr
        && _stricmp(profile.revivalDllSha256, sha256.c_str()) == 0;
    if (!revivalMatches)
    {
        return false;
    }

    if (profile.companionDdrawFileSize == 0
        || profile.companionDdrawSha256 == nullptr)
    {
        return true;
    }

    // The renderer-split 1.02j DLL and its bundled Ddraw.dll expose mutually
    // incompatible import/export ABIs compared with the earlier build.  Bind
    // launcher-first adoption to the exact already-loaded companion instead
    // of accepting a mixed package that merely shares the Revival DLL name.
    HMODULE ddraw = GetModuleHandleA("Ddraw.dll");
    char ddrawPath[MAX_PATH] = {};
    const DWORD ddrawPathLength = ddraw != nullptr
        ? GetModuleFileNameA(ddraw, ddrawPath, MAX_PATH)
        : 0;
    if (ddrawPathLength == 0 || ddrawPathLength >= MAX_PATH)
    {
        return false;
    }

    uint32_t ddrawFileSize = 0;
    std::string ddrawSha256;
    return ReadFileSha256(ddrawPath, &ddrawFileSize, &ddrawSha256)
        && ddrawFileSize == profile.companionDdrawFileSize
        && _stricmp(
            profile.companionDdrawSha256,
            ddrawSha256.c_str()) == 0;
}

bool CaptureExactParent(ParentCandidate* outParent, bool* outRevivalNamed)
{
    if (outParent == nullptr || outRevivalNamed == nullptr)
    {
        return false;
    }
    *outParent = {};
    *outRevivalNamed = false;

    DWORD parentPid = 0;
    std::string snapshotBaseName;
    if (!GetCurrentParent(&parentPid, &snapshotBaseName))
    {
        return false;
    }
    *outRevivalNamed = snapshotBaseName == "efzrevival.exe";

    constexpr DWORD kAccess = PROCESS_QUERY_INFORMATION
        | PROCESS_TERMINATE
        | PROCESS_VM_READ
        | PROCESS_VM_WRITE
        | PROCESS_VM_OPERATION
        | PROCESS_CREATE_THREAD
        | SYNCHRONIZE;
    HANDLE process = OpenProcess(kAccess, FALSE, parentPid);
    if (process == nullptr)
    {
        return false;
    }

    FILETIME creation = {};
    FILETIME exit = {};
    FILETIME kernel = {};
    FILETIME user = {};
    uintptr_t imageBase = 0;
    std::string imagePath;
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)
        || !GetProcessMainModule(parentPid, &imageBase, &imagePath))
    {
        CloseHandle(process);
        return false;
    }

    const std::string baseName = LowerBaseName(imagePath);
    *outRevivalNamed = *outRevivalNamed
        || baseName == "efzrevival.exe";

    uint32_t timestamp = 0;
    uint32_t sizeOfImage = 0;
    uint32_t entryPointRva = 0;
    if (!ReadRemotePeIdentity(
            process,
            imageBase,
            &timestamp,
            &sizeOfImage,
            &entryPointRva))
    {
        CloseHandle(process);
        return false;
    }

    std::vector<const RevivalAddressProfile*> peMatches;
    for (size_t i = 0; i < kRevivalProfileCount; ++i)
    {
        const RevivalAddressProfile* profile = kAllRevivalProfiles[i];
        if (profile != nullptr
            && ProfileMatchesLauncherPe(
                *profile, timestamp, sizeOfImage, entryPointRva))
        {
            peMatches.push_back(profile);
        }
    }

    // A renamed exact launcher is supported, but arbitrary parents are not
    // hashed unless their cheap in-memory PE identity first matches Revival.
    if (peMatches.empty())
    {
        CloseHandle(process);
        return false;
    }

    uint32_t fileSize = 0;
    std::string sha256;
    if (!ReadFileSha256(imagePath, &fileSize, &sha256))
    {
        CloseHandle(process);
        return false;
    }

    std::vector<const RevivalAddressProfile*> exactMatches;
    for (const RevivalAddressProfile* profile : peMatches)
    {
        if (ProfileMatchesLauncherFile(*profile, fileSize, sha256))
        {
            exactMatches.push_back(profile);
        }
    }
    if (exactMatches.empty())
    {
        CloseHandle(process);
        return false;
    }

    outParent->pid = parentPid;
    outParent->process = process;
    outParent->creationTime = creation;
    outParent->imageBase = imageBase;
    outParent->imagePath = imagePath;
    outParent->imageBaseNameLower = baseName;
    outParent->sha256 = sha256;
    outParent->timestamp = timestamp;
    outParent->sizeOfImage = sizeOfImage;
    outParent->entryPointRva = entryPointRva;
    outParent->fileSize = fileSize;
    outParent->matchingProfiles = std::move(exactMatches);
    return true;
}

bool ParentStillHasSameIdentity(const ParentCandidate& parent)
{
    FILETIME creation = {};
    FILETIME exit = {};
    FILETIME kernel = {};
    FILETIME user = {};
    DWORD exitCode = 0;
    return parent.process != nullptr
        && GetProcessTimes(
            parent.process, &creation, &exit, &kernel, &user) != FALSE
        && SameFileTime(creation, parent.creationTime)
        && GetExitCodeProcess(parent.process, &exitCode) != FALSE
        && exitCode == STILL_ACTIVE;
}

bool ParentAcceptsProfile(
    const ParentCandidate& parent,
    const RevivalAddressProfile* profile)
{
    if (profile == nullptr)
    {
        return false;
    }
    return std::any_of(
        parent.matchingProfiles.begin(),
        parent.matchingProfiles.end(),
        [profile](const RevivalAddressProfile* candidate) {
            // Profiles are constexpr header objects and therefore have one
            // address per translation unit. Never use pointer identity across
            // the launcher probe and the runtime profile selector.
            return candidate != nullptr
                && candidate->peTimestamp == profile->peTimestamp
                && candidate->launcherExeTimestamp
                    == profile->launcherExeTimestamp
                && candidate->versionTag != nullptr
                && profile->versionTag != nullptr
                && std::strcmp(
                       candidate->versionTag,
                       profile->versionTag) == 0;
        });
}

bool SameProfileIdentity(
    const RevivalAddressProfile* lhs,
    const RevivalAddressProfile* rhs)
{
    return lhs != nullptr && rhs != nullptr
        && lhs->peTimestamp == rhs->peTimestamp
        && lhs->launcherExeTimestamp == rhs->launcherExeTimestamp
        && lhs->versionTag != nullptr && rhs->versionTag != nullptr
        && std::strcmp(lhs->versionTag, rhs->versionTag) == 0;
}

bool ParentMatchesProfileNow(
    const RevivalLauncherProbe& probe,
    const RevivalAddressProfile& profile)
{
    if (!probe.exactExternalParent
        || probe.parentProcess == nullptr
        || probe.parentPid == 0
        || GetProcessId(probe.parentProcess) != probe.parentPid)
    {
        return false;
    }

    FILETIME creation = {};
    FILETIME exit = {};
    FILETIME kernel = {};
    FILETIME user = {};
    DWORD exitCode = 0;
    if (!GetProcessTimes(
            probe.parentProcess, &creation, &exit, &kernel, &user)
        || !SameFileTime(creation, probe.parentCreationTime)
        || !GetExitCodeProcess(probe.parentProcess, &exitCode)
        || exitCode != STILL_ACTIVE)
    {
        return false;
    }

    uintptr_t imageBase = 0;
    std::string imagePath;
    uint32_t timestamp = 0;
    uint32_t sizeOfImage = 0;
    uint32_t entryPointRva = 0;
    uint32_t fileSize = 0;
    std::string sha256;
    return GetProcessMainModule(probe.parentPid, &imageBase, &imagePath)
        && ReadRemotePeIdentity(
            probe.parentProcess,
            imageBase,
            &timestamp,
            &sizeOfImage,
            &entryPointRva)
        && ProfileMatchesLauncherPe(
            profile, timestamp, sizeOfImage, entryPointRva)
        && ReadFileSha256(imagePath, &fileSize, &sha256)
        && ProfileMatchesLauncherFile(profile, fileSize, sha256);
}

bool ParentIdentityStillLive(const RevivalLauncherProbe& probe)
{
    if (!probe.exactExternalParent
        || probe.parentProcess == nullptr
        || probe.parentPid == 0
        || GetProcessId(probe.parentProcess) != probe.parentPid)
    {
        return false;
    }

    FILETIME creation = {};
    FILETIME exit = {};
    FILETIME kernel = {};
    FILETIME user = {};
    DWORD exitCode = 0;
    return GetProcessTimes(
               probe.parentProcess, &creation, &exit, &kernel, &user)
            != FALSE
        && SameFileTime(creation, probe.parentCreationTime)
        && GetExitCodeProcess(probe.parentProcess, &exitCode) != FALSE
        && exitCode == STILL_ACTIVE;
}

template <typename T>
bool GuardedRead(uintptr_t address, T* out)
{
    if (address == 0 || out == nullptr)
    {
        return false;
    }
    __try
    {
        *out = *reinterpret_cast<const T*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool GuardedReadBytes(uintptr_t address, void* out, size_t size)
{
    if (address == 0 || out == nullptr || size == 0)
    {
        return false;
    }
    __try
    {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CheckedAddressAdd(
    uintptr_t address,
    uintptr_t byteOffset,
    uintptr_t* out)
{
    if (out == nullptr
        || address > static_cast<uintptr_t>(-1) - byteOffset)
    {
        return false;
    }
    *out = address + byteOffset;
    return true;
}

struct MsvcTournamentDequeState
{
    uintptr_t map = 0;
    uint32_t mapSlots = 0;
    uint32_t firstOffset = 0;
    uint32_t size = 0;
};

bool ReadMsvcTournamentDequeState(
    uintptr_t dequeAddress,
    MsvcTournamentDequeState* out)
{
    if (out == nullptr)
    {
        return false;
    }
    MsvcTournamentDequeState value = {};
    if (!GuardedRead(dequeAddress + 4u, &value.map)
        || !GuardedRead(dequeAddress + 8u, &value.mapSlots)
        || !GuardedRead(dequeAddress + 12u, &value.firstOffset)
        || !GuardedRead(dequeAddress + 16u, &value.size))
    {
        return false;
    }
    *out = value;
    return true;
}

bool SameMsvcTournamentDequeState(
    const MsvcTournamentDequeState& lhs,
    const MsvcTournamentDequeState& rhs)
{
    return lhs.map == rhs.map
        && lhs.mapSlots == rhs.mapSlots
        && lhs.firstOffset == rhs.firstOffset
        && lhs.size == rhs.size;
}

bool ReadMsvcTournamentQueue(
    uintptr_t dequeAddress,
    std::array<uint16_t,
        launcher_probe_detail::kTournamentAutoNavInputCount>* outValues,
    size_t* outCount)
{
    if (outValues == nullptr || outCount == nullptr)
    {
        return false;
    }
    *outCount = 0;

    MsvcTournamentDequeState before = {};
    if (!ReadMsvcTournamentDequeState(dequeAddress, &before)
        || before.map == 0
        || (before.map & (sizeof(uintptr_t) - 1u)) != 0u
        || before.mapSlots < 2u
        || before.mapSlots > 1024u
        || (before.mapSlots & (before.mapSlots - 1u)) != 0u
        || before.size
            > launcher_probe_detail::kTournamentAutoNavInputCount)
    {
        return false;
    }

    // MSVC's deque stores eight uint16_t values per block.  _Myoff is kept
    // modulo the total map capacity while pop_front advances it.
    constexpr uint64_t kElementsPerBlock = 8u;
    const uint64_t capacity =
        static_cast<uint64_t>(before.mapSlots) * kElementsPerBlock;
    if (capacity == 0u || before.firstOffset >= capacity)
    {
        return false;
    }

    for (uint32_t i = 0; i < before.size; ++i)
    {
        const uint64_t absoluteIndex =
            static_cast<uint64_t>(before.firstOffset) + i;
        const uintptr_t mapSlot = static_cast<uintptr_t>(
            (absoluteIndex / kElementsPerBlock)
            & (before.mapSlots - 1u));
        uintptr_t mapEntryAddress = 0;
        if (!CheckedAddressAdd(
                before.map,
                mapSlot * sizeof(uintptr_t),
                &mapEntryAddress))
        {
            return false;
        }

        uintptr_t block = 0;
        uintptr_t elementAddress = 0;
        const uintptr_t inBlock = static_cast<uintptr_t>(
            absoluteIndex & (kElementsPerBlock - 1u));
        if (!GuardedRead(mapEntryAddress, &block)
            || block == 0
            || (block & (alignof(uint16_t) - 1u)) != 0u
            || !CheckedAddressAdd(
                block, inBlock * sizeof(uint16_t), &elementAddress)
            || !GuardedRead(elementAddress, &(*outValues)[i]))
        {
            return false;
        }
    }

    // A native frame may consume the front while admission is sampling it.
    // Never validate a torn deque view; the bounded outer probe will retry.
    MsvcTournamentDequeState after = {};
    if (!ReadMsvcTournamentDequeState(dequeAddress, &after)
        || !SameMsvcTournamentDequeState(before, after))
    {
        return false;
    }
    *outCount = before.size;
    return true;
}

struct LibstdcppTournamentDequeState
{
    uintptr_t map = 0;
    uint32_t mapSlots = 0;
    uintptr_t startCur = 0;
    uintptr_t startFirst = 0;
    uintptr_t startLast = 0;
    uintptr_t startNode = 0;
    uintptr_t finishCur = 0;
    uintptr_t finishFirst = 0;
    uintptr_t finishLast = 0;
    uintptr_t finishNode = 0;
};

bool ReadLibstdcppTournamentDequeState(
    uintptr_t dequeAddress,
    LibstdcppTournamentDequeState* out)
{
    if (out == nullptr)
    {
        return false;
    }
    LibstdcppTournamentDequeState value = {};
    if (!GuardedRead(dequeAddress + 0x00u, &value.map)
        || !GuardedRead(dequeAddress + 0x04u, &value.mapSlots)
        || !GuardedRead(dequeAddress + 0x08u, &value.startCur)
        || !GuardedRead(dequeAddress + 0x0Cu, &value.startFirst)
        || !GuardedRead(dequeAddress + 0x10u, &value.startLast)
        || !GuardedRead(dequeAddress + 0x14u, &value.startNode)
        || !GuardedRead(dequeAddress + 0x18u, &value.finishCur)
        || !GuardedRead(dequeAddress + 0x1Cu, &value.finishFirst)
        || !GuardedRead(dequeAddress + 0x20u, &value.finishLast)
        || !GuardedRead(dequeAddress + 0x24u, &value.finishNode))
    {
        return false;
    }
    *out = value;
    return true;
}

bool SameLibstdcppTournamentDequeState(
    const LibstdcppTournamentDequeState& lhs,
    const LibstdcppTournamentDequeState& rhs)
{
    return lhs.map == rhs.map
        && lhs.mapSlots == rhs.mapSlots
        && lhs.startCur == rhs.startCur
        && lhs.startFirst == rhs.startFirst
        && lhs.startLast == rhs.startLast
        && lhs.startNode == rhs.startNode
        && lhs.finishCur == rhs.finishCur
        && lhs.finishFirst == rhs.finishFirst
        && lhs.finishLast == rhs.finishLast
        && lhs.finishNode == rhs.finishNode;
}

bool IsLibstdcppTournamentIteratorSane(
    uintptr_t cur,
    uintptr_t first,
    uintptr_t last)
{
    constexpr uintptr_t kBlockBytes = 512u;
    uintptr_t expectedLast = 0;
    return first != 0
        && (first & (alignof(uint16_t) - 1u)) == 0u
        && CheckedAddressAdd(first, kBlockBytes, &expectedLast)
        && last == expectedLast
        && cur >= first
        && cur < last
        && ((cur - first) % sizeof(uint16_t)) == 0u;
}

bool ReadLibstdcppTournamentQueue(
    uintptr_t dequeAddress,
    std::array<uint16_t,
        launcher_probe_detail::kTournamentAutoNavInputCount>* outValues,
    size_t* outCount)
{
    if (outValues == nullptr || outCount == nullptr)
    {
        return false;
    }
    *outCount = 0;

    LibstdcppTournamentDequeState before = {};
    if (!ReadLibstdcppTournamentDequeState(dequeAddress, &before)
        || before.map == 0
        || (before.map & (sizeof(uintptr_t) - 1u)) != 0u
        || before.mapSlots < 2u
        || before.mapSlots > 1024u
        || !IsLibstdcppTournamentIteratorSane(
            before.startCur, before.startFirst, before.startLast)
        || !IsLibstdcppTournamentIteratorSane(
            before.finishCur, before.finishFirst, before.finishLast))
    {
        return false;
    }

    uintptr_t lastMapEntry = 0;
    if (!CheckedAddressAdd(
            before.map,
            static_cast<uintptr_t>(before.mapSlots - 1u)
                * sizeof(uintptr_t),
            &lastMapEntry)
        || before.startNode < before.map
        || before.startNode > lastMapEntry
        || before.finishNode < before.startNode
        || before.finishNode > lastMapEntry
        || ((before.startNode - before.map) % sizeof(uintptr_t)) != 0u
        || ((before.finishNode - before.map) % sizeof(uintptr_t)) != 0u)
    {
        return false;
    }

    uintptr_t startBlock = 0;
    uintptr_t finishBlock = 0;
    if (!GuardedRead(before.startNode, &startBlock)
        || !GuardedRead(before.finishNode, &finishBlock)
        || startBlock != before.startFirst
        || finishBlock != before.finishFirst)
    {
        return false;
    }

    constexpr uint64_t kElementsPerBlock = 256u;
    const uint64_t nodeDistance =
        (before.finishNode - before.startNode) / sizeof(uintptr_t);
    uint64_t count = 0;
    if (nodeDistance == 0u)
    {
        if (before.finishCur < before.startCur)
        {
            return false;
        }
        count = (before.finishCur - before.startCur) / sizeof(uint16_t);
    }
    else
    {
        count = (before.startLast - before.startCur) / sizeof(uint16_t)
            + (nodeDistance - 1u) * kElementsPerBlock
            + (before.finishCur - before.finishFirst) / sizeof(uint16_t);
    }
    if (count > launcher_probe_detail::kTournamentAutoNavInputCount)
    {
        return false;
    }

    uintptr_t node = before.startNode;
    uintptr_t cur = before.startCur;
    uintptr_t last = before.startLast;
    for (size_t i = 0; i < static_cast<size_t>(count); ++i)
    {
        if (!GuardedRead(cur, &(*outValues)[i])
            || !CheckedAddressAdd(cur, sizeof(uint16_t), &cur))
        {
            return false;
        }
        if (cur == last)
        {
            if (!CheckedAddressAdd(node, sizeof(uintptr_t), &node)
                || node > lastMapEntry)
            {
                return false;
            }
            uintptr_t first = 0;
            if (!GuardedRead(node, &first)
                || !CheckedAddressAdd(first, 512u, &last))
            {
                return false;
            }
            cur = first;
        }
    }
    if (node != before.finishNode || cur != before.finishCur)
    {
        return false;
    }

    LibstdcppTournamentDequeState after = {};
    if (!ReadLibstdcppTournamentDequeState(dequeAddress, &after)
        || !SameLibstdcppTournamentDequeState(before, after))
    {
        return false;
    }
    *outCount = static_cast<size_t>(count);
    return true;
}

enum class TournamentQueueLayout
{
    Invalid,
    Msvc,
    Libstdcpp,
};

TournamentQueueLayout ResolveTournamentQueueLayout(
    const RevivalAddressProfile& profile)
{
    if (profile.versionTag == nullptr)
    {
        return TournamentQueueLayout::Invalid;
    }
    if (std::strcmp(profile.versionTag, "1.02j") == 0)
    {
        return profile.tournamentInputQueueOffset == 0x340u
            ? TournamentQueueLayout::Libstdcpp
            : TournamentQueueLayout::Invalid;
    }
    if (std::strcmp(profile.versionTag, "1.02i") == 0)
    {
        return profile.tournamentInputQueueOffset == 748u
            ? TournamentQueueLayout::Msvc
            : TournamentQueueLayout::Invalid;
    }
    const bool legacy = std::strcmp(profile.versionTag, "1.02e") == 0
        || std::strcmp(profile.versionTag, "1.02f") == 0
        || std::strcmp(profile.versionTag, "1.02f-framestepping") == 0
        || std::strcmp(profile.versionTag, "1.02g") == 0
        || std::strcmp(profile.versionTag, "1.02h") == 0;
    return legacy && profile.tournamentInputQueueOffset == 740u
        ? TournamentQueueLayout::Msvc
        : TournamentQueueLayout::Invalid;
}

launcher_probe_detail::TournamentAutoNavSuffixState
ReadTournamentAutoNavSuffixState(
    const RevivalAddressProfile& profile,
    uintptr_t sessionPtr)
{
    using launcher_probe_detail::TournamentAutoNavSuffixState;
    if (sessionPtr == 0 || profile.addrGameModeCurrentIndex == 0)
    {
        return TournamentAutoNavSuffixState::Invalid;
    }

    const TournamentQueueLayout layout = ResolveTournamentQueueLayout(profile);
    uintptr_t dequeAddress = 0;
    int modeBefore = -1;
    int modeAfter = -1;
    std::array<uint16_t,
        launcher_probe_detail::kTournamentAutoNavInputCount> values = {};
    size_t count = 0;
    if (layout == TournamentQueueLayout::Invalid
        || !CheckedAddressAdd(
            sessionPtr, profile.tournamentInputQueueOffset, &dequeAddress)
        || !GuardedRead(profile.addrGameModeCurrentIndex, &modeBefore))
    {
        return TournamentAutoNavSuffixState::Invalid;
    }

    const bool queueRead = layout == TournamentQueueLayout::Libstdcpp
        ? ReadLibstdcppTournamentQueue(dequeAddress, &values, &count)
        : ReadMsvcTournamentQueue(dequeAddress, &values, &count);
    if (!queueRead
        || !GuardedRead(profile.addrGameModeCurrentIndex, &modeAfter)
        || modeBefore != modeAfter)
    {
        return TournamentAutoNavSuffixState::Invalid;
    }
    return launcher_probe_detail::ClassifyTournamentAutoNavSuffix(
        values.data(), count, modeAfter);
}

revival_launch::SessionObjectKind ClassifyObject(
    const RevivalAddressProfile& profile,
    uintptr_t dllBase,
    uintptr_t sessionPtr)
{
    if (sessionPtr == 0)
    {
        return revival_launch::SessionObjectKind::None;
    }
    uintptr_t vtable = 0;
    if (!GuardedRead(sessionPtr, &vtable) || vtable < dllBase)
    {
        return revival_launch::SessionObjectKind::Unknown;
    }
    const uintptr_t rva = vtable - dllBase;
    if (rva == profile.onlineSessionVtableRva)
    {
        return revival_launch::SessionObjectKind::Online;
    }
    if (rva == profile.spectatorSessionVtableRva)
    {
        return revival_launch::SessionObjectKind::Spectator;
    }
    if (rva == profile.practiceSessionVtableRva)
    {
        return revival_launch::SessionObjectKind::Practice;
    }
    if (rva == profile.tournamentSessionVtableRva)
    {
        return revival_launch::SessionObjectKind::Tournament;
    }
    return revival_launch::SessionObjectKind::Unknown;
}

bool ValidateParentBinding(
    const RevivalAddressProfile& profile,
    revival_launch::SessionObjectKind kind,
    uintptr_t sessionPtr,
    DWORD parentPid,
    const FILETIME* parentCreationTime,
    bool* outHandlePresent,
    bool* outPidMatches)
{
    if (outHandlePresent == nullptr || outPidMatches == nullptr)
    {
        return false;
    }
    *outHandlePresent = false;
    *outPidMatches = false;

    uintptr_t handleOffset = 0;
    uintptr_t pidOffset = 0;
    if (kind == revival_launch::SessionObjectKind::Online)
    {
        handleOffset = profile.sessionOffsetHelperHandle;
        pidOffset = profile.sessionOffsetHelperPid;
    }
    else if (kind == revival_launch::SessionObjectKind::Spectator)
    {
        handleOffset = profile.spectatorSessionOffsetHelperHandle;
        pidOffset = profile.spectatorSessionOffsetHelperPid;
    }
    else
    {
        return false;
    }

    HANDLE handle = nullptr;
    int storedPid = 0;
    if (!GuardedRead(sessionPtr + handleOffset, &handle)
        || !GuardedRead(sessionPtr + pidOffset, &storedPid)
        || handle == nullptr)
    {
        return false;
    }

    *outHandlePresent = true;
    const DWORD handlePid = GetProcessId(handle);
    FILETIME handleCreation = {};
    FILETIME handleExit = {};
    FILETIME handleKernel = {};
    FILETIME handleUser = {};
    DWORD handleExitCode = 0;
    const bool creationMatches = parentCreationTime == nullptr
        || (GetProcessTimes(
                handle,
                &handleCreation,
                &handleExit,
                &handleKernel,
                &handleUser) != FALSE
            && SameFileTime(handleCreation, *parentCreationTime));
    const bool handleAlive = GetExitCodeProcess(handle, &handleExitCode) != FALSE
        && handleExitCode == STILL_ACTIVE;
    *outPidMatches = storedPid > 0
        && static_cast<DWORD>(storedPid) == parentPid
        && handlePid == parentPid
        && creationMatches
        && handleAlive;
    return true;
}

revival_launch::SessionRole RoleFromInt(int role)
{
    switch (role)
    {
    case kLocalRoleOnline:
        return revival_launch::SessionRole::Online;
    case kLocalRoleSpectate:
        return revival_launch::SessionRole::Spectator;
    case kLocalRoleLocalPlay:
        return revival_launch::SessionRole::Practice;
    case kLocalRoleTournament:
        return revival_launch::SessionRole::Tournament;
    default:
        return revival_launch::SessionRole::Unknown;
    }
}

bool IsCoherentGuardlessBootstrap(
    revival_launch::SessionRole role,
    revival_launch::SessionObjectKind objectKind,
    bool sessionPresent)
{
    switch (role)
    {
    case revival_launch::SessionRole::Practice:
        return !sessionPresent
            || objectKind == revival_launch::SessionObjectKind::Practice;
    case revival_launch::SessionRole::Tournament:
        return !sessionPresent
            || objectKind == revival_launch::SessionObjectKind::Tournament;
    case revival_launch::SessionRole::Online:
        return sessionPresent
            && objectKind == revival_launch::SessionObjectKind::Online;
    case revival_launch::SessionRole::Spectator:
        return sessionPresent
            && objectKind == revival_launch::SessionObjectKind::Spectator;
    case revival_launch::SessionRole::Unknown:
        return false;
    }
    return false;
}

bool DecodeRel32Target(
    uintptr_t instructionAddress,
    size_t instructionSize,
    int32_t displacement,
    uintptr_t* outTarget)
{
    if (outTarget == nullptr)
    {
        return false;
    }

    uintptr_t nextInstruction = 0;
    if (!CheckedAddressAdd(
            instructionAddress, instructionSize, &nextInstruction))
    {
        return false;
    }

    if (displacement >= 0)
    {
        return CheckedAddressAdd(
            nextInstruction,
            static_cast<uintptr_t>(displacement),
            outTarget);
    }

    const uintptr_t magnitude = static_cast<uintptr_t>(
        -static_cast<int64_t>(displacement));
    if (nextInstruction < magnitude)
    {
        return false;
    }
    *outTarget = nextInstruction - magnitude;
    return true;
}

bool IsExecutableCommittedRange(uintptr_t address, size_t size)
{
    if (address == 0 || size == 0)
    {
        return false;
    }

    uintptr_t requestedEnd = 0;
    if (!CheckedAddressAdd(address, size, &requestedEnd))
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(
            reinterpret_cast<const void*>(address),
            &memory,
            sizeof(memory)) != sizeof(memory)
        || memory.State != MEM_COMMIT
        || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
    {
        return false;
    }

    const DWORD baseProtection = memory.Protect & 0xFFu;
    const bool executable = baseProtection == PAGE_EXECUTE
        || baseProtection == PAGE_EXECUTE_READ
        || baseProtection == PAGE_EXECUTE_READWRITE
        || baseProtection == PAGE_EXECUTE_WRITECOPY;
    uintptr_t regionEnd = 0;
    return executable
        && CheckedAddressAdd(
            reinterpret_cast<uintptr_t>(memory.BaseAddress),
            static_cast<uintptr_t>(memory.RegionSize),
            &regionEnd)
        && address >= reinterpret_cast<uintptr_t>(memory.BaseAddress)
        && requestedEnd <= regionEnd;
}

bool IsNativeFrameBootstrapInstalled(
    const RevivalAddressProfile& profile,
    uintptr_t revivalBase)
{
    // Every supported exported init installs the same 10-byte EFZ dispatcher
    // hook, but an opcode-only E9 witness can be supplied by any unrelated
    // owner. Bind the complete trampoline to this exact Revival generation:
    // native wrapper shape, profile callback, displaced stock instructions,
    // and the exact return edge must all agree.
    static constexpr uintptr_t kExeFrameHookAddress = 0x00401582u;
    static constexpr size_t kStolenBytes = 10u;
    static constexpr size_t kTrampolinePrefixBytes = 9u;
    static constexpr size_t kTrampolineSize =
        kTrampolinePrefixBytes + kStolenBytes + 5u;
    static constexpr uint8_t kStockDisplaced[kStolenBytes] = {
        0xC7, 0x05, 0x4C, 0x01, 0x79,
        0x00, 0x01, 0x00, 0x00, 0x00,
    };

    if (revivalBase == 0 || profile.frameHookRva == 0)
    {
        return false;
    }

    uintptr_t revivalImageBase = 0;
    uintptr_t revivalImageEnd = 0;
    uintptr_t expectedCallback = 0;
    if (!ReadModuleImageRange(
            reinterpret_cast<HMODULE>(revivalBase),
            &revivalImageBase,
            &revivalImageEnd)
        || revivalImageBase != revivalBase
        || !CheckedAddressAdd(
            revivalBase, profile.frameHookRva, &expectedCallback)
        || expectedCallback < revivalImageBase
        || expectedCallback >= revivalImageEnd
        || !IsExecutableCommittedRange(expectedCallback, 1u))
    {
        return false;
    }

    uint8_t site[kStolenBytes] = {};
    if (!GuardedReadBytes(kExeFrameHookAddress, site, sizeof(site))
        || site[0] != 0xE9)
    {
        return false;
    }
    for (size_t i = 5u; i < kStolenBytes; ++i)
    {
        if (site[i] != 0x90)
        {
            return false;
        }
    }

    int32_t trampolineRel = 0;
    std::memcpy(&trampolineRel, &site[1], sizeof(trampolineRel));
    uintptr_t trampoline = 0;
    if (!DecodeRel32Target(
            kExeFrameHookAddress, 5u, trampolineRel, &trampoline)
        || !IsExecutableCommittedRange(trampoline, kTrampolineSize))
    {
        return false;
    }

    uint8_t trampolineBytes[kTrampolineSize] = {};
    if (!GuardedReadBytes(
            trampoline, trampolineBytes, sizeof(trampolineBytes))
        || trampolineBytes[0] != 0x60
        || trampolineBytes[1] != 0x9C
        || trampolineBytes[2] != 0xE8
        || trampolineBytes[7] != 0x9D
        || trampolineBytes[8] != 0x61
        || trampolineBytes[kTrampolinePrefixBytes + kStolenBytes] != 0xE9
        || std::memcmp(
            &trampolineBytes[kTrampolinePrefixBytes],
            kStockDisplaced,
            sizeof(kStockDisplaced)) != 0)
    {
        return false;
    }

    int32_t callbackRel = 0;
    std::memcpy(&callbackRel, &trampolineBytes[3], sizeof(callbackRel));
    uintptr_t callback = 0;
    int32_t returnRel = 0;
    std::memcpy(
        &returnRel,
        &trampolineBytes[kTrampolinePrefixBytes + kStolenBytes + 1u],
        sizeof(returnRel));
    uintptr_t returnTarget = 0;
    return DecodeRel32Target(
               trampoline + 2u, 5u, callbackRel, &callback)
        && DecodeRel32Target(
            trampoline + kTrampolinePrefixBytes + kStolenBytes,
            5u,
            returnRel,
            &returnTarget)
        && callback == expectedCallback
        && returnTarget == kExeFrameHookAddress + kStolenBytes;
}

bool ReadSessionReadiness(
    const RevivalAddressProfile& profile,
    revival_launch::SessionObjectKind objectKind,
    uintptr_t sessionPtr,
    uint32_t* outValue,
    bool* outSatisfied)
{
    if (outValue == nullptr || outSatisfied == nullptr)
    {
        return false;
    }

    *outValue = 0;
    *outSatisfied = true;

    uintptr_t offset = 0;
    RevivalSessionReadinessCheck check =
        RevivalSessionReadinessCheck::None;
    if (objectKind == revival_launch::SessionObjectKind::Online)
    {
        offset = profile.onlineSessionReadinessOffset;
        check = profile.onlineSessionReadinessCheck;
    }
    else if (objectKind == revival_launch::SessionObjectKind::Spectator)
    {
        offset = profile.spectatorSessionReadinessOffset;
        check = profile.spectatorSessionReadinessCheck;
    }
    else if (objectKind == revival_launch::SessionObjectKind::Tournament)
    {
        // Tournament publishes its object before every EFZ patch is
        // necessarily visible.  Admission is safe only after the complete
        // native constructor patch shape has settled.  The later attachment
        // transaction performs stricter executable-target validation before
        // seeding the pristine restoration journal.
        static constexpr uintptr_t kTournamentPatchAddresses[] = {
            0x763F04u, 0x763E50u, 0x754C1Au, 0x7599EDu,
        };
        static constexpr size_t kTournamentPatchSizes[] = {
            7u, 7u, 1u, 20u,
        };
        uint8_t patch0[7] = {};
        uint8_t patch1[7] = {};
        uint8_t patch2 = 0xFF;
        uint8_t patch3[20] = {};
        if (profile.tournamentExePatchCount != 4u
            || profile.tournamentExePatchAddr[0]
                != kTournamentPatchAddresses[0]
            || profile.tournamentExePatchAddr[1]
                != kTournamentPatchAddresses[1]
            || profile.tournamentExePatchAddr[2]
                != kTournamentPatchAddresses[2]
            || profile.tournamentExePatchAddr[3]
                != kTournamentPatchAddresses[3]
            || profile.tournamentExePatchSize[0] != kTournamentPatchSizes[0]
            || profile.tournamentExePatchSize[1] != kTournamentPatchSizes[1]
            || profile.tournamentExePatchSize[2] != kTournamentPatchSizes[2]
            || profile.tournamentExePatchSize[3] != kTournamentPatchSizes[3]
            || !GuardedReadBytes(
                kTournamentPatchAddresses[0], patch0, sizeof(patch0))
            || !GuardedReadBytes(
                kTournamentPatchAddresses[1], patch1, sizeof(patch1))
            || !GuardedRead(
                kTournamentPatchAddresses[2], &patch2)
            || !GuardedReadBytes(
                kTournamentPatchAddresses[3], patch3, sizeof(patch3)))
        {
            return false;
        }

        uint32_t witness = 0;
        if (patch0[0] == 0xE9 && patch0[5] == 0x90 && patch0[6] == 0x90)
        {
            witness |= 0x1u;
        }
        if (patch1[0] == 0xE9 && patch1[5] == 0x90 && patch1[6] == 0x90)
        {
            witness |= 0x2u;
        }
        if (patch2 == 0x00)
        {
            witness |= 0x4u;
        }
        bool allNops = true;
        for (size_t i = 0; i < sizeof(patch3); ++i)
        {
            allNops = allNops && patch3[i] == 0x90;
        }
        if (allNops)
        {
            witness |= 0x8u;
        }
        *outValue = witness;
        if (witness != 0xFu)
        {
            *outSatisfied = false;
            return true;
        }

        // Do not admit a merely role-shaped Tournament object.  Its native
        // constructor also seeds a canonical 22-input auto-navigation deque,
        // which may have advanced by the time our DLL is initialized.  Read
        // and validate the live suffix on every snapshot; only the stable EXE
        // patch witness is retained in RevivalLauncherProbe because the queue
        // count necessarily changes as frames run.
        const launcher_probe_detail::TournamentAutoNavSuffixState queueState =
            ReadTournamentAutoNavSuffixState(profile, sessionPtr);
        if (queueState
            == launcher_probe_detail::TournamentAutoNavSuffixState::Invalid)
        {
            return false;
        }
        *outSatisfied = queueState
            == launcher_probe_detail::TournamentAutoNavSuffixState::Ready;
        return true;
    }
    else
    {
        // Practice has a valid null/session-stable local state and needs no
        // additional object readiness witness.
        return true;
    }

    // Every catalogued Online/Spectator layout must explicitly identify its
    // witness.  A missing profile field is an admission error, not readiness.
    if (sessionPtr == 0
        || offset == 0
        || check == RevivalSessionReadinessCheck::None
        || !GuardedRead(sessionPtr + offset, outValue))
    {
        return false;
    }

    *outSatisfied = RevivalSessionReadinessSatisfied(check, *outValue);
    return true;
}

bool ReadLauncherSessionSnapshot(
    const RevivalLauncherProbe& probe,
    HMODULE revival,
    LauncherSessionSnapshot* outSnapshot)
{
    if (outSnapshot == nullptr || probe.profile == nullptr || revival == nullptr)
    {
        return false;
    }
    *outSnapshot = {};

    const RevivalAddressProfile& profile = *probe.profile;
    const uintptr_t dllBase = reinterpret_cast<uintptr_t>(revival);
    uintptr_t sessionPtr = 0;
    int role = -1;
    if (!GuardedRead(dllBase + profile.canonicalRoleFlagOffset, &role)
        || !GuardedRead(
            dllBase + profile.canonicalSessionPtrOffset,
            &sessionPtr))
    {
        return false;
    }

    revival_launch::LaunchEvidence evidence = {};
    evidence.externalParentPresent = probe.exactExternalParent;
    evidence.exactParentExecutable = probe.exactExternalParent;
    evidence.revivalDllPresent = true;
    evidence.exactRevivalDll = true;
    evidence.supportedProfile = true;
    evidence.role = RoleFromInt(role);
    evidence.sessionPresent = sessionPtr != 0;
    evidence.objectKind = ClassifyObject(profile, dllBase, sessionPtr);
    evidence.externalBootstrapComplete = IsNativeFrameBootstrapInstalled(
        profile, dllBase)
        && IsCoherentGuardlessBootstrap(
            evidence.role,
            evidence.objectKind,
            evidence.sessionPresent);

    uintptr_t vtable = 0;
    uintptr_t helperHandleValue = 0;
    int helperPid = -1;
    uint32_t readinessValue = 0;
    bool readinessSatisfied = true;
    int activePlayer = -1;
    if (sessionPtr != 0)
    {
        if (!GuardedRead(sessionPtr, &vtable))
        {
            return false;
        }

        uintptr_t helperHandleOffset = 0;
        uintptr_t helperPidOffset = 0;
        if (evidence.objectKind == revival_launch::SessionObjectKind::Online)
        {
            helperHandleOffset = profile.sessionOffsetHelperHandle;
            helperPidOffset = profile.sessionOffsetHelperPid;
            if (!GuardedRead(
                    sessionPtr + profile.sessionOffsetActivePlayer,
                    &activePlayer))
            {
                return false;
            }
        }
        else if (evidence.objectKind
                    == revival_launch::SessionObjectKind::Spectator)
        {
            helperHandleOffset = profile.spectatorSessionOffsetHelperHandle;
            helperPidOffset = profile.spectatorSessionOffsetHelperPid;
        }

        if (helperHandleOffset != 0
            && (!GuardedRead(
                    sessionPtr + helperHandleOffset,
                    &helperHandleValue)
                || !GuardedRead(
                    sessionPtr + helperPidOffset,
                    &helperPid)))
        {
            return false;
        }

        if (!ReadSessionReadiness(
                profile,
                evidence.objectKind,
                sessionPtr,
                &readinessValue,
                &readinessSatisfied))
        {
            return false;
        }
    }

    if (probe.exactExternalParent
        && (evidence.objectKind == revival_launch::SessionObjectKind::Online
            || evidence.objectKind
                == revival_launch::SessionObjectKind::Spectator))
    {
        bool handlePresent = false;
        bool pidMatches = false;
        if (!ValidateParentBinding(
                profile,
                evidence.objectKind,
                sessionPtr,
                probe.parentPid,
                &probe.parentCreationTime,
                &handlePresent,
                &pidMatches))
        {
            return false;
        }
        evidence.parentProcessHandlePresent = handlePresent;
        evidence.sessionPidMatchesParent = pidMatches;
    }

    outSnapshot->disposition = revival_launch::ClassifyLaunch(evidence);
    outSnapshot->dllBase = dllBase;
    outSnapshot->sessionPtr = sessionPtr;
    outSnapshot->vtable = vtable;
    outSnapshot->helperHandleValue = helperHandleValue;
    outSnapshot->helperPid = helperPid;
    outSnapshot->readinessValue = readinessValue;
    outSnapshot->readinessSatisfied = readinessSatisfied;
    outSnapshot->role = role;
    outSnapshot->activePlayer = activePlayer;
    return true;
}

bool SameLauncherSessionSnapshot(
    const LauncherSessionSnapshot& lhs,
    const LauncherSessionSnapshot& rhs)
{
    return lhs.disposition == rhs.disposition
        && lhs.dllBase == rhs.dllBase
        && lhs.sessionPtr == rhs.sessionPtr
        && lhs.vtable == rhs.vtable
        && lhs.helperHandleValue == rhs.helperHandleValue
        && lhs.helperPid == rhs.helperPid
        && lhs.readinessValue == rhs.readinessValue
        && lhs.readinessSatisfied == rhs.readinessSatisfied
        && lhs.role == rhs.role
        && lhs.activePlayer == rhs.activePlayer;
}

void RetainAdmittedLauncherSnapshot(
    RevivalLauncherProbe* probe,
    const LauncherSessionSnapshot& snapshot)
{
    if (probe == nullptr)
    {
        return;
    }
    probe->disposition = snapshot.disposition;
    probe->revivalDllBase = snapshot.dllBase;
    probe->sessionPtr = snapshot.sessionPtr;
    probe->sessionVtable = snapshot.vtable;
    probe->helperHandleValue = snapshot.helperHandleValue;
    probe->helperPid = snapshot.helperPid;
    probe->readinessValue = snapshot.readinessValue;
    probe->readinessSatisfied = snapshot.readinessSatisfied;
    probe->role = snapshot.role;
    probe->activePlayer = snapshot.activePlayer;
}

bool MatchesAdmittedLauncherSnapshot(
    const RevivalLauncherProbe& probe,
    const LauncherSessionSnapshot& snapshot)
{
    return probe.disposition == snapshot.disposition
        && probe.revivalDllBase == snapshot.dllBase
        && probe.sessionPtr == snapshot.sessionPtr
        && probe.sessionVtable == snapshot.vtable
        && probe.helperHandleValue == snapshot.helperHandleValue
        && probe.helperPid == snapshot.helperPid
        && probe.readinessValue == snapshot.readinessValue
        && probe.readinessSatisfied == snapshot.readinessSatisfied
        && probe.role == snapshot.role
        && probe.activePlayer == snapshot.activePlayer;
}

bool ProbePreloadedExistingRevival(RevivalLauncherProbe* result)
{
    if (result == nullptr)
    {
        return false;
    }
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    DetectRevivalVersion();
    const RevivalAddressProfile* profile = g_activeRevival;
    const bool supported = ActiveRevivalProfileSupportsSessionStart();
    const bool exactDll = supported && profile != nullptr
        && ProfileMatchesDllFile(*profile, revival);
    if (!exactDll)
    {
        result->disposition =
            revival_launch::LaunchDisposition::PassiveFailClosed;
        mod::Log(
            "LauncherProbe: preloaded Revival DLL is unsupported or not an exact catalog build; fail closed");
        return true;
    }

    result->profile = profile;
    LauncherSessionSnapshot first = {};
    LauncherSessionSnapshot second = {};
    if (!ReadLauncherSessionSnapshot(*result, revival, &first)
        || !ReadLauncherSessionSnapshot(*result, revival, &second)
        || !SameLauncherSessionSnapshot(first, second))
    {
        result->disposition =
            revival_launch::LaunchDisposition::PassiveFailClosed;
        mod::Log(
            "LauncherProbe: preloaded Revival session changed during admission; fail closed");
        return true;
    }
    RetainAdmittedLauncherSnapshot(result, second);
    mod::Log(
        "LauncherProbe: exact preloaded Revival without live parent disposition=%d version=%s role=%d session=0x%08lX",
        static_cast<int>(result->disposition),
        profile->versionTag != nullptr ? profile->versionTag : "unknown",
        result->role,
        static_cast<unsigned long>(result->sessionPtr));
    return true;
}
}

RevivalLauncherProbe ProbeRevivalLauncherStartup(DWORD timeoutMs)
{
    RevivalLauncherProbe result = {};
    result.disposition = revival_launch::LaunchDisposition::DirectGameHost;

    ParentCandidate parent = {};
    bool revivalNamed = false;
    if (!CaptureExactParent(&parent, &revivalNamed))
    {
        if (ProbePreloadedExistingRevival(&result))
        {
            return result;
        }
        if (revivalNamed)
        {
            result.disposition =
                revival_launch::LaunchDisposition::PassiveFailClosed;
            mod::Log(
                "LauncherProbe: existing/native Revival provenance failed exact identity; fail closed");
        }
        else
        {
            mod::Log("LauncherProbe: no exact Revival parent; direct EFZ path");
        }
        return result;
    }

    result.exactExternalParent = true;
    result.parentPid = parent.pid;
    result.parentCreationTime = parent.creationTime;
    mod::Log(
        "LauncherProbe: exact external Revival parent pid=%lu timestamp=0x%08X path='%s'",
        static_cast<unsigned long>(parent.pid),
        static_cast<unsigned>(parent.timestamp),
        parent.imagePath.c_str());

    const DWORD startTick = GetTickCount();
    for (;;)
    {
        if (!ParentStillHasSameIdentity(parent))
        {
            // Standalone Practice/Tournament launchers intentionally exit as
            // soon as EFZ is resumed.  The exact initialized DLL remains in
            // the child and is sufficient to prevent a destructive second
            // init, but never sufficient to adopt an orphan online session.
            result.exactExternalParent = false;
            result.parentPid = 0;
            result.parentCreationTime = {};
            if (parent.process != nullptr)
            {
                CloseHandle(parent.process);
                parent.process = nullptr;
            }
            if (!ProbePreloadedExistingRevival(&result))
            {
                result.disposition =
                    revival_launch::LaunchDisposition::PassiveFailClosed;
            }
            mod::Log(
                "LauncherProbe: verified parent exited before attachment; child disposition=%d",
                static_cast<int>(result.disposition));
            break;
        }

        HMODULE revival = GetModuleHandleA("EfzRevival.dll");
        if (revival != nullptr)
        {
            DetectRevivalVersion();
            const RevivalAddressProfile* profile = g_activeRevival;
            const bool supported = ActiveRevivalProfileSupportsSessionStart();
            const bool parentPair = supported
                && ParentAcceptsProfile(parent, profile);
            const bool exactDll = parentPair
                && ProfileMatchesDllFile(*profile, revival);

            if (exactDll)
            {
                result.profile = profile;
                LauncherSessionSnapshot first = {};
                LauncherSessionSnapshot second = {};
                if (ReadLauncherSessionSnapshot(result, revival, &first)
                    && ReadLauncherSessionSnapshot(result, revival, &second)
                    && SameLauncherSessionSnapshot(first, second)
                    && (second.disposition
                        == revival_launch::LaunchDisposition::AttachExistingPractice
                    || second.disposition
                        == revival_launch::LaunchDisposition::AdoptExternalOnline
                    || second.disposition
                        == revival_launch::LaunchDisposition::AdoptExternalSpectator
                    || second.disposition
                        == revival_launch::LaunchDisposition::AttachExistingTournament))
                {
                    const bool roleRequiresReadiness =
                        second.disposition
                                == revival_launch::LaunchDisposition::AdoptExternalOnline
                            || second.disposition
                                == revival_launch::LaunchDisposition::AdoptExternalSpectator
                            || second.disposition
                                == revival_launch::LaunchDisposition::AttachExistingTournament;
                    if (roleRequiresReadiness && !second.readinessSatisfied)
                    {
                        // A matching object can become visible before its
                        // role-specific constructor/post-init witness.  This
                        // is an expected transient state, so keep polling the
                        // exact object rather than converting it into a
                        // permanent provenance failure.
                        if (GetTickCount() - startTick >= timeoutMs)
                        {
                            result.disposition =
                                revival_launch::LaunchDisposition::PassiveFailClosed;
                            mod::Log(
                                "LauncherProbe: %s object never published readiness value (0x%08X); fail closed",
                                second.disposition
                                        == revival_launch::LaunchDisposition::AdoptExternalOnline
                                    ? "Online"
                                    : (second.disposition
                                                == revival_launch::LaunchDisposition::AdoptExternalSpectator
                                            ? "Spectator"
                                            : "Tournament"),
                                static_cast<unsigned>(second.readinessValue));
                            break;
                        }
                        Sleep(10);
                        continue;
                    }

                    // Online role 0 covers both host and join.  Recovery must
                    // know whether Revival applied the client input swap, so
                    // do not accept the object until this canonical field has
                    // settled to P1 or P2.
                    if (second.disposition
                            == revival_launch::LaunchDisposition::AdoptExternalOnline
                        && second.activePlayer != 0
                        && second.activePlayer != 1)
                    {
                        if (GetTickCount() - startTick >= timeoutMs)
                        {
                            result.disposition =
                                revival_launch::LaunchDisposition::PassiveFailClosed;
                            mod::Log(
                                "LauncherProbe: online object never published a canonical activePlayer; fail closed");
                            break;
                        }
                        Sleep(10);
                        continue;
                    }
                    RetainAdmittedLauncherSnapshot(&result, second);
                    break;
                }
            }
            else if (supported || profile != nullptr)
            {
                result.disposition =
                    revival_launch::LaunchDisposition::PassiveFailClosed;
                mod::Log(
                    "LauncherProbe: parent/DLL pair mismatch parentTs=0x%08X dllProfile=%s; fail closed",
                    static_cast<unsigned>(parent.timestamp),
                    profile != nullptr && profile->versionTag != nullptr
                        ? profile->versionTag
                        : "unknown");
                break;
            }
        }

        if (GetTickCount() - startTick >= timeoutMs)
        {
            result.disposition =
                revival_launch::LaunchDisposition::PassiveFailClosed;
            mod::Log(
                "LauncherProbe: timed out after %lums waiting for exact launcher session; fail closed",
                static_cast<unsigned long>(timeoutMs));
            break;
        }
        Sleep(10);
    }

    if (result.disposition
            == revival_launch::LaunchDisposition::AttachExistingPractice
        || result.disposition
            == revival_launch::LaunchDisposition::AdoptExternalOnline
        || result.disposition
            == revival_launch::LaunchDisposition::AdoptExternalSpectator)
    {
        result.parentProcess = parent.process;
        parent.process = nullptr;
    }

    if (parent.process != nullptr)
    {
        CloseHandle(parent.process);
    }
    return result;
}

static bool RevalidateRevivalLauncherStartupImpl(
    const RevivalLauncherProbe& probe,
    bool verifyExactFiles)
{
    const bool managedDisposition =
        probe.disposition
            == revival_launch::LaunchDisposition::AttachExistingPractice
        || probe.disposition
            == revival_launch::LaunchDisposition::AdoptExternalOnline
        || probe.disposition
            == revival_launch::LaunchDisposition::AdoptExternalSpectator
        || probe.disposition
            == revival_launch::LaunchDisposition::AttachExistingTournament;
    if (!managedDisposition
        || probe.profile == nullptr
        || !SameProfileIdentity(g_activeRevival, probe.profile)
        || !ActiveRevivalProfileSupportsSessionStart())
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr
        || reinterpret_cast<uintptr_t>(revival) != probe.revivalDllBase
        || (verifyExactFiles
            && !ProfileMatchesDllFile(*probe.profile, revival)))
    {
        return false;
    }

    const bool externalActive =
        probe.disposition
            == revival_launch::LaunchDisposition::AdoptExternalOnline
        || probe.disposition
            == revival_launch::LaunchDisposition::AdoptExternalSpectator;
    if (externalActive
        && !(verifyExactFiles
            ? ParentMatchesProfileNow(probe, *probe.profile)
            : ParentIdentityStillLive(probe)))
    {
        return false;
    }

    // Read the canonical globals/object/binding/readiness twice.  The retained
    // values are exact identity/stable readiness witnesses, so a replacement
    // at the same address or recycled helper handle cannot pass.  Tournament's
    // queue contents are deliberately not retained: each read validates a
    // fresh canonical suffix, allowing the native front-pop count to advance
    // between admission and this parked revalidation.
    LauncherSessionSnapshot first = {};
    LauncherSessionSnapshot second = {};
    if (!ReadLauncherSessionSnapshot(probe, revival, &first)
        || !ReadLauncherSessionSnapshot(probe, revival, &second)
        || !SameLauncherSessionSnapshot(first, second)
        || !MatchesAdmittedLauncherSnapshot(probe, first)
        || ((externalActive
                || probe.disposition
                    == revival_launch::LaunchDisposition::AttachExistingTournament)
            && !first.readinessSatisfied))
    {
        return false;
    }

    if (probe.disposition
            == revival_launch::LaunchDisposition::AdoptExternalOnline
        && (first.activePlayer != 0 && first.activePlayer != 1))
    {
        return false;
    }

    // The exact parent image/file was checked before the mutable samples.
    // Bracket them with the retained handle identity/liveness only; hashing
    // the same file a second time adds disk latency without strengthening the
    // identity of the already-open process object.
    return !externalActive || ParentIdentityStillLive(probe);
}

bool RevalidateRevivalLauncherStartup(const RevivalLauncherProbe& probe)
{
    return RevalidateRevivalLauncherStartupImpl(probe, true);
}

bool RevalidateRevivalLauncherSessionSnapshot(
    const RevivalLauncherProbe& probe)
{
    return RevalidateRevivalLauncherStartupImpl(probe, false);
}

void ReleaseRevivalLauncherProbe(RevivalLauncherProbe* probe)
{
    if (probe == nullptr)
    {
        return;
    }
    if (probe->parentProcess != nullptr)
    {
        CloseHandle(probe->parentProcess);
        probe->parentProcess = nullptr;
    }
    probe->parentPid = 0;
}
}
