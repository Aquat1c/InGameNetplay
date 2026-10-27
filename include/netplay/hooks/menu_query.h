#pragma once

namespace netplay::hooks
{
// True while the in-engine netplay menu overlay is open.
bool IsNetplayMenuActive();

// True while UI hooks are suspended for an online simulation handoff.
bool IsOnlineSimulationUiSuspended();
}
