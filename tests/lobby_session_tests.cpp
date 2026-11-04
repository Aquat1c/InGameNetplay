#include <winsock2.h>
#include <windows.h>
#include <atomic>

namespace
{
std::atomic<unsigned> g_wakeSignals{0};

BOOL WINAPI TrackLobbyWakeEvent(HANDLE event)
{
    const BOOL result = ::SetEvent(event);
    if (result != FALSE)
    {
        g_wakeSignals.fetch_add(1);
    }
    return result;
}
}

#define SetEvent TrackLobbyWakeEvent
#include "../src/netplay/core/lobby_client.cpp"
#undef SetEvent

#include <condition_variable>
#include <deque>
#include <iostream>

namespace
{
std::mutex g_transportMutex;
std::condition_variable g_transportCv;
std::deque<std::string> g_joinResponses;
int g_joinRequests = 0;
int g_endRequests = 0;
int g_leaveRequests = 0;
bool g_holdJoin = false;
int g_failures = 0;

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

template<class Predicate>
bool WaitUntil(Predicate predicate)
{
    const DWORD started = GetTickCount();
    do
    {
        if (predicate()) return true;
        Sleep(5);
    }
    while (GetTickCount() - started < 2000u);
    return false;
}

int JoinRequests()
{
    std::lock_guard<std::mutex> lock(g_transportMutex);
    return g_joinRequests;
}

bool Respond(const std::string& url, std::string* body)
{
    const size_t actionStart = url.find("action=");
    if (actionStart == std::string::npos) return false;
    const size_t valueStart = actionStart + 7;
    const std::string action = url.substr(valueStart, url.find('&', valueStart) - valueStart);
    std::unique_lock<std::mutex> lock(g_transportMutex);
    if (action == "join")
    {
        ++g_joinRequests;
        g_transportCv.wait(lock, [] { return !g_holdJoin; });
        if (!g_joinResponses.empty())
        {
            *body = std::move(g_joinResponses.front());
            g_joinResponses.pop_front();
            return !body->empty();
        }
        *body = R"({"status":"OK","id":1,"msg":2,"secret":3})";
    }
    else
    {
        if (action == "end") ++g_endRequests;
        if (action == "leave") ++g_leaveRequests;
        *body = R"({"status":"OK","idle":[["Target",42]],"playing":[],"challenge":[]})";
    }
    return true;
}

void FailJoin()
{
    std::lock_guard<std::mutex> lock(g_transportMutex);
    g_joinResponses.push_back({});
}

void CheckWaitingForRefresh(netplay::lobby::LobbySession& session, int requests)
{
    using netplay::lobby::PollState;
    Expect(WaitUntil([&] { return session.GetStatus().pollState == PollState::Error; }),
        "failed join reports an error");
    Sleep(netplay::lobby::kPollIntervalMs + 50u);
    Expect(session.GetStatus().pollState == PollState::Error && JoinRequests() == requests,
        "failed join stays idle until Refresh");
}
}

namespace mod
{
void Log(const char*, ...) {}
}

namespace netplay::tls
{
struct KeepAliveClient::Connection {};
KeepAliveClient::KeepAliveClient() = default;
KeepAliveClient::~KeepAliveClient() = default;
bool IsAvailable() { return true; }
bool HttpGet(const std::string& url, bool, uint32_t, uint32_t,
    std::string* body, std::string*)
{
    return Respond(url, body);
}
bool KeepAliveClient::Get(const std::string& url, bool verify, uint32_t connectTimeout,
    uint32_t receiveTimeout, std::string* body, std::string* error)
{
    return HttpGet(url, verify, connectTimeout, receiveTimeout, body, error);
}
}

int main()
{
    using namespace netplay::lobby;
    SetEnvironmentVariableA("EFZ_LOBBY_BASE_URL", "https://lobby.invalid");
    SetEnvironmentVariableA("EFZ_LOBBY_PROXY_BASE_URL", "invalid");
    SetEnvironmentVariableA("EFZ_LOBBY_FORCE_EMBEDDED_TLS", "1");
    SetEnvironmentVariableA("EFZ_LOBBY_FORCE_WININET", "0");
    GetWinInetApi().available = false;
    GetWinHttpApi().available = false;
    (void)GetLobbyEndpointConfig();

    FailJoin();
    {
        LobbySession session("Retry", 10800);
        CheckWaitingForRefresh(session, 1);
        FailJoin();
        session.RequestRefresh();
        Expect(WaitUntil([&] { return JoinRequests() == 2; }), "Refresh retries the initial join");
        CheckWaitingForRefresh(session, 2);
        session.RequestRefresh();
        Expect(WaitUntil([&] { return session.GetStatus().pollState == PollState::Polling; }),
            "a successful refresh returns to polling");
        Expect(JoinRequests() == 3, "each Refresh queues one join attempt");
        Expect(session.GetPlayerId() == 2, "retry publishes the joined identity");

        session.SendChallenge(42, "Target", "127.0.0.1:10800");
        Expect(WaitUntil([&] { return session.GetChallengeSendState() == ChallengeSendState::Sent; }),
            "a retried session can send a challenge");
        session.NotifyMatchConnected();
        session.NotifyEndMatch();
        Expect(WaitUntil([&] { return !session.IsInBattle(); }), "End releases the retried match");
    }
    Expect(g_endRequests == 1 && g_leaveRequests == 1, "retried session sends End and Leave on cleanup");

    FailJoin();
    const int beforeCancel = JoinRequests();
    const DWORD cancelStarted = GetTickCount();
    {
        LobbySession session("Cancel", 10800);
        CheckWaitingForRefresh(session, beforeCancel + 1);
        session.NotifyEndMatch();
    }
    Expect(GetTickCount() - cancelStarted < kPollIntervalMs + 2000u,
        "leaving an initial failure wakes and joins the worker");
    Expect(JoinRequests() == beforeCancel + 1 && g_endRequests == 1 && g_leaveRequests == 1,
        "cancelling a failed initial join sends no request for an absent identity");

    FailJoin();
    auto session = std::make_unique<LobbySession>("Cancel retry", 10800);
    Expect(WaitUntil([&] { return session->GetStatus().pollState == PollState::Error; }),
        "cancelled retry fixture reaches initial failure");
    const int beforeRetry = JoinRequests();
    {
        std::lock_guard<std::mutex> lock(g_transportMutex);
        g_holdJoin = true;
    }
    session->RequestRefresh();
    Expect(WaitUntil([&] { return JoinRequests() == beforeRetry + 1; }), "retry enters transport before cancellation");
    const unsigned wakesBeforeShutdown = g_wakeSignals.load();
    std::thread shutdown([owned = std::move(session)]() mutable { owned.reset(); });
    Expect(WaitUntil([&] { return g_wakeSignals.load() > wakesBeforeShutdown; }),
        "destruction publishes stop and wakes the worker before the retry completes");
    {
        std::lock_guard<std::mutex> lock(g_transportMutex);
        g_holdJoin = false;
    }
    g_transportCv.notify_all();
    shutdown.join();
    Expect(g_leaveRequests == 2, "cancellation cleans up an identity received by the in-flight retry");
    return g_failures == 0 ? 0 : 1;
}
