#include "netplay/bridge/takeover_internal.h"

#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>

namespace
{
HMODULE g_fixtureModule = nullptr;
int g_failures = 0;
bool g_delayLoader = false;
HANDLE g_delayedThread = nullptr;
LPVOID g_remotePath = nullptr;

HANDLE WINAPI ControlledCreateRemoteThread(HANDLE process, LPSECURITY_ATTRIBUTES attributes,
    SIZE_T stackSize, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD threadId)
{
    HANDLE thread = CreateRemoteThread(process, attributes, stackSize, start, parameter,
        flags | (g_delayLoader ? CREATE_SUSPENDED : 0), threadId);
    if (thread != nullptr && g_delayLoader)
    {
        g_remotePath = parameter;
        DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &g_delayedThread,
            0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    return thread;
}
}

#define CreateRemoteThread ControlledCreateRemoteThread
#include "../src/netplay/bridge/dll_injection.cpp"
#undef CreateRemoteThread

namespace
{
void Expect(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++g_failures; }
}

struct Child
{
    PROCESS_INFORMATION process = {};
    ~Child()
    {
        if (process.hProcess != nullptr)
        {
            TerminateProcess(process.hProcess, 0);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hProcess);
        }
        if (process.hThread != nullptr) CloseHandle(process.hThread);
    }
};

bool HasModule(DWORD pid, uintptr_t base)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W module = {};
    module.dwSize = sizeof(module);
    bool found = false;
    for (BOOL ok = Module32FirstW(snapshot, &module); ok; ok = Module32NextW(snapshot, &module))
        if (reinterpret_cast<uintptr_t>(module.modBaseAddr) == base
            && _wcsicmp(module.szModule, L"fixture.dll") == 0) found = true;
    CloseHandle(snapshot);
    return found;
}
}

namespace mod
{
void Log(const char*, ...) {}
}

namespace netplay::bridge::takeover
{
HMODULE SelfModule() { return g_fixtureModule; }
bool IsRunningUnderWine() { return false; }
std::string ErrorString(DWORD code) { return std::to_string(code); }
}

namespace
{
void TestInjection(const wchar_t* suffix, bool delayLoader = false)
{
    g_delayLoader = delayLoader;
    g_delayedThread = nullptr;
    g_remotePath = nullptr;
    wchar_t temp[MAX_PATH] = {};
    wchar_t system[MAX_PATH] = {};
    wchar_t executable[MAX_PATH] = {};
    const DWORD tempLength = GetTempPathW(MAX_PATH, temp);
    const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
    const DWORD exeLength = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    Expect(tempLength > 0 && tempLength < MAX_PATH && systemLength > 0
        && systemLength < MAX_PATH && exeLength > 0 && exeLength < MAX_PATH,
        "fixture paths are available");
    if (tempLength == 0 || tempLength >= MAX_PATH || systemLength == 0
        || systemLength >= MAX_PATH || exeLength == 0 || exeLength >= MAX_PATH) return;

    const std::wstring directory = std::wstring(temp) + L"efz_injection_"
        + std::to_wstring(GetCurrentProcessId()) + suffix;
    const std::wstring fixture = directory + L"\\fixture.dll";
    const bool directoryCreated = CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
    Expect(directoryCreated, "fixture directory is created");
    if (!directoryCreated) return;
    const bool copied = CopyFileW((std::wstring(system) + L"\\version.dll").c_str(), fixture.c_str(), TRUE) != FALSE;
    Expect(copied, "system DLL fixture is copied");
    if (!copied) { RemoveDirectoryW(directory.c_str()); return; }

    g_fixtureModule = LoadLibraryExW(fixture.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    Expect(g_fixtureModule != nullptr, "fixture module loads without running its entry point");
    if (g_fixtureModule != nullptr)
    {
        {
            Child child;
            STARTUPINFOW startup = {};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE;
            const bool created = CreateProcessW(executable, nullptr, nullptr, nullptr, FALSE,
                CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child.process) != FALSE;
            Expect(created, "isolated suspended child is created");
            if (created)
            {
                uintptr_t remoteBase = 0;
                Expect(!netplay::bridge::takeover::InjectSelf(child.process.hProcess, nullptr),
                    "missing output pointer is rejected");
                const bool injected = netplay::bridge::takeover::InjectSelf(child.process.hProcess, &remoteBase);
                Expect(injected == !delayLoader, "injection reports completion or timeout");
                if (delayLoader)
                {
                    Expect(g_delayedThread != nullptr, "delayed loader thread is retained for the test");
                    MEMORY_BASIC_INFORMATION memory = {};
                    const bool pathRetained = g_remotePath != nullptr
                        && VirtualQueryEx(child.process.hProcess, g_remotePath, &memory, sizeof(memory)) != 0
                        && memory.State == MEM_COMMIT;
                    Expect(pathRetained, "timed-out loader retains its path allocation");
                    if (pathRetained && g_delayedThread != nullptr)
                    {
                        Expect(ResumeThread(g_delayedThread) != static_cast<DWORD>(-1),
                            "delayed loader resumes");
                        Expect(WaitForSingleObject(g_delayedThread, 5000) == WAIT_OBJECT_0,
                            "delayed loader completes");
                        DWORD loaded = 0;
                        Expect(GetExitCodeThread(g_delayedThread, &loaded) != FALSE
                            && loaded != 0 && loaded != STILL_ACTIVE
                            && HasModule(child.process.dwProcessId, loaded),
                            "loader still loads the DLL after injection timed out");
                    }
                    if (g_delayedThread != nullptr) CloseHandle(g_delayedThread);
                    g_delayedThread = nullptr;
                }
                else
                {
                    Expect(remoteBase != 0 && HasModule(child.process.dwProcessId, remoteBase),
                        "child contains the DLL at the returned module base");
                }
            }
        }
        FreeLibrary(g_fixtureModule);
        g_fixtureModule = nullptr;
    }
    Expect(DeleteFileW(fixture.c_str()) != FALSE, "fixture DLL is removed");
    Expect(RemoveDirectoryW(directory.c_str()) != FALSE, "fixture directory is removed");
}
}

int main()
{
    TestInjection(L"_timeout", true);
    TestInjection(L"_ascii");
    TestInjection(L"_\u6C38\u9060_\u03A9");
    return g_failures == 0 ? 0 : 1;
}
