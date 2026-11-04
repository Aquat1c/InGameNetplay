// Prefer EFZNetplay_CopyState(output, outputSize, minimumVersion), returning int (__cdecl).
// Returns 1 for a coherent bounded copy; 0 leaves output untouched. outputSize must be >= 12.
// Legacy EFZNetplay_GetState and the "EFZNetplay_State" mapping expose mutable storage.
// Validate magic, version and structSize before using any copied fields.
#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

#define EFZ_NETPLAY_STATE_MAGIC       0x4E5A4645u
#define EFZ_NETPLAY_STATE_VERSION     7u
#define EFZ_NETPLAY_STATE_SHM_NAME    "EFZNetplay_State"

// Each bit marks a field group as populated.
#define EFZ_CAP_SESSION       (1u << 0)  // sessionMode, sessionPhase, localSide
#define EFZ_CAP_SCORES        (1u << 1)  // p1Wins, p2Wins, matchCounter
#define EFZ_CAP_NICKNAMES     (1u << 2)  // localNickname, p1Name, p2Name
#define EFZ_CAP_NETWORK       (1u << 3)  // pingMs, rollbackFrames
#define EFZ_CAP_MENU          (1u << 4)  // inNetplayMenu, netplayMenuScreen, netplayMenuDetail
#define EFZ_CAP_REVIVAL       (1u << 5)  // revivalVersion
#define EFZ_CAP_GAME_FLOW     (1u << 6)  // inNetplayCharacterSelect, inNetplayMatch
#define EFZ_CAP_ACTIVITY      (1u << 7)  // activityPhase, endReason
#define EFZ_CAP_CHAR_SELECT   (1u << 8)  // p1CharId, p2CharId, p1Locked, p2Locked, localCursorCharId
#define EFZ_CAP_MATCH_CONTEXT (1u << 9)  // stageId, roundIndex, roundTimerFrames, isRoundActive
#define EFZ_CAP_ASYNC_HOST    (1u << 10) // asyncHost* fields, hostPort  (v7)
#define EFZ_CAP_NET_DETAIL    (1u << 11) // avg/min/max ping, recommended/min/max delay  (v7)
#define EFZ_CAP_CONNECTION    (1u << 12) // connectionAddress  (v7)

enum EFZNetplaySessionMode
{
    EFZ_SESSION_NONE       = 0,
    EFZ_SESSION_HOSTING    = 1,
    EFZ_SESSION_JOINING    = 2,
    EFZ_SESSION_SPECTATING = 3,
    EFZ_SESSION_TOURNAMENT = 4,
};

enum EFZNetplaySessionPhase
{
    EFZ_PHASE_IDLE          = 0,
    EFZ_PHASE_CONNECTING    = 1,
    EFZ_PHASE_DELAY_SETUP   = 2,
    EFZ_PHASE_CONNECTED     = 3,
    EFZ_PHASE_FAILED        = 4,
    EFZ_PHASE_SESSION_ENDED = 5,
};

enum EFZNetplayActivityPhase
{
    EFZ_ACTIVITY_IDLE           = 0,
    EFZ_ACTIVITY_MENU           = 1,
    EFZ_ACTIVITY_CONNECTING     = 2,
    EFZ_ACTIVITY_DELAY_SETUP    = 3,
    EFZ_ACTIVITY_CHAR_SELECT    = 4,
    EFZ_ACTIVITY_LOADING        = 5,
    EFZ_ACTIVITY_MATCH          = 6,
    EFZ_ACTIVITY_RESULTS        = 7,
    EFZ_ACTIVITY_HOST_IDLE      = 8,  // Hosting while playing normally; not a live match
};

enum EFZNetplayEndReason
{
    EFZ_END_NONE               = 0,
    EFZ_END_GRACEFUL           = 1,
    EFZ_END_DISCONNECT         = 2,
    EFZ_END_CANCELLED          = 3,
    EFZ_END_CONNECT_FAILED     = 4,
    EFZ_END_PEER_PROCESS_DIED  = 5,
    EFZ_END_UNKNOWN            = 255,
};

enum EFZNetplayMenuScreen
{
    EFZ_MENU_MAIN         = 0,
    EFZ_MENU_HOST         = 1,
    EFZ_MENU_JOIN         = 2,
    EFZ_MENU_PLAYER_ROOMS = 3,
    EFZ_MENU_OPTIONS      = 4,
    EFZ_MENU_NICKNAME     = EFZ_MENU_OPTIONS, // Legacy alias retained for source compatibility
    EFZ_MENU_LOBBY        = 5,
    EFZ_MENU_BATTLE_LOG   = 6,
};

