#pragma once
// Cross-process shared-memory transport for the mod-interop overlay channel.
// The mod DLL runs in BOTH processes: efz.exe (game side, OverlayChannel +
// palette apply) and EfzRevival.exe (helper, owns the UDP stack).  Two
// lock-free SPSC rings exchange overlay frames over the helper's own
// already-punched socket.
//
// Ring directions:
//   toGame   : helper -> game (helper RX hooks copy frames in; game poll drains).
//   toHelper : game -> helper (game pushes; helper's WSASendTo hook sends on
//              the same socket + peer address Revival just used).
//
// SPSC: one producer and one consumer process per ring - publish the slot
// bytes BEFORE advancing the index; x86 store order + a compiler barrier
// suffices.  Decoupled from the takeover's SharedBlock: this block is
// best-effort cosmetic and must never disturb the load-bearing takeover IPC.

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

// helperInstallReason: the helper writes it only after a successful Attach.
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

    // Peer endpoint captured from Revival's own WSASendTo (network byte
    // order); queueing into toHelper before socketValid==1 is allowed.
    volatile std::uint32_t peerAddrBE;      // IPv4, network order (0 for IPv6)
    volatile std::uint16_t peerPortBE;      // network order
    volatile std::uint16_t reserved0;
    volatile std::uint32_t socketValid;

    // Helper-side diagnostics: helper writes, game reads/logs (the helper
    // cannot write the game-held log file).
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

// Create-or-open the shared mapping; idempotent, callable from either process.
// `asHelper` selects which attached-flag is set.  Returns true on success.
bool Attach(bool asHelper);
void Detach(bool asHelper);
bool IsAttached();
OverlayIpcBlock* Block();   // nullptr until Attach() succeeds

// Push returns false when the ring is full (frame dropped; loss tolerated).
// Pop returns the frame length, or 0 when the ring is empty; `cap` bounds the
// copy and oversize frames are skipped.
bool Push(Ring& ring, const std::uint8_t* data, std::uint32_t len);
std::uint32_t Pop(Ring& ring, std::uint8_t* out, std::uint32_t cap);
} // namespace netplay::interop::ipc
