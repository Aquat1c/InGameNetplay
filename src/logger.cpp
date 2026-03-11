#include "logger.h"

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

    FILE* file = _fsopen(g_logPath.c_str(), "a", _SH_DENYNO);
    if (file != nullptr)
    {
        g_logFile = file;
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
            SetConsoleTitleA("EFZ Netplay Mod Logger");

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
            SetConsoleTitleA("EFZ Netplay Mod Logger");

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
