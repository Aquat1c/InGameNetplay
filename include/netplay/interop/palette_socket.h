#pragma once
// Interim UDP side-channel transport for the overlay palette exchange; the
// production path piggybacks Revival's own socket (no extra port/NAT).
// Recv thread -> mutex-guarded queue; Poll/SendSink run on the game thread,
// so OverlayChannel itself needs no locking.

#include <cstddef>
#include <cstdint>

namespace netplay::interop { class OverlayChannel; }

namespace netplay::interop::socket_transport
{
// Bind INADDR_ANY:port, remember the peer, start the recv thread.
// Idempotent; false on bad args or socket failure.
bool Start(const char* peerIp, std::uint16_t port);

// Stop the recv thread and close the socket. Idempotent.
void Stop();

bool IsRunning();

// OverlayChannel::SendSink; game thread only.
bool SendSink(const std::uint8_t* datagram, std::size_t len, void* user);

// Drain queued datagrams into the channel (game thread); returns the count.
std::size_t Poll(OverlayChannel& channel);
} // namespace netplay::interop::socket_transport
