#include "netplay/bridge/takeover_internal.h"

namespace netplay::bridge::takeover
{
bool InjectSelf(HANDLE process, uintptr_t* outRemoteBase)
{
    if (outRemoteBase == nullptr)
    {
        return false;
    }

    if (IsRunningUnderWine())
    {
        mod::Log("Takeover [Wine]: injecting via CreateRemoteThread(LoadLibraryW)");
    }
    wchar_t selfPath[MAX_PATH] = {};
    const DWORD pathLength = GetModuleFileNameW(SelfModule(), selfPath, MAX_PATH);
    if (pathLength == 0 || pathLength >= MAX_PATH)
    {
        mod::Log("Takeover: failed to get self module path");
        return false;
    }

    const size_t size = (static_cast<size_t>(pathLength) + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remotePath == nullptr)
    {
        mod::Log("Takeover: VirtualAllocEx for path failed");
        return false;
    }

    SIZE_T written = 0;
    if (WriteProcessMemory(process, remotePath, selfPath, size, &written) == FALSE || written != size)
    {
        mod::Log("Takeover: WriteProcessMemory for path failed");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW"));
    if (loadLibrary == nullptr)
    {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (thread == nullptr)
    {
        mod::Log("Takeover: CreateRemoteThread LoadLibraryW failed: %s", ErrorString(GetLastError()).c_str());
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, 10000);
    DWORD remoteBase = 0;
    if (wait != WAIT_OBJECT_0 || GetExitCodeThread(thread, &remoteBase) == FALSE || remoteBase == 0)
    {
        mod::Log("Takeover: remote LoadLibrary failed wait=%lu exit=0x%08lX", static_cast<unsigned long>(wait), static_cast<unsigned long>(remoteBase));
        CloseHandle(thread);
        // An unfinished loader can still read this buffer; its process reclaims it on exit.
        if (wait == WAIT_OBJECT_0) VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }

    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);

    *outRemoteBase = static_cast<uintptr_t>(remoteBase);
    mod::Log("Takeover: injected self module base=0x%08lX", static_cast<unsigned long>(remoteBase));
    return true;
}
}