enum EFZNetplayMenuDetail
{
    EFZ_MENU_DETAIL_NONE = 0,

    EFZ_MENU_DETAIL_PLAYER_ROOMS_ROOT   = 1,
    EFZ_MENU_DETAIL_PLAYER_ROOMS_JOIN   = 2,
    EFZ_MENU_DETAIL_PLAYER_ROOMS_CREATE = 3,

    EFZ_MENU_DETAIL_OPTIONS_ROOT     = 16,
    EFZ_MENU_DETAIL_OPTIONS_CATEGORY = 17,
    EFZ_MENU_DETAIL_OPTIONS_EDIT     = 18,
    EFZ_MENU_DETAIL_OPTIONS_MODAL    = 19,

    EFZ_MENU_DETAIL_BATTLE_LOG_SUMMARY_PROFILE = 32,
    EFZ_MENU_DETAIL_BATTLE_LOG_SUMMARY_FULL    = 33,
    EFZ_MENU_DETAIL_BATTLE_LOG_SUMMARY_SEARCH  = 34,
    EFZ_MENU_DETAIL_BATTLE_LOG_BROWSER         = 35,
    EFZ_MENU_DETAIL_BATTLE_LOG_FILTERS         = 36,
    EFZ_MENU_DETAIL_BATTLE_LOG_SET_DETAILS     = 37,
};

struct EFZNetplayState
{
    uint32_t magic;
    uint32_t version;
    uint32_t structSize;
    uint32_t lastUpdateTick;

    int32_t  sessionMode;
    int32_t  sessionPhase;
    int32_t  localSide;          // 0 = P1 (host), 1 = P2 (joiner), -1 = unknown / spectator

    int32_t  p1Wins;
    int32_t  p2Wins;
    int32_t  matchCounter;

    char     localNickname[64];  // UTF-8
    char     p1Name[64];
    char     p2Name[64];

    int32_t  pingMs;             // -1 = unavailable
    int32_t  rollbackFrames;     // -1 = not set

    uint8_t  inNetplayMenu;
    uint8_t  netplayMenuScreen;  // Only valid when inNetplayMenu is non-zero
    uint8_t  netplayMenuDetail;  // Only valid when inNetplayMenu is non-zero
    uint8_t  _pad0;

    // Null-terminated, e.g. "1.02e"
    char     revivalVersion[16];

    uint8_t  inNetplayCharacterSelect;
    uint8_t  inNetplayMatch;
    uint8_t  _pad1[2];

    uint32_t capabilityFlags;    // Bitmask of EFZ_CAP_*
    uint32_t stateSeq;           // Incremented every publish; compare to detect stale reads
    uint32_t sessionId;
    uint32_t setId;

    uint8_t  activityPhase;
    uint8_t  endReason;          // Latches until the next session starts
    uint8_t  _pad2[2];

    uint8_t  p1CharId;           // Engine character ID, not efz-training-mode's CHAR_ID_*; 0xFF = unknown
    uint8_t  p2CharId;           // Same rules as p1CharId
    uint8_t  p1Locked;
    uint8_t  p2Locked;
    uint8_t  localCursorCharId;  // 0xFF = unavailable
    uint8_t  _pad3[3];

    uint8_t  stageId;            // 0xFF = unknown
    uint8_t  roundIndex;         // 0-based, 0xFF = unknown
    uint8_t  isRoundActive;
    uint8_t  _pad4;
    uint16_t roundTimerFrames;   // 0xFFFF = unknown
    uint8_t  _pad5[2];

    uint8_t  asyncHostActive;
    uint8_t  asyncHostMinimized;
    uint8_t  asyncHostPeerFound;
    uint8_t  asyncHostTimedOut;
    uint16_t hostPort;            // 0 = n/a
    uint8_t  _pad6[2];

    int32_t  avgPingMs;
    int32_t  minPingMs;
    int32_t  maxPingMs;
    int32_t  recommendedDelay;
    int32_t  minDelay;
    int32_t  maxDelay;

    char     connectionAddress[64];
};

#ifdef __cplusplus
}
#endif
