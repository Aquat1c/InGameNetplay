#pragma once

#include <string>

namespace netplay::mod_settings
{
struct Settings
{
    std::string offlineVsHumanMode = "Tournament";
    bool writeLogFile = true;
    bool preserveModLogAcrossLaunches = false;
    bool preserveRevivalLogsAcrossLaunches = false;
    bool enableConsole = false;
    bool enableDebugMenu = false;
    bool verboseBridgePatchLogging = false;
    bool verboseSyncDiagnostics = false;
    bool verboseRevival102jLifecycleLogging = false;
    bool hideEmptySetsInBattleLog = true;
    // Per-frame desync detection: state checksums exchanged with a modded
    // peer over a UDP side channel, forensic dumps to <dll>\logs on
    // confirmation, unpruned logEfz history, and [Global] Debug=1 enforced
    // in EfzRevival.ini at session start.
    bool desyncDetection = true;
    // Menu text via the TTF game-RT overlay (crisp badge font) instead of
    // the 5x7 indexed-surface font, where a producer supports it (battle
    // log menu, footer tooltip). Falls back to 5x7 automatically when the
    // D3D9/ImGui overlay is unavailable.
    bool menuTtfText = true;
    // Keyboard binding (DIK_* form) for the async-hosting "return / rehost"
    // hotkey used while the hosting overlay is minimized in-game.
    std::string asyncHostReturnKey = "DIK_F1";
};

void Reload();
const Settings& Get();

bool UseTournamentModeForOfflineVsHuman();
bool IsFileLoggingEnabled();
bool PreserveModLogAcrossLaunches();
bool PreserveRevivalLogsAcrossLaunches();
bool IsConsoleEnabled();
bool IsDebugMenuEnabled();
bool IsVerboseBridgePatchLoggingEnabled();
bool IsVerboseSyncDiagnosticsEnabled();
bool IsVerboseRevival102jLifecycleLoggingEnabled();
bool AreAllVerboseLogsEnabled();
bool HideEmptySetsInBattleLogByDefault();
bool IsDesyncDetectionEnabled();
bool IsMenuTtfTextEnabled();
// Async-hosting return/rehost hotkey as a DIK_* binding value (e.g. "DIK_F1").
const std::string& AsyncHostReturnKeyBinding();
} // namespace netplay::mod_settings
