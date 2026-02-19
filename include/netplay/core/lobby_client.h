#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace netplay::lobby
{
// Maximum number of idle players shown in the lobby browser.
constexpr int kMaxDisplayPlayers = 6;

// Maximum number of playing pairs shown in the lobby browser.
constexpr int kMaxPlayingPairs = 1;

struct LobbyPlayer
{
    std::string name;
    int playerId = 0;
};

struct LobbyPlayingPair
{
    std::string p1Name;
    std::string p2Name;
    int p1Id = 0;
    int p2Id = 0;
    std::string hostIp; // informational only; P2P connection not yet implemented
};

enum class PollState
{
    NotJoined,   // session created but join request not yet sent
    Joining,     // join HTTP request in flight
    Polling,     // joined and periodically polling status
    Error,       // last request failed or join was rejected
    Leaving,     // leave request in flight before thread exits
};

struct LobbyStatus
{
    PollState pollState = PollState::NotJoined;
    std::vector<LobbyPlayer> idlePlayers;   // up to kMaxDisplayPlayers
    std::vector<LobbyPlayingPair> playing;   // up to kMaxPlayingPairs
    std::string statusMessage; // human-readable status or error text
    DWORD lastPollTick = 0;
};

// Manages a single Concerto lobby session for the EFZ lobby (alias "EFZ").
// The join/poll/leave cycle runs on an internal background thread.
// All public methods are thread-safe.
class LobbySession
{
public:
    // |nickname| is the player's display name; |hostPort| is the port they are
    // hosting on (0 if not hosting), advertised in the join request so other
    // players can initiate P2P connections.
    explicit LobbySession(std::string nickname, uint16_t hostPort = 0);
    ~LobbySession();

    // Non-copyable, non-movable.
    LobbySession(const LobbySession&) = delete;
    LobbySession& operator=(const LobbySession&) = delete;

    // Returns the nickname this session was created with.
    const std::string& GetNickname() const;

    // Thread-safe snapshot of the current lobby status.
    LobbyStatus GetStatus() const;

    // Wakes the polling thread so it polls immediately on next iteration.
    void RequestRefresh();

private:
    // Background thread entry point: join → poll loop → leave.
    void PollThreadEntry();

    // Individual HTTP operations (blocking, called from polling thread only).
    bool DoJoin();
    bool DoPollStatus();
    void DoLeave();

    // Performs a synchronous HTTPS GET and returns the response body.
    // Returns empty string on any failure.
    std::string DoHttpGet(const std::string& path);

    // Minimal JSON helpers: extract integer value for a given key.
    static bool ExtractJsonInt(const std::string& json, const char* key, int* out);

    // Parse the idle player array from a status response body.
    // Fills up to kMaxDisplayPlayers entries into out.
    static void ParseIdlePlayers(const std::string& json, std::vector<LobbyPlayer>* out);

    // Parse the playing array from a status response body.
    // Fills up to kMaxPlayingPairs entries into out.
    static void ParsePlayingPairs(const std::string& json, std::vector<LobbyPlayingPair>* out);

    std::string m_nickname;
    uint16_t m_hostPort = 0;

    // Lobby session credentials set by DoJoin.
    int m_lobbyNumericId = 0;
    int m_playerId = 0;
    int m_secret = 0;

    mutable std::mutex m_mutex;
    LobbyStatus m_status;

    std::atomic<bool> m_shouldStop{false};
    std::atomic<bool> m_refreshRequested{false};

    // Signalled to wake the polling thread early (refresh or stop).
    HANDLE m_wakeEvent = nullptr;

    std::thread m_pollThread;
};

} // namespace netplay::lobby
