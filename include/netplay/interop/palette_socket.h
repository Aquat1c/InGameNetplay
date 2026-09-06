#pragma once
// Stage-2 interim UDP side-channel transport for the overlay palette exchange.
// Runs in efz.exe (where the channel lives), on a shared side port both clients
// bind, sending to the configured peer. This is the DIRECT/LAN test transport;
// the production path piggybacks Revival's own socket (no extra port / NAT).
//
// Threading: a background thread blocks on recvfrom and pushes datagrams into a
// mutex-guarded queue. The channel stays single-threaded - the game thread
// drains the queue (Poll) and feeds OverlayChannel::OnInboundDatagram, and the
// send sink is called only from the game thread. So OverlayChannel itself needs
// no locking.

#include <cstddef>
#include <cstdint>

namespace netplay::interop { class OverlayChannel; }

namespace netplay::interop::socket_transport
{
// Open the UDP socket, bind INADDR_ANY:port, remember peerIp:port, and start the
// recv thread. Returns false on bad args or socket failure. Idempotent.
bool Start(const char* peerIp, std::uint16_t port);

// Stop the recv thread and close the socket. Idempotent.
void Stop();

bool IsRunning();

// OverlayChannel::SendSink - transmit a framed datagram to the peer (game
// thread only). Signature matches SendSink.
bool SendSink(const std::uint8_t* datagram, std::size_t len, void* user);

// Drain any datagrams the recv thread queued into the channel (game thread).
// Returns the number delivered.
std::size_t Poll(OverlayChannel& channel);
} // namespace netplay::interop::socket_transport
