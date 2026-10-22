#pragma once

#include "netplay/core/network_capability.h"

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
constexpr int kMaxDisplayPlayers = 6;

constexpr int kMaxPlayingPairs = 8;

enum class RoomOrigin : uint8_t
{
    GlobalLobby = 0,
    PlayerRooms = 1,
};

struct PublicIpDiscoveryResult
{
    struct FamilyDiagnostics
    {
        uint32_t sourceAttempts = 0;
        uint32_t transportFailures = 0;
        uint32_t parseFailures = 0;
        uint32_t nonGlobalResponses = 0;
        bool resolved = false;
        bool localCapabilityAvailable = false;
        network::LocalFamilyCapability localCapability = {};
    };

    network::NetworkFamily preferredFamily = network::NetworkFamily::IPv4;
    network::NetworkFamily effectiveFamily = network::NetworkFamily::IPv4;
    std::string publicIp;
    bool usedFamilyFallback = false;
    bool cancelled = false;
    FamilyDiagnostics ipv4;
    FamilyDiagnostics ipv6;
};

PublicIpDiscoveryResult DiscoverPublicIpSynchronously(
    network::NetworkFamily preferredFamily,
    const std::atomic<bool>* cancelFlag = nullptr,
    bool allowAlternateFamily = true,
    const network::LocalNetworkCapabilitySnapshot* capabilitySnapshot =
        nullptr);

struct PublicRoomSummary
{
    std::string roomCode;
    int playerCount = 0;
};

struct LobbyJoinedRoom
{
    int lobbyNumericId = 0;
    int playerId = 0;
    int secret = 0;
    std::string roomType;
    std::string roomAlias;
    std::string roomCode;
    RoomOrigin origin = RoomOrigin::GlobalLobby;
    bool isGlobalRoom = false;
};

struct LobbyPlayer
{
    std::string name;
    int playerId = 0;
};

struct LobbyChallenge
{
    std::string name;
    int playerId = 0;
    std::string ipPort;
};

struct LobbyDisplayEntry
{
    std::string name;
    int playerId = 0;
    bool isChallenge = false;
    bool isSelf = false;
    bool isPlaying = false;
    std::string ipPort;
    std::string spectateIp;
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
    NotJoined,
    Joining,
    Polling,
    Error,
    Leaving,
};

struct LobbyStatus
{
    PollState pollState = PollState::NotJoined;
    std::vector<LobbyPlayer> idlePlayers;
    std::vector<LobbyChallenge> challenges;
    std::vector<LobbyDisplayEntry> displayEntries;
    std::vector<LobbyPlayingPair> playing;
    std::string statusMessage;
    std::string publicIp;
    bool publicIpDiscoveryComplete = false;
    bool publicIpDetectionFailed = false;
    network::NetworkFamily preferredFamily = network::NetworkFamily::IPv4;
    network::NetworkFamily effectiveFamily = network::NetworkFamily::IPv4;
    bool usedFamilyFallback = false;
    std::string roomType;
    std::string roomAlias;
    std::string roomCode;
    RoomOrigin roomOrigin = RoomOrigin::GlobalLobby;
    bool isGlobalRoom = false;
    DWORD lastPollTick = 0;
    bool inBattle = false;
};

bool ListPublicRooms(std::vector<PublicRoomSummary>* outRooms, std::string* outError);
bool JoinRoom(
    const std::string& nickname,
    const std::string& roomCode,
    uint16_t hostPort,
    RoomOrigin origin,
    LobbyJoinedRoom* outJoinedRoom,
    std::string* outError);
bool CreateRoom(
    const std::string& nickname,
    const std::string& roomType,
    uint16_t hostPort,
    RoomOrigin origin,
    LobbyJoinedRoom* outJoinedRoom,
    std::string* outError);

class LobbySession
{
public:
    explicit LobbySession(
        std::string nickname,
        uint16_t hostPort = 0,
        const LobbyJoinedRoom* joinedRoom = nullptr,
        network::NetworkFamily preferredFamily = network::NetworkFamily::IPv4);
    ~LobbySession();

    LobbySession(const LobbySession&) = delete;
    LobbySession& operator=(const LobbySession&) = delete;

    const std::string& GetNickname() const;

    LobbyStatus GetStatus() const;

    void RequestRefresh();

    uint16_t GetHostPort() const { return m_hostPort; }
    network::NetworkFamily GetPreferredFamily() const { return m_preferredFamily; }
    network::NetworkFamily GetEffectiveFamily() const;
    bool UsedFamilyFallback() const;

