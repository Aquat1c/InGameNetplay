#include <winsock2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int WSAAPI ReceiveFixture(SOCKET, char*, int, int, sockaddr*, int*);
#define recvfrom ReceiveFixture
#include "../src/netplay/interop/palette_socket.cpp"
#undef recvfrom

namespace mod { void Log(const char*, ...) {} }
namespace netplay::interop {
bool OverlayChannel::OnInboundDatagram(const std::uint8_t*, std::size_t) { return true; }
}

namespace {
int received = 0;
std::uint8_t validFrame[netplay::interop::protocol::kMaxFrameBytes];
std::size_t validSize = 0;
void Expect(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}

int WSAAPI ReceiveFixture(SOCKET, char* buffer, int capacity, int, sockaddr* from, int* fromLen)
{
    using namespace netplay::interop::socket_transport;
    ++received;
    if (received == 1)
    {
        WSASetLastError(WSAEMSGSIZE);
        return SOCKET_ERROR;
    }
    Expect(capacity >= static_cast<int>(validSize), "receive buffer capacity");
    std::memcpy(buffer, validFrame, validSize);
    std::memcpy(from, &g_peer, sizeof(g_peer));
    *fromLen = sizeof(g_peer);
    if (received == 2)
    {
        std::memset(buffer + validSize, 0xAA, 160 - validSize);
        return 160;
    }
    g_stop.store(true);
    return static_cast<int>(validSize);
}

int main()
{
    using namespace netplay::interop;
    using namespace netplay::interop::socket_transport;
    validSize = protocol::BuildHello(protocol::kFeaturePalettes, 123,
                                    validFrame, sizeof(validFrame));
    g_peer.sin_family = AF_INET;
    g_peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_peer.sin_port = htons(10800);
    g_stop.store(false);
    RecvThreadMain();
    Expect(received == 3, "oversized datagram does not stop receive loop");
    Expect(g_inbound.size() == 1 && g_inbound[0].size() == validSize
               && std::memcmp(g_inbound[0].data(), validFrame, validSize) == 0,
           "valid datagram after oversized packet is delivered unchanged");
    std::puts("palette transport tests passed");
}
