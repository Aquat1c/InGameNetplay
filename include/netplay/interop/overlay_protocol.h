#pragma once
// Wire: [flag][typeId][FrameHeader][body], little-endian.

#include <cstddef>
#include <cstdint>

namespace netplay::interop::protocol
{
// flag 0 = plain (never compressed).
inline constexpr std::uint8_t kEnvelopeFlagPlain = 0x00u;
// Revival's dispatcher silently drops typeId >= 0x0B.
inline constexpr std::uint8_t kEnvelopeTypeId = 0x7Fu;
inline constexpr std::size_t kEnvelopeBytes = 2u;

// "EFZM" as a LE u32.
inline constexpr std::uint32_t kOverlayMagic = 0x4D5A4645u;
inline constexpr std::uint8_t kProtocolVersion = 1u;

enum class Kind : std::uint8_t
{
    Invalid     = 0,
    Hello       = 1,
    Ack         = 2,
    PaletteBlob = 3,
};

inline constexpr std::uint32_t kFeaturePalettes = 0x00000001u;

#pragma pack(push, 1)
struct FrameHeader
{
    std::uint32_t magic;
    std::uint8_t  version;
    std::uint8_t  kind;
    std::uint16_t length;
};

struct HelloBody
{
    std::uint32_t featureBits;
    std::uint32_t nonce;
};

// Raw .pal bytes, not the 160-byte engine body.
inline constexpr std::size_t kPaletteRawBytes = 120u;
struct PaletteBlobBody
{
    std::uint8_t  side;        // 0 = P1, 1 = P2 (source side)
    std::uint8_t  charId;      // supported engine char id 0..23
    std::uint8_t  colorSlot;   // 0..5
    std::uint8_t  sourceFlag;  // 0 = .dat (stock), 1 = .pal (custom)
    std::uint8_t  slotByte;    // 1 = apply custom, 0 = force stock
    std::uint8_t  rawLen;
    std::uint16_t seq;         // per-side monotonic; receiver drops seq<=last
    std::uint32_t keyDword;    // engine palette key (slot|(src<<16)|(char<<24))
    std::uint8_t  rawBgr[kPaletteRawBytes];
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 8, "FrameHeader must be packed to 8 bytes");
static_assert(sizeof(HelloBody) == 8, "HelloBody must be 8 bytes");
static_assert(sizeof(PaletteBlobBody) == 132, "PaletteBlobBody must be 132 bytes");

// Stays under Revival's 1022-byte payload cap.
inline constexpr std::size_t kMaxFrameBytes =
    kEnvelopeBytes + sizeof(FrameHeader) + sizeof(PaletteBlobBody);

std::size_t BuildHello(std::uint32_t featureBits, std::uint32_t nonce,
                       std::uint8_t* out, std::size_t cap);
std::size_t BuildAck(std::uint32_t featureBits, std::uint32_t nonce,
                     std::uint8_t* out, std::size_t cap);
std::size_t BuildPaletteBlob(const PaletteBlobBody& body,
                             std::uint8_t* out, std::size_t cap);

// Does NOT validate the body.
bool LooksLikeOverlayFrame(const std::uint8_t* datagram, std::size_t len);

bool ParseKind(const std::uint8_t* datagram, std::size_t len, Kind* outKind);
bool ParseHello(const std::uint8_t* datagram, std::size_t len, HelloBody* out);
bool ParseAck(const std::uint8_t* datagram, std::size_t len, HelloBody* out);
bool IsValidPaletteRow(const PaletteBlobBody& row);

bool ParsePaletteBlob(const std::uint8_t* datagram, std::size_t len,
                      PaletteBlobBody* out);
}
