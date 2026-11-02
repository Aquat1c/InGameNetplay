#pragma once

namespace netplay::hooks
{
bool IsNetplayMenuActive();

bool IsOnlineSimulationUiSuspended();

bool IsLobbyBackgrounded();
bool IsLobbyPresenceLost();
const char* LobbyPresenceChallenger();
}
