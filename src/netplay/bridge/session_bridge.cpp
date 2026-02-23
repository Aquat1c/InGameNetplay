#include "netplay/bridge/session_bridge.h"

#include "netplay/bridge/revival_takeover.h"
#include "logger.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace netplay::bridge
{
namespace
{
std::mutex g_mutex;
NetbridgeStatus g_status = {};
uint32_t g_connectStartTick = 0;
bool g_initialized = false;

std::thread g_startWorker;
bool g_startWorkerRunning = false;
uint32_t g_startRequestSerial = 0;

void SetPhase(NetbridgePhase phase, const char* error)
{
    g_status.phase = static_cast<int>(phase);
    g_status.phaseTick = GetTickCount();
    if (error != nullptr)
    {
#if defined(_MSC_VER)
        strncpy_s(g_status.errorMsg, sizeof(g_status.errorMsg), error, _TRUNCATE);
#else
        std::snprintf(g_status.errorMsg, sizeof(g_status.errorMsg), "%s", error);
#endif
    }
    else if (phase != NetbridgePhase::Failed)
    {
        g_status.errorMsg[0] = '\0';
    }
}

void JoinFinishedWorkerUnlocked()
{
    if (g_startWorker.joinable() && !g_startWorkerRunning)
    {
        g_startWorker.join();
    }
}

void InitializeHostUnlocked(const char* reason)
{
    g_status = {};
    SetPhase(NetbridgePhase::Idle, nullptr);
    takeover::InitializeHost();
    takeover::Tick(&g_status, &g_connectStartTick);
    g_initialized = true;
    mod::Log("SessionBridge: initialized (role=host, reason=%s)", reason != nullptr ? reason : "unknown");
}
} // namespace

bool IsCurrentProcessRevival()
{
    return takeover::IsCurrentProcessRevival();
}

void Initialize()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized)
    {
        return;
    }

    InitializeHostUnlocked("startup");
}

void Shutdown()
{
    std::thread workerToJoin;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_initialized)
        {
            return;
        }

        ++g_startRequestSerial;
        if (g_startWorkerRunning)
        {
            takeover::RequestAbortStart();
            if (g_startWorker.joinable())
            {
                workerToJoin = std::move(g_startWorker);
            }
            g_startWorkerRunning = false;
        }
        else
        {
            JoinFinishedWorkerUnlocked();
        }
    }

    if (workerToJoin.joinable())
    {
        workerToJoin.join();
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        takeover::CancelSession("shutdown", &g_status);
        takeover::ShutdownHost();
        g_status = {};
        SetPhase(NetbridgePhase::Idle, nullptr);
        g_connectStartTick = 0;
        g_initialized = false;
    }

    mod::Log("SessionBridge: shutdown (role=host)");
}

void InitializeInjectedProcess()
{
    takeover::InitializeInjected();
}

void ShutdownInjectedProcess()
{
    takeover::ShutdownInjected();
}

void Tick()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return;
    }

    // Keep the main thread non-blocking while start worker owns takeover startup.
    if (g_startWorkerRunning)
    {
        return;
    }

    JoinFinishedWorkerUnlocked();
    takeover::Tick(&g_status, &g_connectStartTick);
}

bool StartSession(NetbridgeRole role, uint16_t port, const char* address, const char* nickname)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        InitializeHostUnlocked("on_demand");
    }

    JoinFinishedWorkerUnlocked();
    if (g_startWorkerRunning)
    {
        SetPhase(NetbridgePhase::Failed, "session start already in progress");
        mod::Log("SessionBridge: StartSession rejected (worker already running)");
        return false;
    }

    const std::string addressCopy = (address != nullptr) ? address : "";
    const std::string nicknameCopy = (nickname != nullptr) ? nickname : "";

    g_status.role = static_cast<int>(role);
    g_status.port = port;
    mod::Log(
        "SessionBridge: StartSession role=%d port=%u address='%s' nickname='%s'",
        static_cast<int>(role),
        static_cast<unsigned>(port),
        addressCopy.c_str(),
        nicknameCopy.c_str());
#if defined(_MSC_VER)
    strncpy_s(g_status.address, sizeof(g_status.address), addressCopy.c_str(), _TRUNCATE);
    strncpy_s(g_status.nickname, sizeof(g_status.nickname), nicknameCopy.c_str(), _TRUNCATE);
#else
    std::snprintf(g_status.address, sizeof(g_status.address), "%s", addressCopy.c_str());
    std::snprintf(g_status.nickname, sizeof(g_status.nickname), "%s", nicknameCopy.c_str());
