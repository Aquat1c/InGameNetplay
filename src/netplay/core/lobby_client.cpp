#include "netplay/core/lobby_client.h"

#include "logger.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

namespace netplay::lobby
{
namespace
{
constexpr const wchar_t* kConcertoHost = L"concerto-mbaacc.shib.live";
constexpr INTERNET_PORT kConcertoPort = INTERNET_DEFAULT_HTTPS_PORT;
constexpr DWORD kConnectTimeoutMs = 8000;
constexpr DWORD kReceiveTimeoutMs = 12000;
constexpr DWORD kPollIntervalMs = 3000;

// Minimal percent-encoding for a query-string value.
std::string UrlEncode(const std::string& s)
{
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '~')
        {
            out += static_cast<char>(c);
        }
        else if (c == ' ')
        {
            out += '+';
        }
        else
        {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned>(c));
            out += buf;
        }
    }
    return out;
}
} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

LobbySession::LobbySession(std::string nickname, uint16_t hostPort)
    : m_nickname(std::move(nickname))
    , m_hostPort(hostPort)
{
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_wakeEvent == nullptr)
    {
        mod::Log("LobbySession: CreateEvent failed (%lu)", GetLastError());
    }

    m_status.pollState = PollState::NotJoined;
    m_pollThread = std::thread(&LobbySession::PollThreadEntry, this);
    mod::Log("LobbySession: started for nickname='%s'", m_nickname.c_str());
}

LobbySession::~LobbySession()
{
    mod::Log("LobbySession: shutting down");
    m_shouldStop.store(true);
    if (m_wakeEvent != nullptr)
    {
        SetEvent(m_wakeEvent);
    }
    if (m_pollThread.joinable())
    {
        m_pollThread.join();
    }
    if (m_wakeEvent != nullptr)
    {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
    mod::Log("LobbySession: shut down complete");
}

const std::string& LobbySession::GetNickname() const
{
    return m_nickname;
}

LobbyStatus LobbySession::GetStatus() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void LobbySession::RequestRefresh()
{
    m_refreshRequested.store(true);
    if (m_wakeEvent != nullptr)
    {
        SetEvent(m_wakeEvent);
    }
}

// ---------------------------------------------------------------------------
// Background thread
// ---------------------------------------------------------------------------

void LobbySession::PollThreadEntry()
{
    mod::Log("LobbySession::PollThread: entering");

    // Update state to Joining before the HTTP call.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Joining;
        m_status.statusMessage = "Connecting to lobby...";
    }

    if (m_shouldStop.load())
    {
        mod::Log("LobbySession::PollThread: stop requested before join");
        return;
    }

    if (!DoJoin())
    {
        mod::Log("LobbySession::PollThread: join failed, exiting");
        return;
    }

    mod::Log("LobbySession::PollThread: joined lobby id=%d playerId=%d", m_lobbyNumericId, m_playerId);

    // Poll loop.
    while (!m_shouldStop.load())
    {
        m_refreshRequested.store(false);
        DoPollStatus();

        // Wait for kPollIntervalMs or until woken early.
        if (m_wakeEvent != nullptr)
        {
            WaitForSingleObject(m_wakeEvent, kPollIntervalMs);
        }
        else
        {
            Sleep(kPollIntervalMs);
        }
    }

    // Leave the lobby on the way out.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Leaving;
        m_status.statusMessage = "Leaving lobby...";
    }
    DoLeave();
    mod::Log("LobbySession::PollThread: exiting");
}

// ---------------------------------------------------------------------------
// Individual lobby HTTP operations
// ---------------------------------------------------------------------------

bool LobbySession::DoJoin()
{
    char path[512];
    if (m_hostPort != 0)
    {
        std::snprintf(
            path,
            sizeof(path),
            "/l?action=join&id=EFZ&game=efz&name=%s&port=%u",
            UrlEncode(m_nickname).c_str(),
            static_cast<unsigned>(m_hostPort));
    }
    else
    {
        std::snprintf(
            path,
            sizeof(path),
            "/l?action=join&id=EFZ&game=efz&name=%s",
            UrlEncode(m_nickname).c_str());
    }

    const std::string body = DoHttpGet(path);
    if (body.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Error;
        m_status.statusMessage = "HTTP request failed";
        return false;
    }

    mod::Log("LobbySession::DoJoin: response='%s'", body.c_str());

    // Expect {"status":"OK","id":N,"msg":N,"secret":N}
    if (body.find("\"OK\"") == std::string::npos)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Error;
        m_status.statusMessage = "Join rejected by server";
        return false;
    }

    int numericId = 0;
    int playerId = 0;
    int secret = 0;
    if (!ExtractJsonInt(body, "id", &numericId)
        || !ExtractJsonInt(body, "msg", &playerId)
        || !ExtractJsonInt(body, "secret", &secret))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Error;
        m_status.statusMessage = "Failed to parse join response";
        return false;
    }

    m_lobbyNumericId = numericId;
    m_playerId = playerId;
    m_secret = secret;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.pollState = PollState::Polling;
    m_status.statusMessage.clear();
    return true;
}

