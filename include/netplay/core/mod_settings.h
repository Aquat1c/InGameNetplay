#pragma once

#include <string>

namespace netplay::mod_settings
{
struct Settings
{
    std::string offlineVsHumanMode = "Tournament";
    bool writeLogFile = true;
    bool enableConsole = false;
    bool enableDebugMenu = false;
    bool hideEmptySetsInBattleLog = true;
};

void Reload();
const Settings& Get();

bool UseTournamentModeForOfflineVsHuman();
bool IsFileLoggingEnabled();
bool IsConsoleEnabled();
bool IsDebugMenuEnabled();
bool HideEmptySetsInBattleLogByDefault();
} // namespace netplay::mod_settings
