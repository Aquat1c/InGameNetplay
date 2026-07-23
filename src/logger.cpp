#include "logger.h"
#include "mod_version.h"
#include "netplay/core/mod_settings.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <share.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
// ---------------------------------------------------------------------------
// Async logger
// ---------------------------------------------------------------------------
// Log() used to do fputs + fflush under a shared mutex on the caller's
// thread.  With the poll thread and bridge worker all logging through the
// same mutex, the game thread could stall waiting on a background-thread
// disk write.  The logger now formats on the caller's stack and hands the
// finished line off to a dedicated writer thread via a bounded queue.
// Callers hold g_queueMutex only long enough to push one string.

// Queue side: guards the ring of pending lines, stop signal, CV.
std::mutex g_queueMutex;
std::condition_variable g_queueCv;
std::deque<std::string> g_queue;
std::atomic<bool> g_writerShouldStop{false};
std::thread g_writerThread;
std::atomic<uint64_t> g_droppedLines{0};
constexpr std::size_t kNormalMaxQueuedLines = 4096;
constexpr std::size_t kRevival102jDiagnosticMaxQueuedLines = 32768;

// File/console side: guards the FILE* and console-attachment state.
// The writer thread locks this to do fputs/fflush; SetFileLoggingEnabled /
// SetConsoleVisible / FlushLoggerSync also lock it.  Caller-side Log() never
// touches this mutex.
std::mutex g_fileMutex;
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
    path += "logs\\efz_netplay_mod.log";
    return path;
}

// Caller must hold g_fileMutex.  Writes each line followed by a single
// fflush at the end so the OS only issues one disk sync per batch.
void WriteBatchLocked(const std::vector<std::string>& batch)
{
    if (batch.empty())
    {
        return;
    }

    if (g_consoleReady)
    {
        for (const std::string& line : batch)
        {
            fputs(line.c_str(), stdout);
        }
        fflush(stdout);
    }

    if (g_logFile != nullptr)
    {
        for (const std::string& line : batch)
        {
            fputs(line.c_str(), g_logFile);
        }
        fflush(g_logFile);
    }
}

void OpenLogFileUnlocked()
{
    if (!g_fileLoggingEnabled || g_logFile != nullptr || g_logPath.empty())
    {
        return;
    }

    // The log lives in <mod>\logs\; create the folder lazily so it only
    // exists when file logging is actually enabled.
    const std::size_t dirEnd = g_logPath.find_last_of("\\/");
    if (dirEnd != std::string::npos)
    {
        (void)CreateDirectoryA(g_logPath.substr(0, dirEnd).c_str(), nullptr);
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

void WriterThreadEntry()
{
    std::vector<std::string> batch;
    batch.reserve(64);

    while (true)
    {
        {
            // Swap the whole deque out under the lock (O(1)) instead of an
            // element-wise move+pop (O(queue length)) - producers, including
            // the game thread's Log(), must never block behind a long drain
            // (2026-07-20 desync-surface audit hygiene finding).
            std::deque<std::string> pending;
            {
                std::unique_lock<std::mutex> lock(g_queueMutex);
                g_queueCv.wait(lock, []() {
                    return !g_queue.empty() || g_writerShouldStop.load();
                });
                pending.swap(g_queue);
            }
            for (std::string& line : pending)
            {
                batch.push_back(std::move(line));
            }
        }

        if (!batch.empty())
        {
            std::lock_guard<std::mutex> lock(g_fileMutex);
            WriteBatchLocked(batch);
            batch.clear();
        }

        if (g_writerShouldStop.load())
        {
            // Drain one final time in case producers enqueued after our
            // last wake-up but before they observed the stop flag.
            {
                std::deque<std::string> pending;
                {
                    std::lock_guard<std::mutex> lock(g_queueMutex);
                    pending.swap(g_queue);
                }
                for (std::string& line : pending)
                {
                    batch.push_back(std::move(line));
                }
            }
            if (!batch.empty())
            {
                std::lock_guard<std::mutex> lock(g_fileMutex);
                WriteBatchLocked(batch);
                batch.clear();
            }
            return;
        }
    }
}

void StartWriterThreadIfNeeded()
{
    if (g_writerThread.joinable())
    {
        return;
    }
    g_writerShouldStop.store(false);
    g_writerThread = std::thread(WriterThreadEntry);
}

// Push a fully-formatted line (already ending with '\n') into the queue.
// Game-thread callers spend microseconds here: one mutex acquire, one
// deque push, one CV notify.  When the queue is full we drop the oldest
// line so recent context is preserved at the cost of losing ancient log
// history that hasn't been flushed yet.
void EnqueueLine(std::string line)
{
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        const std::size_t maxQueuedLines =
            netplay::mod_settings::IsVerboseRevival102jLifecycleLoggingEnabled()
                ? kRevival102jDiagnosticMaxQueuedLines
                : kNormalMaxQueuedLines;
        if (g_queue.size() >= maxQueuedLines)
        {
            g_queue.pop_front();
            g_droppedLines.fetch_add(1, std::memory_order_relaxed);
        }
        g_queue.push_back(std::move(line));
    }
    g_queueCv.notify_one();
}
}

