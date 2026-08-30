#pragma once
// Cross-process shared-memory transport for the mod-interop overlay channel.
//
// The mod DLL is injected into BOTH processes: efz.exe (the game, which runs the
// OverlayChannel + palette apply) and EfzRevival.exe (the helper, which owns the
// entire UDP networking stack). This block lets the two halves exchange overlay
// frames over the helper's OWN already-punched socket, so the feature needs no
// second port, no manual peer IP, and no additional NAT traversal.
//
// It is intentionally DECOUPLED from the takeover's SharedBlock (that block is
// load-bearing for the Revival takeover; this one is best-effort cosmetic and
// must never be able to disturb it). It has its own named file mapping.
//
// Two lock-free single-producer/single-consumer rings:
//   toGame   : helper -> game. The helper's WSARecvFrom/GetQueuedCompletionStatus
//              hooks OBSERVE inbound datagrams and copy overlay frames here; the
//              game's poll drains them into OverlayChannel::OnInboundDatagram.
//   toHelper : game -> helper. The game pushes outbound overlay frames here; the
//              helper's WSASendTo hook drains them and sends each on the same
//              socket + peer address it just used for Revival's own traffic.
//
// SPSC discipline: each ring has exactly one producer process and one consumer
// process, so head/tail need no lock - only correct store ordering (publish the
// slot bytes BEFORE advancing the index). x86 total-store-order + a compiler
// barrier is sufficient; we additionally use volatile indices.

#include <cstdint>

namespace netplay::interop::ipc
{
// Frame slots are sized to the overlay protocol's max frame (142 B) rounded up.
constexpr std::uint32_t kSlotBytes = 160u;
constexpr std::uint32_t kRingSlots = 64u;   // power of two for cheap masking
constexpr std::uint32_t kRingMask  = kRingSlots - 1u;
constexpr std::uint32_t kBlockMagic = 0x4F49504Fu;   // 'OIPO'
constexpr std::uint32_t kBlockVersion = 3u;

// helperIatMask bits (which imports the helper actually patched).
constexpr std::uint32_t kIatWSARecvFrom = 1u << 0;
constexpr std::uint32_t kIatWSASendTo   = 1u << 1;
constexpr std::uint32_t kIatGQCS        = 1u << 2;

// helperInstallReason: only the helper writes it, and only AFTER a successful
// ipc::Attach (pre-attach failures can't reach the shared block, so they surface
// as the game's default 0). Lets us tell "Install never ran / attach failed" (0)
// apart from "ran + attached but IAT patch found nothing" (2).
constexpr std::uint32_t kReasonUnknown      = 0u;   // helper never wrote (didn't run / attach failed)
constexpr std::uint32_t kReasonInstalledOk  = 1u;
constexpr std::uint32_t kReasonNoImports    = 2u;   // attached, but 0 IAT slots patched

struct Slot
{
    std::uint32_t len;                 // valid bytes in data (0 == empty)
    std::uint8_t  data[kSlotBytes];
};

struct Ring
{
    volatile std::uint32_t head;       // producer publishes here (writes then ++)
    volatile std::uint32_t tail;       // consumer advances here after reading
    std::uint32_t          pad[2];     // keep slots 16-byte aligned
    Slot                   slots[kRingSlots];
};

struct OverlayIpcBlock
{
    std::uint32_t magic;               // kBlockMagic once initialized
    std::uint32_t version;             // kBlockVersion
    volatile std::uint32_t helperAttached;  // 1 while the helper side is live
    volatile std::uint32_t gameAttached;    // 1 while the game side is live

    // Peer endpoint the helper captured from Revival's own WSASendTo (network
    // byte order). socketValid flips to 1 once the helper has a socket + peer to
    // send on; until then the game may still queue into toHelper harmlessly.
    volatile std::uint32_t peerAddrBE;      // IPv4, network order
    volatile std::uint16_t peerPortBE;      // network order
    volatile std::uint16_t reserved0;
    volatile std::uint32_t socketValid;

    // Helper-side diagnostics: the helper writes these, the game reads and logs
    // them. This is the only visibility we have into the helper hooks, since the
    // helper process cannot write the game-held log file. Purely observational.
    volatile std::uint32_t helperInstalled;   // 1 once the helper hooks installed
    volatile std::uint32_t helperIatMask;     // kIat* bits actually patched
    volatile std::uint32_t rxObserved;        // overlay frames copied out of RX
    volatile std::uint32_t txFlushed;         // overlay frames sent on TX
    volatile std::uint32_t sendToSeen;        // Revival WSASendTo calls observed
    volatile std::uint32_t recvCompletions;   // recv completions observed via GQCS
    volatile std::uint32_t helperInstallReason;  // kReason* (helper writes)

    Ring toGame;     // helper -> game (RX)
    Ring toHelper;   // game -> helper (TX)
};

// Create-or-open the shared mapping. Idempotent; safe to call from either
// process. `asHelper` marks which attached-flag to set. Returns true on success.
bool Attach(bool asHelper);
void Detach(bool asHelper);
bool IsAttached();
OverlayIpcBlock* Block();   // nullptr until Attach() succeeds

// SPSC ring ops. Push returns false if the ring is full (frame dropped - the
// channel tolerates loss). Pop returns the frame length written to `out`, or 0
// if the ring is empty. `cap` bounds the copy; oversize frames are skipped.
bool Push(Ring& ring, const std::uint8_t* data, std::uint32_t len);
std::uint32_t Pop(Ring& ring, std::uint8_t* out, std::uint32_t cap);
} // namespace netplay::interop::ipc
