#pragma once
// Helper-side (EfzRevival.exe) winsock piggyback for the mod-interop overlay
// channel. Installed only in the helper process and only when the master
// OnlineCustomColors flag is on. IAT-hooks three imports:
//   ws2_32!WSARecvFrom   - record overlapped->buffer for the completion hook
//   kernel32!GetQueuedCompletionStatus - OBSERVE recv completions, copy overlay
//                          frames into the toGame IPC ring (no consume: Revival's
//                          dispatcher drops our reserved typeId harmlessly)
//   ws2_32!WSASendTo     - capture the socket + peer endpoint, then flush queued
//                          outbound overlay frames on that same socket
// See overlay_ipc.h for the shared-memory contract.

namespace netplay::interop::helper_hooks
{
// Idempotent. No-op (returns false) unless this is the helper process and the
// OnlineCustomColors flag is set. Attaches the overlay IPC block and patches the
// three IAT slots. Never touches Revival's data flow (RX is observe-only).
bool Install();
void Uninstall();
} // namespace netplay::interop::helper_hooks