namespace mod
{
bool InitializeLogger(HMODULE moduleHandle, bool spawnConsole, bool writeLogFile)
{
    {
        std::lock_guard<std::mutex> lock(g_fileMutex);

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
    }

    StartWriterThreadIfNeeded();

    char versionLine[256] = {};
    std::snprintf(
        versionLine,
        sizeof(versionLine),
        "[efz_netplay_mod] %s v%s build %s\n",
        netplay::build_info::kDisplayName,
        netplay::build_info::kVersion,
        netplay::build_info::kBuildTimestamp);
    OutputDebugStringA(versionLine);
    EnqueueLine(versionLine);

    char logModeLine[512] = {};
    {
        std::lock_guard<std::mutex> lock(g_fileMutex);
        std::snprintf(
            logModeLine,
            sizeof(logModeLine),
            "[efz_netplay_mod] logger file path='%s' startedFresh=%d preserveAcrossLaunches=%d previousExists=%d previousBytes=%llu\n",
            g_logPath.c_str(),
            g_logFileLastOpenStartedFresh ? 1 : 0,
            netplay::mod_settings::PreserveModLogAcrossLaunches() ? 1 : 0,
            g_logFileLastOpenPreviousExists ? 1 : 0,
            g_logFileLastOpenPreviousBytes);
    }
    OutputDebugStringA(logModeLine);
    EnqueueLine(logModeLine);

    const char* kInitLine = "[efz_netplay_mod] logger initialized (async writer)\n";
    OutputDebugStringA(kInitLine);
    EnqueueLine(kInitLine);
    return true;
}

void SetConsoleVisible(bool visible)
{
    std::lock_guard<std::mutex> lock(g_fileMutex);

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
    std::lock_guard<std::mutex> lock(g_fileMutex);

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

void FlushLoggerSync()
{
    // Snapshot the pending queue and write it synchronously.  Taking
    // g_fileMutex after releasing g_queueMutex means any in-flight batch
    // the writer thread is currently draining serializes in front of
    // ours: writer grabbed g_fileMutex first, so we wait; the writer then
    // releases and we write a strictly-newer batch.  Ordering is
    // preserved in the output file.
    std::vector<std::string> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        while (!g_queue.empty())
        {
            snapshot.push_back(std::move(g_queue.front()));
            g_queue.pop_front();
        }
    }

    std::lock_guard<std::mutex> lock(g_fileMutex);
    WriteBatchLocked(snapshot);
}

void ShutdownLogger()
{
    // Mark the queue closed, wake the writer, and wait for it to drain.
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_writerShouldStop.store(true);
    }
    g_queueCv.notify_all();
    if (g_writerThread.joinable())
    {
        g_writerThread.join();
    }

    std::lock_guard<std::mutex> lock(g_fileMutex);

    if (g_logFile != nullptr)
    {
        const uint64_t dropped = g_droppedLines.load(std::memory_order_relaxed);
        if (dropped != 0)
        {
            char warn[128] = {};
            std::snprintf(
                warn,
                sizeof(warn),
                "[efz_netplay_mod] logger dropped %llu line(s) due to queue overflow\n",
                static_cast<unsigned long long>(dropped));
            fputs(warn, g_logFile);
        }
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
    // Format on the caller's stack - no mutex held during vsnprintf.
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
    const int lineLen = std::snprintf(
        line,
        sizeof(line),
        "[efz_netplay_mod][pid=%lu] %s\n",
        static_cast<unsigned long>(GetCurrentProcessId()),
        message);
    if (lineLen <= 0)
    {
        return;
    }

    // Debugger delivery is intentionally absent from shipping builds.
    // OutputDebugString can synchronously rendezvous with a debugger/DBWIN
    // consumer, so it does not belong on a rollback-thread call path.
#if defined(EFZ_LIFECYCLE_TRACE)
    OutputDebugStringA(line);
#endif

    EnqueueLine(std::string(line, static_cast<std::size_t>(lineLen)));
}

#if defined(EFZ_LIFECYCLE_TRACE)
namespace
{
std::atomic<bool> g_lifecycleTraceEnabled{false};
}

bool IsLifecycleTraceEnabled()
{
    return g_lifecycleTraceEnabled.load(std::memory_order_relaxed);
}

void SetLifecycleTraceEnabled(bool enabled)
{
    g_lifecycleTraceEnabled.store(enabled, std::memory_order_relaxed);
}
#endif
}
