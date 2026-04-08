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
    bool hideEmptySetsInBattleLog = true;
};

void Reload();
const Settings& Get();

bool UseTournamentModeForOfflineVsHuman();
bool IsFileLoggingEnabled();
bool PreserveModLogAcrossLaunches();
bool PreserveRevivalLogsAcrossLaunches();
bool IsConsoleEnabled();
bool IsDebugMenuEnabled();
bool HideEmptySetsInBattleLogByDefault();
} // namespace netplay::mod_settings
