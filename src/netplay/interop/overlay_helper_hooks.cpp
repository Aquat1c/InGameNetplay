#include "netplay/interop/overlay_helper_hooks.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>

#include "logger.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/core/mod_settings.h"
#include "netplay/interop/overlay_ipc.h"
#include "netplay/interop/overlay_protocol.h"

namespace netplay::interop::helper_hooks
{
namespace
{
namespace P = netplay::interop::protocol;

using WSARecvFrom_t = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
                                   struct sockaddr*, LPINT, LPWSAOVERLAPPED,
                                   LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSASendTo_t = int(WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD,
                                 const struct sockaddr*, int, LPWSAOVERLAPPED,
                                 LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using GQCS_t = BOOL(WINAPI*)(HANDLE, LPDWORD, PULONG_PTR, LPOVERLAPPED*, DWORD);

WSARecvFrom_t g_origWSARecvFrom = nullptr;
WSASendTo_t g_origWSASendTo = nullptr;
GQCS_t g_origGQCS = nullptr;

bool g_installed = false;
SOCKET g_capturedSocket = INVALID_SOCKET;

// WSARecvFrom/GQCS/WSASendTo run on IOCP worker threads; g_cs covers the rings and recv map (the game side stays lock-free).
CRITICAL_SECTION g_cs;
bool g_csReady = false;

struct RecvEntry
{
    LPWSAOVERLAPPED ov;
    char*           buf;
    ULONG           len;
};
constexpr int kRecvMapSize = 32;
RecvEntry g_recvMap[kRecvMapSize] = {};
int g_recvEvictNext = 0;

void RecordRecv(LPWSAOVERLAPPED ov, char* buf, ULONG len)
{
    if (ov == nullptr || buf == nullptr) return;
    EnterCriticalSection(&g_cs);
    int slot = -1;
    for (int i = 0; i < kRecvMapSize; ++i)
    {
        if (g_recvMap[i].ov == ov) { slot = i; break; }
        if (slot < 0 && g_recvMap[i].ov == nullptr) slot = i;
    }
    if (slot < 0)
    {
        slot = g_recvEvictNext;
        g_recvEvictNext = (g_recvEvictNext + 1) % kRecvMapSize;
    }
    g_recvMap[slot] = RecvEntry{ov, buf, len};
    LeaveCriticalSection(&g_cs);
}

bool TakeRecv(LPOVERLAPPED ov, char** bufOut, ULONG* lenOut)
{
    bool found = false;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < kRecvMapSize; ++i)
    {
        if (g_recvMap[i].ov == reinterpret_cast<LPWSAOVERLAPPED>(ov))
        {
            *bufOut = g_recvMap[i].buf;
            *lenOut = g_recvMap[i].len;
            g_recvMap[i] = RecvEntry{};
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    return found;
}

inline void Bump(volatile std::uint32_t* p)
{
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(p));
}

void PushToGameLocked(const std::uint8_t* data, std::uint32_t len)
{
    ipc::OverlayIpcBlock* b = ipc::Block();
    if (b == nullptr) return;
    EnterCriticalSection(&g_cs);
    (void)ipc::Push(b->toGame, data, len);
    LeaveCriticalSection(&g_cs);
}

// Copy the recv bytes out under SEH (POD-only region); C++ objects are touched outside the __try.
void ObserveRecvCompletion(LPOVERLAPPED ov, DWORD bytes)
{
    char* buf = nullptr; ULONG bufLen = 0;
    if (!TakeRecv(ov, &buf, &bufLen) || buf == nullptr) return;
    if (bytes < 4u) return;
    if (ipc::Block() != nullptr) Bump(&ipc::Block()->recvCompletions);
    ULONG n = bytes;
    if (n > bufLen) n = bufLen;
    if (n == 0u || n > ipc::kSlotBytes) return;

    std::uint8_t tmp[ipc::kSlotBytes];
    ULONG got = 0;
    __try { std::memcpy(tmp, buf, n); got = n; }
    __except (EXCEPTION_EXECUTE_HANDLER) { got = 0; }
    if (got == 0u) return;

    if (!P::LooksLikeOverlayFrame(tmp, static_cast<std::size_t>(got))) return;
    PushToGameLocked(tmp, static_cast<std::uint32_t>(got));
    if (ipc::Block() != nullptr) Bump(&ipc::Block()->rxObserved);

    static unsigned s_rx = 0;
    if (++s_rx <= 3u || (s_rx % 120u) == 0u)
    {
        mod::Log("OverlayHelper: RX observed #%u %luB -> toGame", s_rx,
                 static_cast<unsigned long>(got));
    }
}

struct PeerEndpoint
{
    sockaddr_in6  addr;        // AF_INET or AF_INET6 (a sockaddr_in fits)
    int           len;         // 0 = empty slot
    DWORD         lastSeenTick;
};
constexpr int   kMaxPeers    = 8;
constexpr DWORD kPeerFreshMs  = 5000;   // exclude peers not sent to in this window
PeerEndpoint    g_peers[kMaxPeers] = {};

int EndpointLen(const sockaddr* to)
{
    return to->sa_family == AF_INET6 ? static_cast<int>(sizeof(sockaddr_in6))
                                     : static_cast<int>(sizeof(sockaddr_in));
}

bool SameEndpoint(const sockaddr* a, const sockaddr* b)
{
    if (a->sa_family != b->sa_family) return false;
    if (a->sa_family == AF_INET)
    {
        const auto* x = reinterpret_cast<const sockaddr_in*>(a);
        const auto* y = reinterpret_cast<const sockaddr_in*>(b);
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    const auto* x = reinterpret_cast<const sockaddr_in6*>(a);
    const auto* y = reinterpret_cast<const sockaddr_in6*>(b);
    return x->sin6_port == y->sin6_port && x->sin6_scope_id == y->sin6_scope_id
        && std::memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) == 0;
}

// Caller holds g_cs.
void TouchPeerLocked(const sockaddr* to, DWORD now)
{
    for (int i = 0; i < kMaxPeers; ++i)
    {
        if (g_peers[i].len != 0
            && SameEndpoint(reinterpret_cast<const sockaddr*>(&g_peers[i].addr), to))
        {
            g_peers[i].lastSeenTick = now;
            return;
        }
    }
    int evict = 0;
    DWORD evictAge = 0;
    for (int i = 0; i < kMaxPeers; ++i)
    {
        if (g_peers[i].len == 0)
        {
            evict = i;
            break;
        }
        const DWORD age = now - g_peers[i].lastSeenTick;
        if (age >= evictAge) { evictAge = age; evict = i; }
    }
    PeerEndpoint& p = g_peers[evict];
    p = PeerEndpoint{};
    p.len = EndpointLen(to);
    std::memcpy(&p.addr, to, static_cast<std::size_t>(p.len));
    p.lastSeenTick = now;
}

// Caller holds g_cs.
int SnapshotFreshPeersLocked(PeerEndpoint* out, DWORD now)
{
    int n = 0;
    for (int i = 0; i < kMaxPeers; ++i)
    {
        if (g_peers[i].len != 0
            && (now - g_peers[i].lastSeenTick) <= kPeerFreshMs)
        {
            out[n++] = g_peers[i];
        }
    }
    return n;
}

void CapturePeerAndFlush(SOCKET s, const sockaddr* to)
{
    ipc::OverlayIpcBlock* b = ipc::Block();
    if (b == nullptr) return;

    Bump(&b->sendToSeen);
    g_capturedSocket = s;
    const auto* sin = reinterpret_cast<const sockaddr_in*>(to);
    b->peerAddrBE = to->sa_family == AF_INET ? sin->sin_addr.s_addr : 0u;
    b->peerPortBE = sin->sin_port;
    b->socketValid = 1u;

    const DWORD now = GetTickCount();
    PeerEndpoint fresh[kMaxPeers];
    int nFresh = 0;
    EnterCriticalSection(&g_cs);
    TouchPeerLocked(to, now);
    nFresh = SnapshotFreshPeersLocked(fresh, now);
    LeaveCriticalSection(&g_cs);
    if (nFresh <= 0) return;

    int framesSent = 0;
    for (;;)
    {
        std::uint8_t frame[ipc::kSlotBytes];
        std::uint32_t len = 0;
        EnterCriticalSection(&g_cs);
        len = ipc::Pop(b->toHelper, frame, sizeof(frame));
        LeaveCriticalSection(&g_cs);
        if (len == 0u) break;

        WSABUF wb;
        wb.buf = reinterpret_cast<char*>(frame);
        wb.len = len;
        for (int i = 0; i < nFresh; ++i)
        {
            if (fresh[i].addr.sin6_family != to->sa_family) continue;
            DWORD sent = 0;
            (void)g_origWSASendTo(
                s, &wb, 1, &sent, 0,
                reinterpret_cast<const sockaddr*>(&fresh[i].addr),
                fresh[i].len, nullptr, nullptr);
            Bump(&b->txFlushed);
        }
        ++framesSent;
    }

    if (framesSent > 0 && nFresh > 1)
    {
        static unsigned s_bc = 0;
        if (++s_bc <= 3u || (s_bc % 60u) == 0u)
        {
            mod::Log("OverlayHelper: TX broadcast %d frame(s) x %d fresh peers",
                     framesSent, nFresh);
        }
    }
}

// Preserve last-error across hook work; asio reads it immediately after each call.
int WSAAPI Hook_WSARecvFrom(SOCKET s, LPWSABUF bufs, DWORD cnt, LPDWORD recvd,
                            LPDWORD flags, struct sockaddr* from, LPINT fromlen,
                            LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    // Record before the call: its completion can be dequeued first.
    if (ov != nullptr && bufs != nullptr && cnt >= 1u)
    {
        RecordRecv(ov, bufs[0].buf, bufs[0].len);
    }
    return g_origWSARecvFrom(s, bufs, cnt, recvd, flags, from, fromlen, ov, cr);
}

int WSAAPI Hook_WSASendTo(SOCKET s, LPWSABUF bufs, DWORD cnt, LPDWORD sent,
                          DWORD flags, const struct sockaddr* to, int tolen,
                          LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    const int r = g_origWSASendTo(s, bufs, cnt, sent, flags, to, tolen, ov, cr);
    const DWORD err = GetLastError();
    if (to != nullptr && tolen >= static_cast<int>(sizeof(sockaddr_in))
        && (to->sa_family == AF_INET
            || (to->sa_family == AF_INET6
                && tolen >= static_cast<int>(sizeof(sockaddr_in6)))))
    {
        CapturePeerAndFlush(s, to);
    }
    SetLastError(err);
    return r;
}

BOOL WINAPI Hook_GQCS(HANDLE port, LPDWORD bytes, PULONG_PTR key,
                      LPOVERLAPPED* ov, DWORD ms)
{
    const BOOL ok = g_origGQCS(port, bytes, key, ov, ms);
    const DWORD err = GetLastError();
    if (ov != nullptr && *ov != nullptr)
    {
        ObserveRecvCompletion(*ov, (ok && bytes != nullptr) ? *bytes : 0u);
    }
    SetLastError(err);
    return ok;
}

bool SwapIatSlot(void** slot, void* hook, void** origOut)
{
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        {
            return false;
        }
        bool written = false;
        __try
        {
            // Store the original BEFORE the hook goes live (Revival's threads call through the slot concurrently).
            *origOut = *slot;
            (void)InterlockedExchangePointer(slot, hook);
            written = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            written = false;
        }
        VirtualProtect(slot, sizeof(void*), old, &old);
        if (written)
        {
            return true;
        }
        Sleep(1);
    }
    return false;
}

bool PatchIatImport(HMODULE module, const char* dllName, const char* funcName,
                    void* hook, void** origOut)
{
    auto base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const DWORD importDir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (importDir == 0) return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir);
    for (; desc->Name != 0; ++desc)
    {
        const char* name = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(name, dllName) != 0) continue;
        if (desc->OriginalFirstThunk == 0) continue;

        auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; orig->u1.AddressOfData != 0; ++orig, ++iat)
        {
            if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + orig->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(byName->Name), funcName) != 0)
                continue;

            return SwapIatSlot(reinterpret_cast<void**>(&iat->u1.Function),
                               hook, origOut);
        }
    }
    return false;
}
} // namespace

bool Install()
{
    if (g_installed) return true;
    if (!netplay::mod_settings::AreOnlineCustomColorsEnabled()) return false;
    if (!netplay::bridge::IsCurrentProcessRevival()) return false;   // helper only

    if (!ipc::Attach(/*asHelper=*/true))
    {
        mod::Log("OverlayHelper: IPC attach failed - install skipped");
        return false;
    }

    if (!g_csReady) { InitializeCriticalSection(&g_cs); g_csReady = true; }

    HMODULE exe = GetModuleHandleA(nullptr);   // EfzRevival.exe
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    const bool a = PatchIatImport(exe, "ws2_32.dll", "WSARecvFrom",
                                  reinterpret_cast<void*>(&Hook_WSARecvFrom),
                                  reinterpret_cast<void**>(&g_origWSARecvFrom));
    const bool b = PatchIatImport(exe, "ws2_32.dll", "WSASendTo",
                                  reinterpret_cast<void*>(&Hook_WSASendTo),
                                  reinterpret_cast<void**>(&g_origWSASendTo));
    const bool c = PatchIatImport(exe, "kernel32.dll", "GetQueuedCompletionStatus",
                                  reinterpret_cast<void*>(&Hook_GQCS),
                                  reinterpret_cast<void**>(&g_origGQCS));
    (void)k32;

    mod::Log("OverlayHelper: IAT patch WSARecvFrom=%d WSASendTo=%d GQCS=%d",
             a ? 1 : 0, b ? 1 : 0, c ? 1 : 0);

    if (!a && !b && !c)
    {
        mod::Log("OverlayHelper: no imports patched - install failed");
        if (ipc::OverlayIpcBlock* blk = ipc::Block())
            blk->helperInstallReason = ipc::kReasonNoImports;
        ipc::Detach(true);
        return false;
    }

    if (ipc::OverlayIpcBlock* blk = ipc::Block())
    {
        blk->helperIatMask = (a ? ipc::kIatWSARecvFrom : 0u)
                           | (b ? ipc::kIatWSASendTo : 0u)
                           | (c ? ipc::kIatGQCS : 0u);
        blk->helperInstallReason = ipc::kReasonInstalledOk;
        blk->helperInstalled = 1u;   // published LAST so the game sees a full record
    }

    g_installed = true;
    mod::Log("OverlayHelper: installed (RX observe-only, TX piggyback)");
    return true;
}

void Uninstall()
{
    // IAT unpatching is omitted: racing Revival's live IOCP threads is riskier
    // than leaving inert trampolines.
    if (ipc::IsAttached()) ipc::Detach(true);
    g_installed = false;
}
} // namespace netplay::interop::helper_hooks
