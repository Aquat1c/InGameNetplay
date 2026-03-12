#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "netplay/core/menu_model.h"

namespace netplay::battle_log
{
struct BattleLogMatch
{
    int lineNumber = 0;
    int matchIndex = 0;
    std::string date;
    std::string time;
    std::string p1CharacterRaw;
    std::string p2CharacterRaw;
    std::string p1CharacterDisplay;
    std::string p2CharacterDisplay;
    int p1Rounds = 0;
    int p2Rounds = 0;
    int p1Score = 0;
    int p2Score = 0;
    int durationSeconds = 0;
};

struct BattleLogSession
{
    int sessionIndex = 0;
    int lineNumber = 0;
    std::string date;
    std::string time;
    std::string p1Name;
    std::string p2Name;
    std::vector<BattleLogMatch> matches;
    std::string finalP1Character;
    std::string finalP2Character;
    std::vector<std::string> p1IconCharacters;
    std::vector<std::string> p2IconCharacters;
    bool p1SwitchedCharacter = false;
    bool p2SwitchedCharacter = false;
    int totalDurationSeconds = 0;
    int64_t sortKey = 0;
};

struct BattleLogFilter
{
    std::string playerName;
    std::string opponentName;
    std::string playerCharacter;
    std::string opponentCharacter;
    std::string setStatus;
    std::string gameCount;
    std::string characterSwitches;
};

struct BattleLogSummary
{
    std::string nickname;
    std::string secondaryNickname;
    int matchingSessions = 0;
    int totalSessions = 0;
    int completedSessions = 0;
    int emptySessions = 0;
    int setWins = 0;
    int setLosses = 0;
    int gameWins = 0;
    int gameLosses = 0;
    int totalGames = 0;
    int totalDurationSeconds = 0;
    double averageGamesPerCompletedSet = 0.0;
    int averageSetDurationSeconds = 0;
    int longestSetByDurationSeconds = 0;
    int longestSetByDurationSessionIndex = -1;
    int longestSetByGamesCount = 0;
    int longestSetByGamesSessionIndex = -1;
    std::string mostUsedCharacter;
    std::string mostUsedMatchup;
    std::string recentOpponent;
    std::string recentTimestamp;
    bool hasSessions = false;
    bool hasPerspective = false;
    bool isHeadToHead = false;
};

struct BattleLogDocument
{
    std::string sourcePath;
    std::string sourceEncoding;
    bool fileExists = false;
    bool saveBattleLogEnabled = true;
    std::vector<BattleLogSession> sessions;
    std::vector<std::string> characterOptions;
};

const netplay::menu::NetplayMenuSpec* GetMenuSpec();

void ResetState();
bool EnterMenu();
void LeaveMenu();
void ShutdownRenderOverlay();

std::string BuildRowLabel(netplay::menu::NetplayMenuAction action);
std::string BuildRowPrimaryText(netplay::menu::NetplayMenuAction action);
std::string BuildRowSecondaryText(netplay::menu::NetplayMenuAction action);
std::string BuildFooterText(netplay::menu::NetplayMenuAction selectedAction);

bool HandleVerticalNavigation(int currentSelection, int delta, int* outNextSelection);
bool HandleInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter);
bool HandleCancel(uint32_t screenContext);
bool ExecuteAction(uint32_t screenContext, netplay::menu::NetplayMenuAction action);

bool DrawOverlayGdi(uint32_t screenContext, bool allowWindowDc);
bool DrawImageOverlayGdi(uint32_t screenContext, bool allowWindowDc);
uint8_t GetMenuDetailForStateExport();
}
