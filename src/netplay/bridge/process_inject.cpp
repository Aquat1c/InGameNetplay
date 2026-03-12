// Process injection, IAT patching, fake threads, and redirect allocations.

#include "netplay/bridge/takeover_internal.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <tlhelp32.h>
#include <windows.h>

// Forward declarations for the nb_stub exports (defined in iat_stubs.cpp).
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_CreateProcessA(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadProcessMemory(HANDLE, LPCVOID, LPVOID, SIZE_T, SIZE_T*);
extern "C" __declspec(dllexport) LPVOID WINAPI nb_stub_VirtualAllocEx(HANDLE, LPVOID, SIZE_T, DWORD, DWORD);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_VirtualFreeEx(HANDLE, LPVOID, SIZE_T, DWORD);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteProcessMemory(HANDLE, LPVOID, LPCVOID, SIZE_T, SIZE_T*);
extern "C" __declspec(dllexport) HANDLE WINAPI nb_stub_CreateRemoteThread(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_TerminateProcess(HANDLE, UINT);
extern "C" __declspec(dllexport) HANDLE WINAPI nb_stub_OpenProcess(DWORD, BOOL, DWORD);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadConsoleA(HANDLE, LPVOID, DWORD, LPDWORD, PCONSOLE_READCONSOLE_CONTROL);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadConsoleW(HANDLE, LPVOID, DWORD, LPDWORD, PCONSOLE_READCONSOLE_CONTROL);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleA(HANDLE, const VOID*, DWORD, LPDWORD, LPVOID);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleW(HANDLE, const VOID*, DWORD, LPDWORD, LPVOID);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleOutputCharacterA(HANDLE, LPCSTR, DWORD, COORD, LPDWORD);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleOutputCharacterW(HANDLE, LPCWSTR, DWORD, COORD, LPDWORD);
extern "C" __declspec(dllexport) VOID WINAPI nb_stub_OutputDebugStringA(LPCSTR);
extern "C" __declspec(dllexport) VOID WINAPI nb_stub_OutputDebugStringW(LPCWSTR);
extern "C" __declspec(dllexport) DWORD WINAPI nb_stub_WaitForSingleObject(HANDLE, DWORD);
extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_GetExitCodeThread(HANDLE, LPDWORD);
extern "C" __declspec(dllexport) DWORD WINAPI nb_stub_ResumeThread(HANDLE);

namespace netplay::bridge::takeover
{

// Helper used only in this TU.
static uint32_t RemoteExportAddress(uintptr_t remoteBase, const void* localExport)
{
    const uintptr_t localBase = reinterpret_cast<uintptr_t>(SelfModule());
    const uintptr_t localAddr = reinterpret_cast<uintptr_t>(localExport);
    return static_cast<uint32_t>(remoteBase + (localAddr - localBase));
}

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

    // ---- Injection path (shared by Wine and native Windows) ----------------
    // The child process is created suspended (CREATE_SUSPENDED), so we can
    // safely inject via CreateRemoteThread(LoadLibraryA).  Under Wine,
    // SelfPatchIat() in DllMain provides an additional safety net.
    // -----------------------------------------------------------------------
    if (IsRunningUnderWine())
    {
        mod::Log("Takeover [Wine]: injecting via CreateRemoteThread(LoadLibraryA)");
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

bool WaitForPreloadedSelf(DWORD processId, uintptr_t* outRemoteBase, DWORD timeoutMs)
{
    // Under Wine, WINEDLLOVERRIDES causes our DLL to load automatically
    // into the child process.  We poll the remote module list until the
    // DLL appears (or we time out).

    if (outRemoteBase == nullptr)
    {
        return false;
    }

    const std::string selfBaseLower = BaseLower(ModulePath(SelfModule()));
    if (selfBaseLower.empty())
    {
        mod::Log("Takeover [Wine]: failed to determine own module filename");
        return false;
    }

    const DWORD startTick = GetTickCount();
    for (;;)
    {
        const std::vector<RemoteModuleRecord> modules = EnumerateRemoteModules(processId);
        for (const RemoteModuleRecord& m : modules)
        {
            if (m.moduleLower == selfBaseLower && m.base != 0)
            {
                *outRemoteBase = m.base;
                mod::Log(
                    "Takeover [Wine]: found preloaded self module '%s' "
                    "base=0x%08lX in remote pid=%lu (waited %lums)",
                    selfBaseLower.c_str(),
                    static_cast<unsigned long>(m.base),
                    static_cast<unsigned long>(processId),
                    static_cast<unsigned long>(GetTickCount() - startTick));
                return true;
            }
        }

        const DWORD elapsed = GetTickCount() - startTick;
        if (elapsed >= timeoutMs)
        {
            mod::Log(
                "Takeover [Wine]: timed out waiting for preloaded '%s' "
                "in pid=%lu after %lums",
                selfBaseLower.c_str(),
                static_cast<unsigned long>(processId),
                static_cast<unsigned long>(elapsed));
            return false;
        }
        Sleep(50);
    }
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

// ---------------------------------------------------------------------------
// SelfPatchIat — in-process IAT patching for Wine/Proton
// ---------------------------------------------------------------------------
// Called from DllMain(DLL_PROCESS_ATTACH) under Wine so that all IAT entries
// in the host EXE already point to our nb_stub_* exports BEFORE the loader
// lock is released and main() starts.  This eliminates the race between the
// helper's main thread (which calls ReadConsoleA, CreateProcessA, etc.)
// and the host-side remote PatchIat() call.
//
// Safety notes for DllMain context:
//   - GetModuleHandleA(nullptr) — safe (no DLL load)
//   - Direct PE header reads — safe (in-process memory)
//   - VirtualProtect — safe (no cross-process call)
//   - No heap allocation beyond the patch map (std::unordered_map)
//   - No logging (mod::Log not initialised yet); use OutputDebugStringA
// ---------------------------------------------------------------------------
int SelfPatchIat()
{
    // Build a local patch map: function name → address of our stub.
    // Since we are in-process, the stub addresses are direct — no
    // base-relocation arithmetic needed.
    struct PatchEntry { const char* name; uint32_t address; };
    const PatchEntry entries[] = {
        { "CreateProcessA",                 static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_CreateProcessA)) },
        { "OpenProcess",                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_OpenProcess)) },
        { "ReadProcessMemory",              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_ReadProcessMemory)) },
        { "VirtualAllocEx",                 static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_VirtualAllocEx)) },
        { "VirtualFreeEx",                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_VirtualFreeEx)) },
        { "WriteProcessMemory",             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_WriteProcessMemory)) },
        { "CreateRemoteThread",             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_CreateRemoteThread)) },
        { "TerminateProcess",               static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_TerminateProcess)) },
        { "ReadConsoleA",                   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_ReadConsoleA)) },
        { "ReadConsoleW",                   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_ReadConsoleW)) },
        { "WriteFile",                      static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_WriteFile)) },
        { "WriteConsoleA",                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_WriteConsoleA)) },
        { "WriteConsoleW",                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_WriteConsoleW)) },
        { "WriteConsoleOutputCharacterA",   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_WriteConsoleOutputCharacterA)) },
        { "WriteConsoleOutputCharacterW",   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_WriteConsoleOutputCharacterW)) },
        { "OutputDebugStringA",             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_OutputDebugStringA)) },
        { "OutputDebugStringW",             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_OutputDebugStringW)) },
        { "WaitForSingleObject",            static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_WaitForSingleObject)) },
        { "GetExitCodeThread",              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_GetExitCodeThread)) },
        { "ResumeThread",                   static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&nb_stub_ResumeThread)) },
    };
    constexpr int kEntryCount = sizeof(entries) / sizeof(entries[0]);

    // Get the host EXE's base address (always mapped at process creation).
    const uintptr_t imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (imageBase == 0)
    {
        OutputDebugStringA("SelfPatchIat: GetModuleHandleA(nullptr) failed\n");
        return -1;
    }

    // Parse PE header from in-process memory.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        OutputDebugStringA("SelfPatchIat: bad DOS signature\n");
        return -1;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(imageBase + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        OutputDebugStringA("SelfPatchIat: bad NT signature\n");
        return -1;
    }

    const DWORD importRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (importRva == 0)
    {
        // No imports — nothing to patch (unusual but not an error).
        return 0;
    }

    int totalPatched = 0;
    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(imageBase + importRva);
    for (; desc->Name != 0; ++desc)
    {
        const char* dllName = reinterpret_cast<const char*>(imageBase + desc->Name);

        // Only patch kernel32.dll and its API-set forwarders.
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

        const DWORD oftRva = (desc->OriginalFirstThunk != 0) ? desc->OriginalFirstThunk : desc->FirstThunk;
        const DWORD ftRva = desc->FirstThunk;

        const auto* oft = reinterpret_cast<const IMAGE_THUNK_DATA32*>(imageBase + oftRva);
        auto* ft = reinterpret_cast<uint32_t*>(imageBase + ftRva);

        for (DWORD i = 0; oft[i].u1.AddressOfData != 0; ++i)
        {
            if (IMAGE_SNAP_BY_ORDINAL32(oft[i].u1.Ordinal))
            {
                continue;
            }

            const auto* importByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                imageBase + oft[i].u1.AddressOfData);
            const char* importName = reinterpret_cast<const char*>(importByName->Name);

            // Linear scan through entries — the list is small (21 entries).
            uint32_t targetAddr = 0;
            for (int e = 0; e < kEntryCount; ++e)
            {
                if (_stricmp(importName, entries[e].name) == 0)
                {
                    targetAddr = entries[e].address;
                    break;
                }
            }
            if (targetAddr == 0)
            {
                continue;
            }

            if (ft[i] != targetAddr)
            {
                DWORD oldProtect = 0;
                VirtualProtect(&ft[i], sizeof(uint32_t), PAGE_READWRITE, &oldProtect);
                ft[i] = targetAddr;
                DWORD ignored = 0;
                VirtualProtect(&ft[i], sizeof(uint32_t), oldProtect, &ignored);
                ++totalPatched;
            }
        }
    }

    // Diagnostic output via OutputDebugString (safe in DllMain context).
    char debugMsg[128];
    wsprintfA(debugMsg, "SelfPatchIat [Wine]: patched %d IAT entries in host EXE\n", totalPatched);
    OutputDebugStringA(debugMsg);

    return totalPatched;
}

} // namespace netplay::bridge::takeover
