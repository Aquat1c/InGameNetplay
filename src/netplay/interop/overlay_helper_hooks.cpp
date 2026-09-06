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

// Serializes the helper's ring ops and the recv map: WSARecvFrom / GQCS /
// WSASendTo can all run on multiple asio IOCP worker threads concurrently, so
// the multi-producer (toGame) and multi-consumer (toHelper) sides need a lock.
// The game side stays lock-free (single logical producer/consumer).
CRITICAL_SECTION g_cs;
bool g_csReady = false;

// overlapped -> recv buffer map, populated at WSARecvFrom and read at the
// completion. Small fixed array (asio keeps only a handful of recvs pending);
// the magic check downstream makes a stale/wrong entry harmless anyway.
struct RecvEntry
{
    LPWSAOVERLAPPED ov;
    char*           buf;
    ULONG           len;
};
constexpr int kRecvMapSize = 32;
RecvEntry g_recvMap[kRecvMapSize] = {};

void RecordRecv(LPWSAOVERLAPPED ov, char* buf, ULONG len)
{
    if (ov == nullptr || buf == nullptr) return;
    EnterCriticalSection(&g_cs);
    int free = -1;
    for (int i = 0; i < kRecvMapSize; ++i)
    {
        if (g_recvMap[i].ov == ov) { g_recvMap[i].buf = buf; g_recvMap[i].len = len;
                                     LeaveCriticalSection(&g_cs); return; }
        if (free < 0 && g_recvMap[i].ov == nullptr) free = i;
    }
    if (free >= 0) { g_recvMap[free] = RecvEntry{ov, buf, len}; }
    LeaveCriticalSection(&g_cs);
}

bool LookupRecv(LPOVERLAPPED ov, char** bufOut, ULONG* lenOut)
{
    bool found = false;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < kRecvMapSize; ++i)
    {
        if (g_recvMap[i].ov == reinterpret_cast<LPWSAOVERLAPPED>(ov))
        {
            *bufOut = g_recvMap[i].buf;
            *lenOut = g_recvMap[i].len;
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_cs);
    return found;
}

// Diagnostic counter bump (fields are cross-thread: asio has multiple IOCP
// worker threads). Interlocked so the counts the game logs are not torn.
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

