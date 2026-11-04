#include "../src/netplay/bridge/console_capture.cpp"

#include <iostream>
#include <future>

namespace
{
int g_failures = 0;
int g_publications = 0;
bool g_metricsPublishedFirst = true;
LONG g_publishedWakeSnapshot = -1;
bool g_testWorkerReadBoundary = false;
bool g_workerRejectedReadBoundary = false;
std::string g_consoleError;
std::string g_quitEndpoint;
bool g_quitIsActivePeer = true;

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}
}

namespace mod
{
void Log(const char*, ...) {}
}

namespace netplay::network
{
const char* FamilyName(NetworkFamily) { return "test"; }
}

namespace netplay::mod_settings
{
bool PreserveRevivalLogsAcrossLaunches() { return false; }
}

namespace netplay::bridge::takeover
{
HANDLE g_revivalProcess = nullptr;
SharedBlock* g_hostBlock = nullptr;
SharedBlock* g_injectedBlock = nullptr;
volatile LONG g_injectedDelayPromptSerial = 0;
volatile LONG g_injectedSpectateConfirmPromptSerial = 0;
DWORD g_injectedSpectateConfirmPromptWaitStartTick = 0;
std::mutex g_consoleLogMutex;
std::string g_consolePendingWriteFile;
std::string g_consolePendingWriteFileDisk;
std::string g_consolePendingWriteConsoleA;
std::string g_consolePendingWriteConsoleW;
std::string g_consolePendingWriteConsoleOutputCharacterA;
std::string g_consolePendingWriteConsoleOutputCharacterW;
std::string g_consolePendingOutputDebugStringA;
std::string g_consolePendingOutputDebugStringW;
DelayPromptMetrics g_delayPromptMetrics;
bool g_nativeWorkflowLoadedSeen = false;
bool g_nativeWorkflowMatchLoopSeen = false;
bool g_nativeWorkflowTournamentSeen = false;
bool g_nativeWorkflowPeerDiedSeen = false;
bool g_nativeWorkflowHolePunchDiedSeen = false;
bool g_holePunchServerConfigLoaded = false;
std::string g_configuredHolePunchServer;

bool IsCurrentProcessRevival() { return true; }
bool TryClassifyRevivalQuitEndpoint(const char* endpoint, bool* active, bool* spectator)
{
    g_quitEndpoint = endpoint;
    *active = g_quitIsActivePeer;
    *spectator = !g_quitIsActivePeer;
    return true;
}
bool TryPublishHeldHostDelayTimeout(LONG) { return false; }
void PublishConsoleError(const char* error)
{
    g_consoleError = error;
    InterlockedIncrement(&g_injectedBlock->consoleErrorSerial);
}
void PublishSpectateConfirmPromptSerial(LONG, int) {}
void PublishHostListenerObservation(network::NetworkFamily, uint16_t) {}
void PublishDelayPromptSerial(LONG serial, LONG wakeSnapshot)
{
    g_publishedWakeSnapshot = wakeSnapshot;
    if (g_testWorkerReadBoundary)
    {
        g_workerRejectedReadBoundary = !FlushPendingConsoleOutput("worker_read", true);
    }
    ++g_publications;
    g_metricsPublishedFirst = g_metricsPublishedFirst
        && g_injectedBlock->delayMetricsSerial == serial
        && g_delayPromptMetrics.serial == serial;
    g_injectedBlock->delayPromptSerial = serial;
}
}

