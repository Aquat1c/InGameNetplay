#include "netplay/interop/palette_socket.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "logger.h"
#include "netplay/interop/overlay_channel.h"
#include "netplay/interop/overlay_protocol.h"

namespace netplay::interop::socket_transport
{
namespace
{
namespace P = netplay::interop::protocol;

std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
SOCKET g_sock = INVALID_SOCKET;
sockaddr_in g_peer{};
std::thread g_recvThread;
bool g_wsaStarted = false;

std::mutex g_queueMutex;
std::vector<std::vector<std::uint8_t>> g_inbound;   // datagrams awaiting drain
constexpr std::size_t kMaxQueued = 64;

void RecvThreadMain()
{
    mod::Log("PaletteSocket: recv thread started");
    std::uint8_t buf[2048];
    sockaddr_in from{};
    while (!g_stop.load(std::memory_order_acquire))
    {
        int fromLen = sizeof(from);
        const int n = recvfrom(g_sock, reinterpret_cast<char*>(buf),
                               static_cast<int>(sizeof(buf)), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n == SOCKET_ERROR)
        {
            if (g_stop.load(std::memory_order_acquire))
            {
                break;
            }
            const int err = WSAGetLastError();
            // Bounded UDP errors (e.g. ICMP port-unreachable -> WSAECONNRESET)
            // are non-fatal; keep listening.
            if (err == WSAECONNRESET || err == WSAEINTR)
            {
                continue;
            }
            mod::Log("PaletteSocket: recvfrom error=%d, stopping recv", err);
            break;
        }
        if (n < static_cast<int>(P::kEnvelopeBytes + sizeof(P::FrameHeader)))
        {
            continue;   // too small to be one of ours
        }
        if (!P::LooksLikeOverlayFrame(buf, static_cast<std::size_t>(n)))
        {
            continue;   // stray traffic on our port
        }
        static unsigned s_rx = 0;
        if (++s_rx <= 3u || (s_rx % 60u) == 0u)
        {
            mod::Log("PaletteSocket: recv #%u %dB from %u.%u.%u.%u", s_rx, n,
                     from.sin_addr.S_un.S_un_b.s_b1, from.sin_addr.S_un.S_un_b.s_b2,
                     from.sin_addr.S_un.S_un_b.s_b3, from.sin_addr.S_un.S_un_b.s_b4);
        }
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (g_inbound.size() < kMaxQueued)
        {
            g_inbound.emplace_back(buf, buf + n);
        }
    }
    mod::Log("PaletteSocket: recv thread stopped");
}
} // namespace

bool Start(const char* peerIp, std::uint16_t port)
{
    if (g_running.load(std::memory_order_acquire))
    {
        return true;
    }
    if (peerIp == nullptr || peerIp[0] == '\0' || port == 0)
    {
        return false;
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0)
    {
        g_wsaStarted = true;
    }

    g_sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET)
    {
        mod::Log("PaletteSocket: socket() failed err=%d", WSAGetLastError());
        Stop();
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(port);
    if (::bind(g_sock, reinterpret_cast<sockaddr*>(&local), sizeof(local))
        == SOCKET_ERROR)
    {
        mod::Log("PaletteSocket: bind(%u) failed err=%d - is the port free?",
                 static_cast<unsigned>(port), WSAGetLastError());
        Stop();
        return false;
    }

    g_peer = sockaddr_in{};
    g_peer.sin_family = AF_INET;
    g_peer.sin_port = htons(port);
    // inet_addr (not inet_pton) for XP compatibility (WINVER 0x0501). IPv4 only,
    // which is all the interim test transport needs.
    const unsigned long addr = ::inet_addr(peerIp);
    if (addr == INADDR_NONE)
    {
        mod::Log("PaletteSocket: bad peer IP '%s'", peerIp);
        Stop();
        return false;
    }
    g_peer.sin_addr.s_addr = addr;

    g_stop.store(false, std::memory_order_release);
    g_running.store(true, std::memory_order_release);
    g_recvThread = std::thread(&RecvThreadMain);
    mod::Log("PaletteSocket: started peer=%s:%u localPort=%u",
             peerIp, static_cast<unsigned>(port), static_cast<unsigned>(port));
    return true;
}

void Stop()
{
    g_stop.store(true, std::memory_order_release);
    if (g_sock != INVALID_SOCKET)
    {
        ::closesocket(g_sock);   // unblocks recvfrom
        g_sock = INVALID_SOCKET;
    }
    if (g_recvThread.joinable())
    {
        g_recvThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_inbound.clear();
    }
    if (g_wsaStarted)
    {
        WSACleanup();
        g_wsaStarted = false;
    }
    g_running.store(false, std::memory_order_release);
}

bool IsRunning()
{
    return g_running.load(std::memory_order_acquire);
}

bool SendSink(const std::uint8_t* datagram, std::size_t len, void* /*user*/)
{
    if (!g_running.load(std::memory_order_acquire)
        || g_sock == INVALID_SOCKET || datagram == nullptr || len == 0)
    {
        return false;
    }
    const int sent = ::sendto(
        g_sock, reinterpret_cast<const char*>(datagram), static_cast<int>(len),
        0, reinterpret_cast<const sockaddr*>(&g_peer), sizeof(g_peer));
    if (sent == SOCKET_ERROR)
    {
        mod::Log("PaletteSocket: sendto failed err=%d", WSAGetLastError());
        return false;
    }
    static unsigned s_tx = 0;
    if (++s_tx <= 3u || (s_tx % 60u) == 0u)
    {
        mod::Log("PaletteSocket: sent #%u %dB", s_tx, sent);
    }
    return true;
}

std::size_t Poll(OverlayChannel& channel)
{
    std::vector<std::vector<std::uint8_t>> drained;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (g_inbound.empty())
        {
            return 0;
        }
        drained.swap(g_inbound);
    }
    for (const auto& d : drained)
    {
        channel.OnInboundDatagram(d.data(), d.size());
    }
    return drained.size();
}
} // namespace netplay::interop::socket_transport