// Copy the recv buffer's bytes out under SEH (POD-only region), then classify +
// enqueue outside the __try (LooksLikeOverlayFrame / Push touch C++ objects).
void ObserveRecvCompletion(LPOVERLAPPED ov, DWORD bytes)
{
    if (bytes < 4u) return;
    char* buf = nullptr; ULONG bufLen = 0;
    if (!LookupRecv(ov, &buf, &bufLen) || buf == nullptr) return;
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

// ---- Multi-peer TX (Gap B1) -----------------------------------------------
// A host fans Revival's traffic out to the client + N spectators, so a queued
// overlay frame must reach EVERY current peer, not just whichever peer the next
// WSASendTo happens to target (which one it lands on is otherwise racy). Track
// the recent distinct destinations Revival sends to (small LRU + last-seen tick)
// and broadcast each drained frame to all FRESH peers. With a single peer (a
// client, or a host with no spectators) the set is {that peer} -> byte-identical
// to the old single-peer flush. Freshness drops departed peers + one-shot setup
// endpoints (STUN); the reserved typeId makes a stray frame harmless anyway.
struct PeerEndpoint
{
    std::uint32_t addrBE;
    std::uint16_t portBE;      // 0 = empty slot
    DWORD         lastSeenTick;
};
constexpr int   kMaxPeers    = 8;
constexpr DWORD kPeerFreshMs  = 5000;   // exclude peers not sent to in this window
PeerEndpoint    g_peers[kMaxPeers] = {};

// Refresh an existing peer or insert it (empty slot, else evict the stalest).
// Caller holds g_cs. Two passes so an empty slot never shadows a later match.
void TouchPeerLocked(std::uint32_t addrBE, std::uint16_t portBE, DWORD now)
{
    for (int i = 0; i < kMaxPeers; ++i)
    {
        if (g_peers[i].portBE == portBE && g_peers[i].addrBE == addrBE)
        {
            g_peers[i].lastSeenTick = now;
            return;
        }
    }
    int evict = 0;
    DWORD evictAge = 0;
    for (int i = 0; i < kMaxPeers; ++i)
    {
        if (g_peers[i].portBE == 0)
        {
            g_peers[i] = PeerEndpoint{addrBE, portBE, now};
            return;
        }
        const DWORD age = now - g_peers[i].lastSeenTick;
        if (age >= evictAge) { evictAge = age; evict = i; }
    }
    g_peers[evict] = PeerEndpoint{addrBE, portBE, now};
}

// Copy the currently-fresh peers into out[] (capacity kMaxPeers). Caller holds
// g_cs. Returns the count.
int SnapshotFreshPeersLocked(PeerEndpoint* out, DWORD now)
{
    int n = 0;
    for (int i = 0; i < kMaxPeers; ++i)
    {
        if (g_peers[i].portBE != 0
            && (now - g_peers[i].lastSeenTick) <= kPeerFreshMs)
        {
            out[n++] = g_peers[i];
        }
    }
    return n;
}

void CapturePeerAndFlush(SOCKET s, const sockaddr_in* sin, int /*tolen*/)
{
    ipc::OverlayIpcBlock* b = ipc::Block();
    if (b == nullptr) return;

    Bump(&b->sendToSeen);
    g_capturedSocket = s;
    const std::uint32_t addrBE = sin->sin_addr.s_addr;
    const std::uint16_t portBE = sin->sin_port;
    // Keep the single-peer fields (most-recent peer) for the game's diagnostics.
    b->peerAddrBE = addrBE;
    b->peerPortBE = portBE;
    b->socketValid = 1u;

    const DWORD now = GetTickCount();
    PeerEndpoint fresh[kMaxPeers];
    int nFresh = 0;
    EnterCriticalSection(&g_cs);
    TouchPeerLocked(addrBE, portBE, now);
    nFresh = SnapshotFreshPeersLocked(fresh, now);
    LeaveCriticalSection(&g_cs);
    if (nFresh <= 0) return;

    // Drain the game's outbound queue; broadcast each frame to every fresh peer
    // on the same socket Revival just used. Synchronous WSASendTo (non-overlapped)
    // so it completes inline, exactly like Revival's own send at 0x59919.
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
            sockaddr_in dst = {};
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = fresh[i].addrBE;
            dst.sin_port = fresh[i].portBE;
            DWORD sent = 0;
            (void)g_origWSASendTo(
                s, &wb, 1, &sent, 0,
                reinterpret_cast<const sockaddr*>(&dst),
                static_cast<int>(sizeof(dst)), nullptr, nullptr);
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

int WSAAPI Hook_WSARecvFrom(SOCKET s, LPWSABUF bufs, DWORD cnt, LPDWORD recvd,
                            LPDWORD flags, struct sockaddr* from, LPINT fromlen,
                            LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    const int r = g_origWSARecvFrom(s, bufs, cnt, recvd, flags, from, fromlen, ov, cr);
    // Only overlapped IOCP recvs reach us via the completion hook.
    if (ov != nullptr && bufs != nullptr && cnt >= 1u)
    {
        RecordRecv(ov, bufs[0].buf, bufs[0].len);
    }
    return r;
}

int WSAAPI Hook_WSASendTo(SOCKET s, LPWSABUF bufs, DWORD cnt, LPDWORD sent,
                          DWORD flags, const struct sockaddr* to, int tolen,
                          LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    const int r = g_origWSASendTo(s, bufs, cnt, sent, flags, to, tolen, ov, cr);
    if (to != nullptr && tolen >= static_cast<int>(sizeof(sockaddr_in))
        && to->sa_family == AF_INET)
    {
        CapturePeerAndFlush(s, reinterpret_cast<const sockaddr_in*>(to), tolen);
    }
    return r;
}

BOOL WINAPI Hook_GQCS(HANDLE port, LPDWORD bytes, PULONG_PTR key,
                      LPOVERLAPPED* ov, DWORD ms)
{
    const BOOL ok = g_origGQCS(port, bytes, key, ov, ms);
    if (ok && ov != nullptr && *ov != nullptr && bytes != nullptr)
    {
        ObserveRecvCompletion(*ov, *bytes);
    }
    return ok;
}

// Patch one named import in `module`'s IAT. Returns true and stores the original
// through origOut on success. Only by-name imports are patched (the winsock data
// calls are imported by name in every supported build).
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

            void** slot = reinterpret_cast<void**>(&iat->u1.Function);
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                return false;
            *origOut = *slot;
            *slot = hook;
            VirtualProtect(slot, sizeof(void*), old, &old);
            return true;
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

    // TX needs WSASendTo; RX needs BOTH WSARecvFrom (buffer map) and GQCS
    // (completion). If nothing patched, back out cleanly.
    if (!a && !b && !c)
    {
        mod::Log("OverlayHelper: no imports patched - install failed");
        // Write the reason into the shared block (still mapped) before detaching,
        // so the game can distinguish this from "Install never ran".
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
    // IAT unpatching is deliberately omitted: teardown races with Revival's live
    // IOCP threads are riskier than leaving inert trampolines that fall through
    // to the originals once the flag/ipc are gone. Just drop the IPC attachment.
    if (ipc::IsAttached()) ipc::Detach(true);
    g_installed = false;
}
} // namespace netplay::interop::helper_hooks
