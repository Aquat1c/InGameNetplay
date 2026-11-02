#include "netplay/core/connection_check.h"

#include <windows.h>

#include <memory>
#include <string>

#include "logger.h"
#include "netplay/core/mod_settings.h"
#include "netplay/core/tls_http_client.h"

namespace netplay::connection_check
{
namespace
{
// Overridden by [Others] ConnectionCheckUrl.
constexpr const char kDefaultBaseUrl[] =
    "https://efz.16-192-153-4.sslip.io/api/connection-check/v1";
constexpr DWORD kConnectTimeoutMs = 6000;
constexpr DWORD kReceiveTimeoutMs = 15000;
// Revival registers with the relay and maps its port with UPnP after the listener starts.
constexpr DWORD kSettleDelayMs = 5000;
// Above the server's per-address limit.
constexpr DWORD kRetryDelayMs = 20000;

struct WorkerArgs
{
    LONG generation;
    uint16_t port;
    std::string baseUrl;
};

volatile LONG g_generation = 0;
volatile LONG g_port = 0;
volatile LONG g_result = static_cast<LONG>(Result::None);

bool IsCurrent(LONG generation)
{
    return InterlockedCompareExchange(&g_generation, 0, 0) == generation;
}

bool RunOnce(const std::string& baseUrl, uint16_t port, bool* outDirect, bool* outRelay)
{
    std::string body;
    std::string error;
    const std::string url = baseUrl + "/run?port=" + std::to_string(port);
    if (!netplay::tls::HttpGet(url, true, kConnectTimeoutMs, kReceiveTimeoutMs, &body, &error))
    {
        mod::Log("ConnectionCheck: run failed (%s)", error.c_str());
        return false;
    }
    if (!ParseRunResult(body, outDirect, outRelay))
    {
        mod::Log("ConnectionCheck: no result (%u bytes)", static_cast<unsigned>(body.size()));
        return false;
    }
    return true;
}

DWORD WINAPI WorkerThreadProc(LPVOID param)
{
    const std::unique_ptr<WorkerArgs> args(static_cast<WorkerArgs*>(param));
    const LONG generation = args->generation;
    const uint16_t port = args->port;
    Sleep(kSettleDelayMs);

    if (!IsCurrent(generation))
    {
        return 0;
    }
    std::string body;
    std::string error;
    if (!netplay::tls::HttpGet(
            args->baseUrl + "/status", true,
            kConnectTimeoutMs, kReceiveTimeoutMs, &body, &error)
        || body.find("\"ready\"") == std::string::npos)
    {
        mod::Log("ConnectionCheck: server unavailable (%s)", error.c_str());
        return 0;
    }

    bool direct = false;
    bool relay = false;
    if (!IsCurrent(generation) || !RunOnce(args->baseUrl, port, &direct, &relay))
    {
        return 0;
    }
    if (!direct && !relay)
    {
        Sleep(kRetryDelayMs);
        if (!IsCurrent(generation) || !RunOnce(args->baseUrl, port, &direct, &relay))
        {
            return 0;
        }
    }
    if (IsCurrent(generation))
    {
        mod::Log("ConnectionCheck: port=%u direct=%d relay=%d",
                 static_cast<unsigned>(port), direct ? 1 : 0, relay ? 1 : 0);
        const Result result = direct ? Result::Direct : (relay ? Result::Relay : Result::Failed);
        InterlockedExchange(&g_result, static_cast<LONG>(result));
    }
    return 0;
}
}

void Start(uint16_t hostPort)
{
    const LONG generation = InterlockedIncrement(&g_generation);
    InterlockedExchange(&g_result, static_cast<LONG>(Result::None));
    InterlockedExchange(&g_port, hostPort);
    if (!netplay::mod_settings::IsConnectionCheckEnabled())
    {
        return;
    }
    std::string baseUrl = ResolveBaseUrl(
        netplay::mod_settings::ConnectionCheckUrl(), kDefaultBaseUrl);
    if (baseUrl.empty())
    {
        mod::Log("ConnectionCheck: ConnectionCheckUrl must start with https://");
        return;
    }
    auto args = std::make_unique<WorkerArgs>(WorkerArgs{generation, hostPort, std::move(baseUrl)});
    HANDLE thread = CreateThread(nullptr, 0, WorkerThreadProc, args.get(), 0, nullptr);
    if (thread == nullptr)
    {
        mod::Log("ConnectionCheck: worker thread creation failed (%lu)",
                 static_cast<unsigned long>(GetLastError()));
        return;
    }
    args.release();
    CloseHandle(thread);
}

void Cancel()
{
    InterlockedIncrement(&g_generation);
    InterlockedExchange(&g_result, static_cast<LONG>(Result::None));
}

Result GetResult(uint16_t hostPort)
{
    if (InterlockedCompareExchange(&g_port, 0, 0) != hostPort)
    {
        return Result::None;
    }
    return static_cast<Result>(InterlockedCompareExchange(&g_result, 0, 0));
}
}
