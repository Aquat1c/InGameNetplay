#pragma once
// See overlay_ipc.h for the shared-memory contract.

namespace netplay::interop::helper_hooks
{
bool Install();
void Uninstall();
} // namespace netplay::interop::helper_hooks