namespace
{
using namespace netplay::bridge;
using namespace netplay::bridge::takeover;

void FeedNativeCharacters(const char* text)
{
    while (*text != '\0')
    {
        ProcessConsoleTextChunk("WriteConsoleOutputCharacterW", text++, 1, -1);
    }
}

void ResetCapture(SharedBlock& block)
{
    block = {};
    g_injectedBlock = &block;
    g_injectedDelayPromptSerial = 0;
    g_delayPromptMetrics = {};
    g_publications = 0;
    g_consoleError.clear();
    g_quitEndpoint.clear();
    g_quitIsActivePeer = true;
    g_metricsPublishedFirst = true;
    g_publishedWakeSnapshot = -1;
    DiscardPendingConsoleFragmentsLocked();
    ResetNativeWorkflowFlags();
}

void TestNativePromptAndRetry()
{
    SharedBlock block;
    ResetCapture(block);
    FeedNativeCharacters(
        "The input delay can always be adjusted during the actual match.\n"
        "Average Ping: 213\nMin Ping: 200\nMax Ping: 220\n"
        "Recommended input delay: 4\n"
        "Enter the initial input delay (between ) 2 and 20, none for recommended): ");
    Expect(g_publications == 0, "initial prompt waits for native input-read flush");
    FlushPendingConsoleOutputLocked();
    Expect(g_publications == 1, "native input-read flush publishes initial prompt once");
    Expect(g_metricsPublishedFirst, "metrics are published before prompt serial");
    Expect(block.delayRangeMin == 2 && block.delayRangeMax == 20,
        "one-character native output retains complete inclusive range");
    Expect(block.delayAveragePingMs == 213 && block.delayMinPingMs == 200
        && block.delayMaxPingMs == 220 && block.delayRecommended == 4,
        "native measurements reach the shared prompt");

    FeedNativeCharacters("\nDelay must be between 3 and 20\nEnter the input delay to use: ");
    Expect(g_publications == 1, "retry prompt waits for native input-read flush");
    FlushPendingConsoleOutputLocked();
    Expect(g_publications == 2 && block.delayPromptSerial == 2,
        "rejected input publishes a fresh retry serial");
    Expect(block.delayRangeMin == 3 && block.delayRangeMax == 20,
        "retry uses native rejection range");
    Expect(block.delayAveragePingMs == 213 && block.delayMinPingMs == 200
        && block.delayMaxPingMs == 220 && block.delayRecommended == 4,
        "retry preserves measured ping and recommendation");
    Expect(g_metricsPublishedFirst, "retry metrics precede retry prompt serial");
    FlushPendingConsoleOutputLocked();
    Expect(g_publications == 2, "empty read flush does not duplicate prompt");
}

void TestQueuedReadBoundary()
{
    SharedBlock block;
    ResetCapture(block);
    block.magic = kIpcMagic;
    block.version = kIpcVersion;
    block.hostPid = GetCurrentProcessId();
    g_testWorkerReadBoundary = true;
    Expect(StartConsoleCaptureWorker(true), "console parser worker starts");
    const char* prompts[] = {
        "Average Ping: 213\nMin Ping: 200\nMax Ping: 220\n"
        "Recommended input delay: 4\n"
        "Enter the initial input delay (between ) 2 and 20, none for recommended): ",
        "\nDelay must be between 2 and 20\nEnter the input delay to use: ",
    };
    for (LONG attempt = 0; attempt < 2; ++attempt)
    {
        std::unique_lock<std::mutex> parserLock(g_consoleLogMutex);
        block.delayPromptServedSerial = attempt;
        const LONG rawWakeSnapshot = attempt * 3;
        for (const char* text = prompts[attempt]; *text != '\0'; ++text)
        {
            Expect(EnqueueConsoleParseChunk(
                "WriteConsoleOutputCharacterW", text, 1, false, rawWakeSnapshot),
                "native character enters the production parser queue");
        }
        InterlockedExchange(&block.consoleControlWakeRequestSerial, rawWakeSnapshot + 3);
        auto readBoundary = std::async(std::launch::async, [] {
            return FlushPendingConsoleOutput("before_ReadConsoleW", true);
        });
        Expect(readBoundary.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready,
            "initial and retry readers wait for queued prompt parsing before classification");
        parserLock.unlock();
        Expect(readBoundary.get(), "completed input-read barrier succeeds");
        Expect(block.delayPromptSerial == attempt + 1 && block.delayMetricsSerial == attempt + 1
            && block.delayRangeMin == 2 && block.delayRangeMax == 20,
            "input-read boundary returns only after complete prompt publication");
        Expect(g_publishedWakeSnapshot == rawWakeSnapshot,
            "deferred prompt keeps raw-write wake generation despite later wake requests");
        Expect(g_workerRejectedReadBoundary, "worker cannot wait on its own parser queue");
    }
    {
        std::lock_guard<std::mutex> queueLock(g_ownedLogEfzQueueMutex);
        g_ownedLogEfzAccepting = false;
    }
    Expect(!FlushPendingConsoleOutput("reader_enqueue_rejected", true),
        "active-worker enqueue failure cannot report a completed input-read boundary");
    {
        std::lock_guard<std::mutex> queueLock(g_ownedLogEfzQueueMutex);
        g_ownedLogEfzAccepting = true;
    }
    std::unique_lock<std::mutex> parserLock(g_consoleLogMutex);
    Expect(EnqueueConsoleParseChunk("WriteConsoleOutputCharacterW", "x", 1, false, 6),
        "shutdown fixture queues pending native text");
    auto interruptedRead = std::async(std::launch::async, [] {
        return FlushPendingConsoleOutput("reader_interrupted", true);
    });
    Expect(interruptedRead.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready,
        "read barrier remains pending while parser is blocked");
    InterlockedExchange(&g_ownedLogEfzEmergencyStop, 1);
    g_ownedLogEfzDrainCv.notify_all();
    g_ownedLogEfzQueueCv.notify_all();
    Expect(!interruptedRead.get(), "emergency shutdown releases read barrier as incomplete");
    parserLock.unlock();
    StopManagedLogEfzWorker(true);
    InterlockedExchange(&g_ownedLogEfzEmergencyStop, 0);
    g_testWorkerReadBoundary = false;
}

void TestFragmentedPeerQuit()
{
    SharedBlock terminal{};
    Expect(!IsDelayPromptTerminated(&terminal), "fresh delay state accepts input");
    terminal.nativeDelayTimeoutSerial = 1;
    Expect(IsDelayPromptTerminated(&terminal), "native timeout blocks input before cleanup completes");
    terminal.nativeDelayTimeoutHandledSerial = 1;
    terminal.consoleErrorSerial = 1;
    Expect(IsDelayPromptTerminated(&terminal), "terminal error keeps delay input blocked after cleanup");

    SharedBlock block;
    ResetCapture(block);
    FeedNativeCharacters("Recv quit from: 127.0.0.1:10800\n");
    Expect(g_quitEndpoint == "127.0.0.1:10800", "fragmented quit retains full peer endpoint");
    Expect(!g_consoleError.empty(), "active peer quit publishes terminal error");
    FeedNativeCharacters("Enter the initial input delay (between 2 and 20): ");
    FlushPendingConsoleOutputLocked();
    Expect(g_publications == 0, "late delay prompt cannot revive a terminal session");

    ResetCapture(block);
    g_quitIsActivePeer = false;
    FeedNativeCharacters("Received quit from: [::1]:10800\n");
    Expect(g_quitEndpoint == "[::1]:10800", "fragmented IPv6 quit retains complete endpoint");
    Expect(g_consoleError.empty(), "spectator quit does not terminate player delay setup");

    ResetCapture(block);
    FeedNativeCharacters("Remote timed out\n");
    Expect(!g_consoleError.empty(), "native timeout publishes terminal error");
    FeedNativeCharacters("Enter the input delay to use: ");
    FlushPendingConsoleOutputLocked();
    Expect(g_publications == 0, "retry cannot revive a timed-out session");
}

void TestUnknownRangeAndSessionReset()
{
    bool hasMetrics = true;
    const auto missing = ParseDelayPromptMetricsFromText("Enter the input delay to use:", &hasMetrics);
    Expect(missing.minDelay == -1 && missing.maxDelay == -1 && !hasMetrics,
        "absent native range stays unknown");

    SharedBlock block;
    ResetCapture(block);
    FeedNativeCharacters("Delay must be between 5 and 20\nEnter the input delay to use: ");
    Expect(FlushPendingConsoleOutput("reader_without_worker", true),
        "input-read boundary parses synchronously when no worker is active");
    Expect(block.delayRangeMin == 5 && block.delayRangeMax == 20,
        "retry range can be parsed without earlier measurements");
    Expect(block.delayAveragePingMs == -1 && block.delayMinPingMs == -1
        && block.delayMaxPingMs == -1 && block.delayRecommended == -1,
        "session reset does not retain previous ping or recommendation");
}
}

int main()
{
    TestNativePromptAndRetry();
    TestQueuedReadBoundary();
    TestFragmentedPeerQuit();
    TestUnknownRangeAndSessionReset();
    if (g_failures == 0)
    {
        std::cout << "Console delay prompt tests passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
