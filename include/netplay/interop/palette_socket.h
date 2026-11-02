#pragma once
// Interim side channel; production piggybacks Revival's socket.

#include <cstddef>
#include <cstdint>

namespace netplay::interop { class OverlayChannel; }

namespace netplay::interop::socket_transport
{
bool Start(const char* peerIp, std::uint16_t port);

void Stop();

bool IsRunning();

// Game thread only.
bool SendSink(const std::uint8_t* datagram, std::size_t len, void* user);

std::size_t Poll(OverlayChannel& channel);
}