bool LobbySession::DoPollStatus()
{
    char path[512];
    std::snprintf(
        path,
        sizeof(path),
        "/l?action=status&id=%d&p=%d&secret=%d",
        m_lobbyNumericId,
        m_playerId,
        m_secret);

    const std::string body = DoHttpGet(path);
    if (body.empty())
    {
        mod::Log("LobbySession::DoPollStatus: empty response");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Error;
        m_status.statusMessage = "Poll request failed";
        return false;
    }

    mod::Log("LobbySession::DoPollStatus: response='%s'", body.c_str());

    std::vector<LobbyPlayer> idlePlayers;
    ParseIdlePlayers(body, &idlePlayers);

    std::vector<LobbyPlayingPair> playingPairs;
    ParsePlayingPairs(body, &playingPairs);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.pollState = PollState::Polling;
    m_status.idlePlayers = std::move(idlePlayers);
    m_status.playing = std::move(playingPairs);
    m_status.statusMessage.clear();
    m_status.lastPollTick = GetTickCount();
    return true;
}

void LobbySession::DoLeave()
{
    if (m_lobbyNumericId == 0)
    {
        return;
    }

    char path[512];
    std::snprintf(
        path,
        sizeof(path),
        "/l?action=leave&id=%d&p=%d&secret=%d",
        m_lobbyNumericId,
        m_playerId,
        m_secret);

    const std::string body = DoHttpGet(path);
    mod::Log("LobbySession::DoLeave: response='%s'", body.c_str());
}

// ---------------------------------------------------------------------------
// WinHTTP helper
// ---------------------------------------------------------------------------

