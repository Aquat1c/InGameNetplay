#pragma once

namespace netplay::hooks
{
// True while the in-engine netplay menu overlay is open. Used to:
//  - suppress the async-host F1 return when already in the netplay menu (the
//    user is already there; firing a return-to-menu transition is a no-op/bug);
//  - suppress the ImGui top-middle hosting badge in the netplay menu, where the
//    indexed top-right badge already shows it.
bool IsNetplayMenuActive();
}
