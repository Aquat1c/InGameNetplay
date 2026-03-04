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

// Maximum number of playing pairs tracked from the server.
constexpr int kMaxPlayingPairs = 8;

struct LobbyPlayer
{
    std::string name;
    int playerId = 0;
};

struct LobbyChallenge
{
    std::string name;        // challenger's display name
    int playerId = 0;        // challenger's player ID
    std::string ipPort;      // challenger's public ip:port (e.g. "1.2.3.4:10800")
};

// A unified entry for the lobby display list.  Merges incoming challenges
// (shown first) with idle players so the menu slot system can treat them
// identically while the action handler distinguishes them.
struct LobbyDisplayEntry
{
    std::string name;
    int playerId = 0;
    bool isChallenge = false;  // true → incoming challenge with ipPort
    bool isSelf = false;       // true → this is our own lobby entry
    bool isPlaying = false;    // true → this player is in an active match
    std::string ipPort;        // only valid when isChallenge == true
    std::string spectateIp;    // host ip:port for spectating (valid when isPlaying)
};

struct LobbyPlayingPair
{
    std::string p1Name;
    std::string p2Name;
    int p1Id = 0;
    int p2Id = 0;
    std::string hostIp;
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
    std::vector<LobbyPlayer> idlePlayers;        // raw idle list from server
    std::vector<LobbyChallenge> challenges;       // raw challenge list from server
    std::vector<LobbyDisplayEntry> displayEntries; // merged: challenges first, then idle
    std::vector<LobbyPlayingPair> playing;        // up to kMaxPlayingPairs
    std::string statusMessage; // human-readable status or error text
    std::string publicIp;      // our discovered public IP (empty until resolved)
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

    // Returns the host port this session was created with.
    uint16_t GetHostPort() const { return m_hostPort; }

    // Returns our player ID assigned by the server (0 if not yet joined).
    int GetPlayerId() const;

    // Queue a challenge request for |targetPlayerId|.  |ipPort| is our
    // public ip:port string (e.g. "1.2.3.4:10800").  Processed on the
    // poll thread during the next iteration.
    void SendChallenge(int targetPlayerId, const std::string& ipPort);

    // Queue a pre_accept for an incoming challenge from |challengerPlayerId|.
    // The actual accept is deferred until NotifyMatchConnected() is called.
    void AcceptChallenge(int challengerPlayerId);

    // Notify that a P2P connection was successfully established after
    // accepting a challenge.  Sends the deferred 'accept' call to the
    // lobby server so the pair appears as "playing".
    void NotifyMatchConnected();

    // Notify that a match/challenge has ended (user cancelled or
    // connection dropped).  Sends 'end' to reset lobby status to idle.
    void NotifyEndMatch();

private:
    // Background thread entry point: join → poll loop → leave.
    void PollThreadEntry();

    // Individual HTTP operations (blocking, called from polling thread only).
    bool DoJoin();
    bool DoPollStatus();
    void DoLeave();
    bool DoChallenge(int targetPlayerId, const std::string& ipPort);
    bool DoPreAccept(int challengerPlayerId);
    bool DoAccept(int challengerPlayerId);
    bool DoEnd();

    // Discover our public IP address via an external service.
    void DiscoverPublicIp();

    // Process any queued actions from the main thread.
    void ProcessPendingActions();

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

    // Parse the challenges array from a status response body.
    static void ParseChallenges(const std::string& json, std::vector<LobbyChallenge>* out);

    // Build the merged display list from challenges + idle players.
    // |selfPlayerId| is our own player ID (used to mark our entry and
    // exclude ourselves from being challengeable).  Challengers that also
    // appear in idlePlayers are deduplicated.  Players in |playing| pairs
    // are flagged so the UI can show them distinctly.
    static void BuildDisplayEntries(
        const std::vector<LobbyChallenge>& challenges,
        const std::vector<LobbyPlayer>& idlePlayers,
        const std::vector<LobbyPlayingPair>& playing,
        int selfPlayerId,
        std::vector<LobbyDisplayEntry>* out);

    std::string m_nickname;
    uint16_t m_hostPort = 0;

    // Lobby session credentials set by DoJoin.
    int m_lobbyNumericId = 0;
    int m_playerId = 0;
    int m_secret = 0;

    // Discovered public IP (empty until resolved).
    std::string m_publicIp;

    // Target player ID from the last pre_accept, used for the deferred accept.
    int m_pendingAcceptTargetId = 0;

    mutable std::mutex m_mutex;
    LobbyStatus m_status;

    // Pending actions queued by the main thread for the poll thread.
    struct PendingAction
    {
        enum Type { Challenge, PreAccept, ConfirmAccept, End } type;
        int targetPlayerId = 0;
        std::string ipPort;
    };
    std::vector<PendingAction> m_pendingActions; // guarded by m_mutex

    std::atomic<bool> m_shouldStop{false};
    std::atomic<bool> m_refreshRequested{false};

    // Signalled to wake the polling thread early (refresh or stop).
    HANDLE m_wakeEvent = nullptr;

    std::thread m_pollThread;
};

} // namespace netplay::lobby