    int GetPlayerId() const;
    RoomOrigin GetOrigin() const { return m_joinedRoom.origin; }
    bool IsGlobalRoom() const { return m_joinedRoom.isGlobalRoom; }
    const std::string& GetRoomType() const { return m_joinedRoom.roomType; }
    const std::string& GetRoomAlias() const { return m_joinedRoom.roomAlias; }
    const std::string& GetRoomCode() const { return m_joinedRoom.roomCode; }

    void SendChallenge(int targetPlayerId, const std::string& targetName, const std::string& ipPort);

    void AcceptChallenge(int challengerPlayerId, const std::string& challengerName);

    bool ConsumeAbandonedIncomingChallenge();

    void NotifyMatchConnected();

    void NotifySpectateStarted();

    void NotifyEndMatch();

    void FlushDeferredEndOnReturn();

    void NotifyEndSpectate(bool preserveUntilRefresh);

    bool IsInBattle() const;

    bool ConsumeAbandonedOutgoingChallenge();

private:
    bool HasPendingEndActionLocked() const;
    void ClearMatchLifecycleState(bool clearStatusInBattle);

    void PollThreadEntry();

    bool DoJoin();
    bool DoPollStatus();
    bool DoLeave();
    bool DoChallenge(int targetPlayerId, const std::string& ipPort);
    bool DoPreAccept(int challengerPlayerId);
    bool DoAccept(int challengerPlayerId);
    bool DoEnd();
    bool TryRejoinIfNeeded();
    bool HandleServerRemovalFailure(const char* operation, const std::string& body);

    void DiscoverPublicIp();
    void StartPublicIpDiscoveryAsync();

    void ProcessPendingActions();

    std::string DoHttpGet(const std::string& path);

    static bool ExtractJsonInt(const std::string& json, const char* key, int* out);

    static void ParseIdlePlayers(const std::string& json, std::vector<LobbyPlayer>* out);

    static void ParsePlayingPairs(const std::string& json, std::vector<LobbyPlayingPair>* out);

    static void ParseChallenges(const std::string& json, std::vector<LobbyChallenge>* out);

    static void BuildDisplayEntries(
        const std::vector<LobbyChallenge>& challenges,
        const std::vector<LobbyPlayer>& idlePlayers,
        const std::vector<LobbyPlayingPair>& playing,
        int selfPlayerId,
        bool selfInBattle,
        std::vector<LobbyDisplayEntry>* out);

    std::string m_nickname;
    uint16_t m_hostPort = 0;
    network::NetworkFamily m_preferredFamily = network::NetworkFamily::IPv4;

    LobbyJoinedRoom m_joinedRoom = {};
    bool m_hasPrejoinedRoom = false;

    std::string m_publicIp;

    int m_pendingAcceptTargetId = 0;
    std::string m_pendingAcceptChallengerName;
    int m_pendingChallengeTargetId = 0;
    std::string m_pendingChallengeTargetName;

    mutable std::mutex m_mutex;
    LobbyStatus m_status;

    struct PendingAction
    {
        enum Type { Challenge, PreAccept, ConfirmAccept, End } type;
        int targetPlayerId = 0;
        std::string ipPort;
    };
    std::vector<PendingAction> m_pendingActions; // guarded by m_mutex

    std::atomic<bool> m_shouldStop{false};
    std::atomic<bool> m_refreshRequested{false};
    std::atomic<bool> m_rejoinRequested{false};
    std::atomic<bool> m_inBattle{false};
    std::atomic<bool> m_returningFromMatch{false};
    std::atomic<bool> m_challengePending{false};
    std::atomic<bool> m_abandonedOutgoingChallenge{false};
    std::atomic<bool> m_abandonedIncomingChallenge{false};
    std::atomic<bool> m_matchConnected{false};
    std::atomic<bool> m_endDeferred{false};
    std::atomic<bool> m_endPending{false};
    std::atomic<bool> m_spectateActive{false};
    std::atomic<bool> m_returningFromSpectate{false};
    std::atomic<bool> m_spectateLeavePending{false};
    std::atomic<bool> m_spectateDetachedFromRoom{false};
    std::atomic<bool> m_isMatchHost{false};

    HANDLE m_wakeEvent = nullptr;

    std::thread m_pollThread;
    std::thread m_publicIpThread;
    std::atomic<bool> m_publicIpDiscoveryStarted{false};

    int m_consecutivePollFailures = 0;
    std::string m_lastLoggedStatusBody;
    DWORD m_lastStatusResponseLogTick = 0;
};

std::string HttpGetViaWinInet(const std::string& url, DWORD connectTimeoutMs, DWORD receiveTimeoutMs);

} // namespace netplay::lobby
