#include "netplay/bridge/native_log_history.h"
#include "netplay/core/mod_settings.h"
#include "logger.h"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>

namespace netplay::bridge::takeover
{
namespace
{
std::mutex g_nativeLogMutex;
std::wstring g_nativeLogPath;
bool g_nativeLogOpened = false;
unsigned long g_nativeLogSession = 0;

bool IsNativeLogOpen(LPCWSTR path, DWORD access, DWORD disposition)
{
    if (path == nullptr || g_nativeLogPath.empty()
        || (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA)) == 0
        || (disposition != CREATE_ALWAYS && disposition != TRUNCATE_EXISTING))
        return false;

    const wchar_t* base = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    if (_wcsicmp(base, L"logEfz.txt") != 0) return false;

    wchar_t absolute[MAX_PATH] = {};
    const DWORD length = GetFullPathNameW(path, MAX_PATH, absolute, nullptr);
    return length > 0 && length < MAX_PATH && _wcsicmp(absolute, g_nativeLogPath.c_str()) == 0;
}

DWORD AppendAccess(DWORD access)
{
    if ((access & GENERIC_WRITE) != 0)
        access = (access & ~GENERIC_WRITE) | FILE_GENERIC_WRITE;
    if ((access & GENERIC_ALL) != 0)
        access = (access & ~GENERIC_ALL) | FILE_ALL_ACCESS;
    return (access & ~FILE_WRITE_DATA) | FILE_APPEND_DATA;
}
}

void ConfigureNativeLogHistory(const std::wstring& gameDirectory, bool existingSession)
{
    std::lock_guard<std::mutex> lock(g_nativeLogMutex);
    if (!g_nativeLogPath.empty() || gameDirectory.empty()) return;
    g_nativeLogPath = gameDirectory + L"\\logEfz.txt";
    // An adopted launcher has already opened this launch's native log.
    g_nativeLogOpened = existingSession;
}

HANDLE WINAPI OpenHostLogFileW(LPCWSTR path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
{
    if (!IsNativeLogOpen(path, access, disposition))
        return CreateFileW(path, access, sharing, security, disposition, flags, templateFile);

    std::lock_guard<std::mutex> lock(g_nativeLogMutex);
    const bool truncate = !g_nativeLogOpened
        && !netplay::mod_settings::PreserveRevivalLogsAcrossLaunches();
    HANDLE file = CreateFileW(path, AppendAccess(access), sharing, security,
        truncate ? CREATE_ALWAYS : OPEN_ALWAYS, flags, templateFile);
    const DWORD openError = GetLastError();
    if (file != INVALID_HANDLE_VALUE)
    {
        g_nativeLogOpened = true;
        const unsigned long session = ++g_nativeLogSession;
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        char header[256] = {};
        const int length = std::snprintf(header, sizeof(header),
            "\r\n============================================================\r\n"
            "SESSION %lu  start=%04u-%02u-%02u %02u:%02u:%02u  pid=%lu\r\n"
            "============================================================\r\n",
            session, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
            static_cast<unsigned long>(GetCurrentProcessId()));
        DWORD written = 0;
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(header)
            || !WriteFile(file, header, static_cast<DWORD>(length), &written, nullptr)
            || written != static_cast<DWORD>(length))
            mod::Log("CAPTURE_LOG: native log session separator failed err=%lu",
                static_cast<unsigned long>(GetLastError()));
        mod::Log("CAPTURE_LOG: native logEfz session=%lu mode=%s", session,
            truncate ? "fresh_launch" : "append");
    }
    SetLastError(openError);
    return file;
}

HANDLE WINAPI OpenHostLogFileA(LPCSTR path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile)
{
    if (path != nullptr)
    {
        const char* base = path;
        for (const char* p = path; *p != '\0'; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        if (_stricmp(base, "logEfz.txt") == 0)
        {
            wchar_t wide[MAX_PATH] = {};
            if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH) > 0)
                return OpenHostLogFileW(wide, access, sharing, security, disposition, flags, templateFile);
        }
    }
    return CreateFileA(path, access, sharing, security, disposition, flags, templateFile);
}
}
