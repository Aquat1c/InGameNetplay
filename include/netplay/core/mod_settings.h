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
    std::string language = "English";
    std::string hostingTipFontFace = "Yu Gothic";
    std::string asyncHostReturnKey = "DIK_F1";
    bool onlineCustomColors = true;
    // Dev-only; never ship enabled.
    bool modInteropLoopback = false;
    std::string modInteropPeer = "";
    uint16_t modInteropPort = 10801;
    int modInteropSide = 0;
    bool checkForUpdates = true;
    bool connectionCheck = true;
    std::string connectionCheckUrl = "";
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
const std::string& Language();
const std::string& HostingTipFontFace();
const std::string& AsyncHostReturnKeyBinding();

// Detected at runtime: the XP build also runs on newer systems.
bool IsWindowsXpFamily();
bool AreOnlineCustomColorsEnabled();
void SetOnlineCustomColorsEnabled(bool enabled);

bool IsUpdateCheckEnabled();
bool IsConnectionCheckEnabled();
const std::string& ConnectionCheckUrl();
bool IsModInteropLoopbackEnabled();
const std::string& ModInteropPeer();
uint16_t ModInteropPort();
int ModInteropSide();
}