#endif

    SetPhase(NetbridgePhase::Connecting, nullptr);
    g_connectStartTick = GetTickCount();

    const uint32_t requestSerial = ++g_startRequestSerial;
    g_startWorkerRunning = true;

    g_startWorker = std::thread([requestSerial, role, port, addressCopy, nicknameCopy]() {
        NetbridgeStatus workerStatus = {};
        workerStatus.role = static_cast<int>(role);
        workerStatus.port = port;
#if defined(_MSC_VER)
        strncpy_s(workerStatus.address, sizeof(workerStatus.address), addressCopy.c_str(), _TRUNCATE);
        strncpy_s(workerStatus.nickname, sizeof(workerStatus.nickname), nicknameCopy.c_str(), _TRUNCATE);
#else
        std::snprintf(workerStatus.address, sizeof(workerStatus.address), "%s", addressCopy.c_str());
        std::snprintf(workerStatus.nickname, sizeof(workerStatus.nickname), "%s", nicknameCopy.c_str());
#endif

        uint32_t workerConnectStartTick = GetTickCount();
        const bool started = takeover::StartSession(
            role,
            port,
            addressCopy.c_str(),
            nicknameCopy.c_str(),
            &workerStatus,
            &workerConnectStartTick);

        bool cancelStaleSession = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (requestSerial == g_startRequestSerial)
            {
                g_status = workerStatus;
                g_connectStartTick = workerConnectStartTick;
                if (!started)
                {
                    mod::Log(
                        "SessionBridge: StartSession failed phase=%s error='%s'",
                        PhaseToString(static_cast<NetbridgePhase>(g_status.phase)),
                        g_status.errorMsg);
                }
            }
            else
            {
                mod::Log(
                    "SessionBridge: dropped stale start result serial=%u current=%u",
                    static_cast<unsigned>(requestSerial),
                    static_cast<unsigned>(g_startRequestSerial));
                cancelStaleSession = started;
            }

            g_startWorkerRunning = false;
        }

        if (cancelStaleSession)
        {
            takeover::CancelSession("stale_worker_result", nullptr);
        }
    });

    return true;
}

void CancelSession(const char* reason)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        return;
    }

    ++g_startRequestSerial;
    mod::Log("SessionBridge: CancelSession reason='%s'", (reason != nullptr) ? reason : "");

    if (g_startWorkerRunning)
    {
        takeover::RequestAbortStart();
        if (reason != nullptr &&
            (std::strcmp(reason, "user_cancel") == 0 ||
             std::strcmp(reason, "leave_menu") == 0 ||
             std::strcmp(reason, "external_cancel") == 0))
        {
            SetPhase(NetbridgePhase::Idle, nullptr);
        }
        else
        {
            SetPhase(NetbridgePhase::Failed, "start canceled");
        }
        return;
    }

    JoinFinishedWorkerUnlocked();
    takeover::CancelSession(reason, &g_status);
}

void OnTitleSelectionConfirmed(int selection)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized)
    {
        InitializeHostUnlocked("title_confirm");
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(g_status.phase);
    if (phase == NetbridgePhase::Connecting || phase == NetbridgePhase::Connected)
    {
        return;
    }

    takeover::OnTitleSelectionConfirmed(selection, &g_status);
}

NetbridgeStatus GetStatus()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

const char* PhaseToString(NetbridgePhase phase)
{
    switch (phase)
    {
    case NetbridgePhase::Idle:
        return "Idle";
    case NetbridgePhase::Connecting:
        return "Connecting";
    case NetbridgePhase::Connected:
        return "Connected";
    case NetbridgePhase::Failed:
        return "Failed";
    case NetbridgePhase::SessionEnded:
        return "SessionEnded";
    default:
        return "Unknown";
    }
}

void BuildStatusLine(const NetbridgeStatus& status, char* buffer, size_t bufferSize)
{
    if (buffer == nullptr || bufferSize == 0)
    {
        return;
    }

    const NetbridgePhase phase = static_cast<NetbridgePhase>(status.phase);
    switch (phase)
    {
    case NetbridgePhase::Idle:
        std::snprintf(
            buffer,
            bufferSize,
            "Nick:%s Host:%u Join:%s:%u Role:%d",
            status.nickname[0] != '\0' ? status.nickname : "Player",
            static_cast<unsigned>(status.port),
            status.address,
            static_cast<unsigned>(status.port),
            status.roleFlag);
        break;
    case NetbridgePhase::Connecting:
        std::snprintf(
            buffer,
            bufferSize,
            "Connecting... pid=%lu ESC/BACK=Cancel",
            static_cast<unsigned long>(status.processId));
        break;
    case NetbridgePhase::Connected:
    {
        char pingText[32] = "?";
        char delayText[32] = "?";
        char namesText[140] = {};
        if (status.pingMs >= 0)
        {
            std::snprintf(pingText, sizeof(pingText), "%dms", status.pingMs);
        }
        if (status.rollbackFrames >= 0)
        {
            std::snprintf(delayText, sizeof(delayText), "%df", status.rollbackFrames);
        }
        if (status.p1Name[0] != '\0' || status.p2Name[0] != '\0')
        {
            std::snprintf(namesText, sizeof(namesText), " %s vs %s", status.p1Name, status.p2Name);
        }
        std::snprintf(
            buffer,
            bufferSize,
            "Connected pid=%lu Ping:%s Delay:%s Role:%d%s",
            static_cast<unsigned long>(status.processId),
            pingText,
            delayText,
            status.roleFlag,
            namesText);
        break;
    }
    case NetbridgePhase::Failed:
        std::snprintf(buffer, bufferSize, "Connect failed: %s", status.errorMsg);
        break;
    case NetbridgePhase::SessionEnded:
        std::snprintf(buffer, bufferSize, "Session ended");
        break;
    default:
        std::snprintf(buffer, bufferSize, "Status unknown");
        break;
    }
}
} // namespace netplay::bridge


