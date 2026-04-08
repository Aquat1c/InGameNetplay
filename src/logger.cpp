#include "logger.h"
#include "mod_version.h"
#include "netplay/core/mod_settings.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <share.h>
#include <string>

namespace
{
std::mutex g_logMutex;
bool g_consoleReady = false;
FILE* g_logFile = nullptr;
bool g_fileLoggingEnabled = true;
std::string g_logPath;
bool g_logFilePrimedForProcess = false;
bool g_logFileLastOpenStartedFresh = false;
bool g_logFileLastOpenPreviousExists = false;
unsigned long long g_logFileLastOpenPreviousBytes = 0;

bool IsRevivalHelperProcess()
{
    char exePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return false;
    }

    const char* baseName = exePath;
    for (const char* p = exePath; *p != '\0'; ++p)
    {
        if (*p == '\\' || *p == '/')
        {
            baseName = p + 1;
        }
    }

    return _stricmp(baseName, "efzrevival.exe") == 0;
}

unsigned long long QueryExistingFileSizeUnlocked(const std::string& path, bool* outExists)
{
    if (outExists != nullptr)
    {
        *outExists = false;
    }

    if (path.empty())
    {
        return 0;
    }

    WIN32_FILE_ATTRIBUTE_DATA attrs = {};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs))
    {
        return 0;
    }
    if ((attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return 0;
    }

    if (outExists != nullptr)
    {
        *outExists = true;
    }

    return (static_cast<unsigned long long>(attrs.nFileSizeHigh) << 32)
        | static_cast<unsigned long long>(attrs.nFileSizeLow);
}

std::string BuildLogPathFromModule(HMODULE moduleHandle)
{
    char modulePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return "efz_netplay_mod.log";
    }

    std::string path(modulePath);
    const std::size_t slashPos = path.find_last_of("\\/");
    if (slashPos == std::string::npos)
    {
        return "efz_netplay_mod.log";
    }

    path.resize(slashPos + 1);
    path += "efz_netplay_mod.log";
    return path;
}

void WriteLineUnlocked(const char* line)
{
    OutputDebugStringA(line);

    if (g_consoleReady)
    {
        fputs(line, stdout);
        fflush(stdout);
    }

    if (g_logFile != nullptr)
    {
        fputs(line, g_logFile);
        fflush(g_logFile);
    }
}

void OpenLogFileUnlocked()
{
    if (!g_fileLoggingEnabled || g_logFile != nullptr || g_logPath.empty())
    {
        return;
    }

    const bool preserveAcrossLaunches = netplay::mod_settings::PreserveModLogAcrossLaunches();
    bool previousExists = false;
    const unsigned long long previousBytes = QueryExistingFileSizeUnlocked(g_logPath, &previousExists);
    const bool startFresh =
        !g_logFilePrimedForProcess
        && !preserveAcrossLaunches
        && !IsRevivalHelperProcess();

    FILE* file = _fsopen(g_logPath.c_str(), startFresh ? "w" : "a", _SH_DENYNO);
    if (file != nullptr)
    {
        g_logFile = file;
        g_logFilePrimedForProcess = true;
        g_logFileLastOpenStartedFresh = startFresh;
        g_logFileLastOpenPreviousExists = previousExists;
        g_logFileLastOpenPreviousBytes = previousBytes;
    }
}

void CloseLogFileUnlocked()
{
    if (g_logFile == nullptr)
    {
        return;
    }

    fclose(g_logFile);
    g_logFile = nullptr;
}
}

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle, bool spawnConsole, bool writeLogFile)
{
    std::lock_guard<std::mutex> lock(g_logMutex);

    g_logPath = BuildLogPathFromModule(moduleHandle);
    g_fileLoggingEnabled = writeLogFile;

    if (!g_consoleReady && spawnConsole)
    {
        if (AllocConsole() != FALSE)
        {
            SetConsoleTitleA("In-game Netplay Logger");

            FILE* outStream = nullptr;
            FILE* errStream = nullptr;
            FILE* inStream = nullptr;
            freopen_s(&outStream, "CONOUT$", "w", stdout);
            freopen_s(&errStream, "CONOUT$", "w", stderr);
            freopen_s(&inStream, "CONIN$", "r", stdin);

            g_consoleReady = true;
        }
    }

    OpenLogFileUnlocked();

    char versionLine[256] = {};
    std::snprintf(
        versionLine,
        sizeof(versionLine),
        "[efz_netplay_mod] %s v%s build %s\n",
        netplay::build_info::kDisplayName,
        netplay::build_info::kVersion,
        netplay::build_info::kBuildTimestamp);
    WriteLineUnlocked(versionLine);
    char logModeLine[512] = {};
    std::snprintf(
        logModeLine,
        sizeof(logModeLine),
        "[efz_netplay_mod] logger file path='%s' startedFresh=%d preserveAcrossLaunches=%d previousExists=%d previousBytes=%llu\n",
        g_logPath.c_str(),
        g_logFileLastOpenStartedFresh ? 1 : 0,
        netplay::mod_settings::PreserveModLogAcrossLaunches() ? 1 : 0,
        g_logFileLastOpenPreviousExists ? 1 : 0,
        g_logFileLastOpenPreviousBytes);
    WriteLineUnlocked(logModeLine);
    WriteLineUnlocked("[efz_netplay_mod] logger initialized\n");
    return true;
}

