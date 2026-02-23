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
}

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle, bool spawnConsole)
{
    std::lock_guard<std::mutex> lock(g_logMutex);

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

    if (g_logFile == nullptr)
    {
        const std::string logPath = BuildLogPathFromModule(moduleHandle);
        FILE* file = _fsopen(logPath.c_str(), "a", _SH_DENYNO);
        if (file != nullptr)
        {
            g_logFile = file;
        }
    }

    WriteLineUnlocked("[efz_netplay_mod] logger initialized\n");
    return true;
}

void ShutdownLogger()
{
    std::lock_guard<std::mutex> lock(g_logMutex);

    if (g_logFile != nullptr)
    {
        fputs("[efz_netplay_mod] logger shutting down\n", g_logFile);
        fflush(g_logFile);
        fclose(g_logFile);
        g_logFile = nullptr;
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

    char line[1200];
    snprintf(line, sizeof(line), "[efz_netplay_mod] %s\n", message);
    WriteLineUnlocked(line);
}
}

