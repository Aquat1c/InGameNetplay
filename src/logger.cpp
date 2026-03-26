#include "logger.h"
#include "mod_version.h"
#include "netplay/core/mod_settings.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
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
constexpr size_t kLogFileBufferBytes = 64 * 1024;
constexpr size_t kLogFileFlushLineInterval = 64;
constexpr DWORD kLogFileFlushIntervalMs = 1000;
char g_logFileBuffer[kLogFileBufferBytes] = {};
size_t g_pendingFileLogLines = 0;
DWORD g_lastFileFlushTick = 0;

bool IsRiskyPathForceFlushLine(const char* line)
{
    if (line == nullptr || std::strstr(line, "RISKY_PATH:") == nullptr)
    {
        return false;
    }

    return std::strstr(line, "slow=1") != nullptr
        || std::strstr(line, "stage=event") != nullptr
        || std::strstr(line, "longjmp_") != nullptr
        || std::strstr(line, "failed") != nullptr
        || std::strstr(line, "invalid_") != nullptr
        || std::strstr(line, "missing") != nullptr
        || std::strstr(line, "unavailable") != nullptr
        || std::strstr(line, "skip_no_") != nullptr;
}

bool ShouldForceFlushLogLine(const char* line)
{
    if (line == nullptr)
    {
        return false;
    }

    return std::strstr(line, "CrashHandler:") != nullptr
        || std::strstr(line, " console error detected") != nullptr
        || std::strstr(line, " DISCONNECT ") != nullptr
        || std::strstr(line, " GRACEFUL SESSION END ") != nullptr
        || IsRiskyPathForceFlushLine(line)
        || std::strstr(line, " STALL detected") != nullptr
        || std::strstr(line, " timed out") != nullptr
        || std::strstr(line, " failed") != nullptr
        || std::strstr(line, " FAIL") != nullptr
        || std::strstr(line, " SEH exception") != nullptr;
}

void FlushLogFileUnlocked(bool force)
{
    if (g_logFile == nullptr)
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (!force)
    {
        const bool intervalElapsed =
            g_lastFileFlushTick == 0
            || (now - g_lastFileFlushTick) >= kLogFileFlushIntervalMs;
        if (g_pendingFileLogLines < kLogFileFlushLineInterval && !intervalElapsed)
        {
            return;
        }
    }

    fflush(g_logFile);
    g_pendingFileLogLines = 0;
    g_lastFileFlushTick = now;
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
        ++g_pendingFileLogLines;
        FlushLogFileUnlocked(ShouldForceFlushLogLine(line));
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
    const bool firstOpenThisProcess = !g_logFilePrimedForProcess;
    const bool startFresh = firstOpenThisProcess && !preserveAcrossLaunches;
    const char* openMode = startFresh ? "w" : "a";

    FILE* file = _fsopen(g_logPath.c_str(), openMode, _SH_DENYNO);
    if (file != nullptr)
    {
        g_logFile = file;
        g_logFilePrimedForProcess = true;
        g_logFileLastOpenStartedFresh = startFresh;
        g_logFileLastOpenPreviousExists = previousExists;
        g_logFileLastOpenPreviousBytes = previousBytes;
        (void)setvbuf(g_logFile, g_logFileBuffer, _IOFBF, sizeof(g_logFileBuffer));
        g_pendingFileLogLines = 0;
        g_lastFileFlushTick = GetTickCount();
    }
}

void CloseLogFileUnlocked()
{
    if (g_logFile == nullptr)
    {
        return;
    }

    FlushLogFileUnlocked(true);
    fclose(g_logFile);
    g_logFile = nullptr;
    g_pendingFileLogLines = 0;
    g_lastFileFlushTick = 0;
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
    FlushLogFileUnlocked(true);
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
}
}