void SetConsoleVisible(bool visible)
{
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (visible && !g_consoleReady)
    {
        if (AllocConsole() != FALSE)
        {
            SetConsoleTitleA("In-game Netplay Logger");

            FILE* outStream = nullptr;
            FILE* errStream = nullptr;
            FILE* inStream = nullptr;
            freopen_s(&outStream, "CONOUT$", "w", stdout);
            freopen_s(&errStream, "CONOUT$", "w", stderr);
            freopen_s(&inStream, "CONIN$", "r", stdin);

            g_consoleReady = true;
        }
    }
    else if (!visible && g_consoleReady)
    {
        FreeConsole();
        g_consoleReady = false;
    }
}

void SetFileLoggingEnabled(HMODULE moduleHandle, bool enabled)
{
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (g_logPath.empty() && moduleHandle != nullptr)
    {
        g_logPath = BuildLogPathFromModule(moduleHandle);
    }

    if (g_fileLoggingEnabled == enabled)
    {
        return;
    }

    g_fileLoggingEnabled = enabled;
    if (enabled)
    {
        OpenLogFileUnlocked();
    }
    else
    {
        CloseLogFileUnlocked();
    }
}

void ShutdownLogger()
{
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (g_logFile != nullptr)
    {
        fputs("[efz_netplay_mod] logger shutting down\n", g_logFile);
        fflush(g_logFile);
        CloseLogFileUnlocked();
    }

    if (g_consoleReady)
    {
        FreeConsole();
        g_consoleReady = false;
    }
}

void Log(const char* fmt, ...)
{
    // --- Lock contention + I/O timing guard ---------------------------------
    // Measure how long the whole Log() call takes (mutex acquire + format +
    // fputs + fflush).  If it exceeds 3ms, the logger itself is stalling the
    // game thread.  Uses OutputDebugStringA (lock-free) for the warning so
    // it doesn't recurse into the same mutex.
    LARGE_INTEGER logQpcStart = {};
    QueryPerformanceCounter(&logQpcStart);

    std::lock_guard<std::mutex> lock(g_logMutex);

    char message[1024];
    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (written <= 0)
    {
        return;
    }

    char line[1248];
    snprintf(
        line,
        sizeof(line),
        "[efz_netplay_mod][pid=%lu] %s\n",
        static_cast<unsigned long>(GetCurrentProcessId()),
        message);
    WriteLineUnlocked(line);

    // Check total Log() duration (including mutex wait + I/O).
    {
        static unsigned long s_logSlowCount = 0;
        LARGE_INTEGER logQpcEnd = {}, freq = {};
        QueryPerformanceCounter(&logQpcEnd);
        QueryPerformanceFrequency(&freq);
        const double elapsedMs =
            static_cast<double>(logQpcEnd.QuadPart - logQpcStart.QuadPart)
            * 1000.0 / static_cast<double>(freq.QuadPart);
        if (elapsedMs > 3.0)
        {
            ++s_logSlowCount;
            // Output via OutputDebugString to avoid re-entering the mutex.
            if (s_logSlowCount <= 10 || (s_logSlowCount % 500 == 0))
            {
                char warn[256];
                snprintf(warn, sizeof(warn),
                         "[efz_netplay_mod] PERF_WARN: Log() took %.1fms "
                         "(slowCount=%lu) — logger stalling game thread\n",
                         elapsedMs, s_logSlowCount);
                OutputDebugStringA(warn);
                // Also write it to the log file directly while we hold the lock.
                WriteLineUnlocked(warn);
            }
        }
    }
}
}
