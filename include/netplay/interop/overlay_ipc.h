#pragma once
// SPSC: publish slot bytes before advancing the index. Must never disturb the takeover's SharedBlock.

#include <cstdint>

namespace netplay::interop::ipc
{
constexpr std::uint32_t kSlotBytes = 160u;
constexpr std::uint32_t kRingSlots = 64u;   // power of two for cheap masking
constexpr std::uint32_t kRingMask  = kRingSlots - 1u;
constexpr std::uint32_t kBlockMagic = 0x4F49504Fu;   // 'OIPO'
constexpr std::uint32_t kBlockVersion = 3u;

constexpr std::uint32_t kIatWSARecvFrom = 1u << 0;
constexpr std::uint32_t kIatWSASendTo   = 1u << 1;
constexpr std::uint32_t kIatGQCS        = 1u << 2;

constexpr std::uint32_t kReasonUnknown      = 0u;
constexpr std::uint32_t kReasonInstalledOk  = 1u;
constexpr std::uint32_t kReasonNoImports    = 2u;

struct Slot
{
    std::uint32_t len;
    std::uint8_t  data[kSlotBytes];
};

struct Ring
{
    volatile std::uint32_t head;
    volatile std::uint32_t tail;
    std::uint32_t          pad[2];
    Slot                   slots[kRingSlots];
};

struct OverlayIpcBlock
{
    std::uint32_t magic;
    std::uint32_t version;
    volatile std::uint32_t helperAttached;
    volatile std::uint32_t gameAttached;

    // Network byte order; queueing before socketValid==1 is allowed.
    volatile std::uint32_t peerAddrBE;
    volatile std::uint16_t peerPortBE;
    volatile std::uint16_t reserved0;
    volatile std::uint32_t socketValid;

    // Helper writes, game logs: the helper cannot write the game-held log file.
    volatile std::uint32_t helperInstalled;
    volatile std::uint32_t helperIatMask;
    volatile std::uint32_t rxObserved;
    volatile std::uint32_t txFlushed;
    volatile std::uint32_t sendToSeen;
    volatile std::uint32_t recvCompletions;
    volatile std::uint32_t helperInstallReason;

    Ring toGame;
    Ring toHelper;
};

bool Attach(bool asHelper);
void Detach(bool asHelper);
bool IsAttached();
OverlayIpcBlock* Block();

bool Push(Ring& ring, const std::uint8_t* data, std::uint32_t len);
std::uint32_t Pop(Ring& ring, std::uint8_t* out, std::uint32_t cap);
}
