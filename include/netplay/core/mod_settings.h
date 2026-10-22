#pragma once

#include <cstdint>
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
    bool menuTtfText = true;
    std::string menuTtfFontFace = "Yu Gothic";
    std::string hostingTipFontFace = "Yu Gothic";
    // DIK_* binding form (e.g. "DIK_F1").
    std::string asyncHostReturnKey = "DIK_F1";
    // Ini key "OnlineCustomColors".
    bool onlineCustomColors = true;
    // Dev-only; never ship enabled.
    bool modInteropLoopback = false;
    // modInteropPeer = peer IP; empty peer disables the side-socket transport.
    std::string modInteropPeer = "";
    uint16_t modInteropPort = 10801;
    // 0 = P1/host, 1 = P2/join.
    int modInteropSide = 0;
    // Ini key "CheckForUpdates".
    bool checkForUpdates = true;
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
bool IsMenuTtfTextEnabled();
const std::string& MenuTtfFontFace();
const std::string& HostingTipFontFace();
const std::string& AsyncHostReturnKeyBinding();
bool AreOnlineCustomColorsEnabled();
// Runtime override; does not persist to the ini (Reload() re-reads it).
void SetOnlineCustomColorsEnabled(bool enabled);

bool IsUpdateCheckEnabled();
bool IsModInteropLoopbackEnabled();
const std::string& ModInteropPeer();
uint16_t ModInteropPort();
int ModInteropSide();
} // namespace netplay::mod_settings
