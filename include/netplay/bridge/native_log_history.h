#pragma once

#include <windows.h>
#include <string>

namespace netplay::bridge::takeover
{
void ConfigureNativeLogHistory(const std::wstring& gameDirectory, bool existingSession);
HANDLE WINAPI OpenHostLogFileA(LPCSTR path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile);
HANDLE WINAPI OpenHostLogFileW(LPCWSTR path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile);
}