std::string LobbySession::DoHttpGet(const std::string& path)
{
    std::string result;

    HINTERNET hSession = WinHttpOpen(
        L"EFZNetplayMod/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (hSession == nullptr)
    {
        mod::Log("LobbySession::DoHttpGet: WinHttpOpen failed (%lu)", GetLastError());
        return result;
    }

    WinHttpSetTimeouts(hSession,
        static_cast<int>(kConnectTimeoutMs),
        static_cast<int>(kConnectTimeoutMs),
        static_cast<int>(kReceiveTimeoutMs),
        static_cast<int>(kReceiveTimeoutMs));

    HINTERNET hConnect = WinHttpConnect(hSession, kConcertoHost, kConcertoPort, 0);
    if (hConnect == nullptr)
    {
        mod::Log("LobbySession::DoHttpGet: WinHttpConnect failed (%lu)", GetLastError());
        WinHttpCloseHandle(hSession);
        return result;
    }

    // Convert the narrow path to wide.
    int wLen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wPath(static_cast<size_t>(wLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wPath[0], wLen);

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        wPath.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (hRequest == nullptr)
    {
        mod::Log("LobbySession::DoHttpGet: WinHttpOpenRequest failed (%lu)", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    const BOOL sent = WinHttpSendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0);
    if (!sent || !WinHttpReceiveResponse(hRequest, nullptr))
    {
        mod::Log("LobbySession::DoHttpGet: send/receive failed (%lu)", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    DWORD available = 0;
    while (WinHttpQueryDataAvailable(hRequest, &available) && available > 0)
    {
        const size_t oldSize = result.size();
        result.resize(oldSize + available);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, &result[oldSize], available, &bytesRead))
        {
            break;
        }
        result.resize(oldSize + bytesRead);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return result;
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers
// ---------------------------------------------------------------------------

bool LobbySession::ExtractJsonInt(const std::string& json, const char* key, int* out)
{
    // Search for "key": and parse the integer that follows.
    const std::string search = std::string("\"") + key + "\":";
    const size_t pos = json.find(search);
    if (pos == std::string::npos)
    {
        return false;
    }

    size_t valuePos = pos + search.size();
    while (valuePos < json.size() && json[valuePos] == ' ')
    {
        ++valuePos;
    }

    if (valuePos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[valuePos])))
    {
        return false;
    }

    char* endPtr = nullptr;
    *out = static_cast<int>(std::strtol(json.c_str() + valuePos, &endPtr, 10));
    return (endPtr != json.c_str() + valuePos);
}

void LobbySession::ParseIdlePlayers(const std::string& json, std::vector<LobbyPlayer>* out)
{
    out->clear();

    // Find the idle array: "idle":[
    const char* idleTag = "\"idle\":[";
    const size_t idlePos = json.find(idleTag);
    if (idlePos == std::string::npos)
    {
        return;
    }

    size_t cursor = idlePos + std::strlen(idleTag);

    // Each element is either [] (empty) or ["name",playerId]
    while (cursor < json.size() && out->size() < static_cast<size_t>(kMaxDisplayPlayers))
    {
        // Skip whitespace and commas between elements.
        while (cursor < json.size() && (json[cursor] == ',' || json[cursor] == ' ' || json[cursor] == '\n' || json[cursor] == '\r'))
        {
            ++cursor;
        }

        if (cursor >= json.size() || json[cursor] == ']')
        {
            break; // End of idle array.
        }

        if (json[cursor] != '[')
        {
            break; // Unexpected character.
        }
        ++cursor; // Consume '['.

        // Skip whitespace.
        while (cursor < json.size() && json[cursor] == ' ')
        {
            ++cursor;
        }

        if (cursor >= json.size() || json[cursor] != '"')
        {
            break; // Expected quoted name.
        }
        ++cursor; // Consume opening '"'.

        // Read the player name until closing '"'.
        std::string playerName;
        while (cursor < json.size() && json[cursor] != '"')
        {
            // Basic escape: skip backslash and next char.
            if (json[cursor] == '\\' && cursor + 1 < json.size())
            {
                ++cursor;
            }
            playerName += json[cursor];
            ++cursor;
        }
        if (cursor < json.size())
        {
            ++cursor; // Consume closing '"'.
        }

        // Skip comma between name and id.
        while (cursor < json.size() && (json[cursor] == ',' || json[cursor] == ' '))
        {
            ++cursor;
        }

        // Read player id.
        if (cursor >= json.size() || !std::isdigit(static_cast<unsigned char>(json[cursor])))
        {
            break;
        }
        char* endPtr = nullptr;
        const int playerId = static_cast<int>(std::strtol(json.c_str() + cursor, &endPtr, 10));
        cursor = static_cast<size_t>(endPtr - json.c_str());

        // Advance past the closing ']' of this element.
        while (cursor < json.size() && json[cursor] != ']')
        {
            ++cursor;
        }
        if (cursor < json.size())
        {
            ++cursor;
        }

        LobbyPlayer player;
        player.name = std::move(playerName);
        player.playerId = playerId;
        out->push_back(std::move(player));
    }
}

void LobbySession::ParsePlayingPairs(const std::string& json, std::vector<LobbyPlayingPair>* out)
{
    out->clear();

    // Find the playing array: "playing":[
    // Each element is ["p1name","p2name",p1id,p2id,"host_ip"]
    const char* playingTag = "\"playing\":";
    const size_t playingPos = json.find(playingTag);
    if (playingPos == std::string::npos)
    {
        return;
    }

    // Find the opening '[' of the playing array.
    size_t arrayStart = json.find('[', playingPos + std::strlen(playingTag));
    if (arrayStart == std::string::npos)
    {
        return;
    }
    size_t cursor = arrayStart + 1;

    auto skipWhitespace = [&]()
    {
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\n' || json[cursor] == '\r'))
        {
            ++cursor;
        }
    };

    auto readQuotedString = [&](std::string* dst) -> bool
    {
        skipWhitespace();
        if (cursor >= json.size() || json[cursor] != '"')
        {
            return false;
        }
        ++cursor; // consume opening '"'
        dst->clear();
        while (cursor < json.size() && json[cursor] != '"')
        {
            if (json[cursor] == '\\' && cursor + 1 < json.size())
            {
                ++cursor;
            }
            *dst += json[cursor];
            ++cursor;
        }
        if (cursor < json.size())
        {
            ++cursor; // consume closing '"'
        }
        return true;
    };

    auto readInt = [&](int* dst) -> bool
    {
        skipWhitespace();
        if (cursor >= json.size() || !std::isdigit(static_cast<unsigned char>(json[cursor])))
        {
            return false;
        }
        char* endPtr = nullptr;
        *dst = static_cast<int>(std::strtol(json.c_str() + cursor, &endPtr, 10));
        cursor = static_cast<size_t>(endPtr - json.c_str());
        return true;
    };

    while (cursor < json.size() && out->size() < static_cast<size_t>(kMaxPlayingPairs))
    {
        // Skip commas and whitespace between elements.
        while (cursor < json.size() && (json[cursor] == ',' || json[cursor] == ' ' || json[cursor] == '\n'))
        {
            ++cursor;
        }
        if (cursor >= json.size() || json[cursor] == ']')
        {
            break;
        }
        if (json[cursor] != '[')
        {
            break;
        }
        ++cursor; // consume '['

        LobbyPlayingPair pair;
        if (!readQuotedString(&pair.p1Name))
        {
            break;
        }
        skipWhitespace();
        if (cursor < json.size() && json[cursor] == ',')
        {
            ++cursor;
        }
        if (!readQuotedString(&pair.p2Name))
        {
            break;
        }
        skipWhitespace();
        if (cursor < json.size() && json[cursor] == ',')
        {
            ++cursor;
        }
        readInt(&pair.p1Id);
        skipWhitespace();
        if (cursor < json.size() && json[cursor] == ',')
        {
            ++cursor;
        }
        readInt(&pair.p2Id);
        skipWhitespace();
        if (cursor < json.size() && json[cursor] == ',')
        {
            ++cursor;
        }
        readQuotedString(&pair.hostIp); // optional, may not be present

        // Advance past the closing ']' of this element.
        while (cursor < json.size() && json[cursor] != ']')
        {
            ++cursor;
        }
        if (cursor < json.size())
        {
            ++cursor;
        }

        out->push_back(std::move(pair));
    }
}
}
