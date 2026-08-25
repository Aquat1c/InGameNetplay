#pragma once
// Mod-interop overlay protocol - the on-wire framing for peer<->peer cosmetic
// side data carried over EfzRevival's own UDP datagrams.
//
// Design + cross-version verification: see, under the canonical Memorial tree,
// Revival/Netplay/MOD_INTEROP_OVERLAY_CHANNEL_AND_ONLINE_PALETTES.md
//
// A Revival datagram is  [byte0 flag][byte1 typeId][payload...].  Revival's
// receive dispatcher is a bounded switch over typeId 0x00-0x0A with a
// `default` that drops the packet with NO side effect (binary-verified on
// every supported build, e-j incl. both 1.02j). We therefore emit our frames
// with a reserved typeId >= 0x0B so an UNMODDED peer discards them harmlessly,
// while a modded peer's recv stub recognizes them (by the reserved typeId AND
// the 4-byte magic) and consumes them before Revival ever parses them.
//
// This header is pure, allocation-free, and engine-independent so it can be
// unit-tested in isolation. All multi-byte fields are little-endian (x86).

#include <cstddef>
#include <cstdint>

namespace netplay::interop::protocol
{
// --- envelope (bytes the send stub prepends onto our frame) -----------------
// flag byte: 0 = plain (we never compress our own frames).
inline constexpr std::uint8_t kEnvelopeFlagPlain = 0x00u;
// Reserved typeId. 0x00-0x0A are Revival's real handlers; >= 0x0B hits the
// dispatcher's `default: return`. 0x7F is well clear of the used range and of
// any plausible future Revival type.
inline constexpr std::uint8_t kEnvelopeTypeId = 0x7Fu;
inline constexpr std::size_t kEnvelopeBytes = 2u;   // [flag][typeId]

// --- frame header (immediately after the 2-byte envelope) -------------------
// 4-byte magic "EFZM" (bytes 'E','F','Z','M') as a LE u32.
inline constexpr std::uint32_t kOverlayMagic = 0x4D5A4645u;
inline constexpr std::uint8_t kProtocolVersion = 1u;

enum class Kind : std::uint8_t
{
    Invalid     = 0,
    Hello       = 1,   // capability advertisement (handshake open)
    Ack         = 2,   // reply to Hello (handshake confirmed)
    PaletteBlob = 3,   // one side's raw .pal row (char-select exchange)
    // future cosmetic payload kinds append here
};

// Feature bits advertised in Hello/Ack (forward-compatible capability mask).
inline constexpr std::uint32_t kFeaturePalettes = 0x00000001u;

#pragma pack(push, 1)
struct FrameHeader
{
    std::uint32_t magic;    // == kOverlayMagic
    std::uint8_t  version;  // == kProtocolVersion
    std::uint8_t  kind;     // Kind
    std::uint16_t length;   // bytes of body following this header
};

struct HelloBody
{
    std::uint32_t featureBits;  // capabilities this peer supports
    std::uint32_t nonce;        // echoed in the Ack to correlate
};

// Raw .pal row for one side (BME PaletteBlobMsg layoutVer 3 semantics). Carries
// the SOURCE .pal bytes (40 colors x 3 = 120), not the 160-byte engine body -
// the receiver rebuilds the engine payload locally.
inline constexpr std::size_t kPaletteRawBytes = 120u;   // 40 colors * 3 (BGR)
struct PaletteBlobBody
{
    std::uint8_t  side;        // 0 = P1, 1 = P2 (source side)
    std::uint8_t  charId;      // engine char id 0..24
    std::uint8_t  colorSlot;   // 0..5
    std::uint8_t  sourceFlag;  // 0 = .dat (stock), 1 = .pal (custom)
    std::uint8_t  slotByte;    // 1 = apply custom, 0 = force stock
    std::uint8_t  rawLen;      // valid bytes in rawBgr (== kPaletteRawBytes)
    std::uint16_t seq;         // per-side monotonic; receiver drops seq<=last
    std::uint32_t keyDword;    // engine palette key (slot|(src<<16)|(char<<24))
    std::uint8_t  rawBgr[kPaletteRawBytes];
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 8, "FrameHeader must be packed to 8 bytes");
static_assert(sizeof(HelloBody) == 8, "HelloBody must be 8 bytes");
static_assert(sizeof(PaletteBlobBody) == 132, "PaletteBlobBody must be 132 bytes");

// Largest frame we ever emit (envelope + header + biggest body), comfortably
// under Revival's 1022-byte payload cap.
inline constexpr std::size_t kMaxFrameBytes =
    kEnvelopeBytes + sizeof(FrameHeader) + sizeof(PaletteBlobBody);

// --- encode -----------------------------------------------------------------
// Each Build* writes a complete on-wire datagram (envelope + header + body)
// into `out` (capacity `cap`) and returns the byte count, or 0 on bad args /
// insufficient capacity. `out` is ready to hand straight to WSASendTo.
std::size_t BuildHello(std::uint32_t featureBits, std::uint32_t nonce,
                       std::uint8_t* out, std::size_t cap);
std::size_t BuildAck(std::uint32_t featureBits, std::uint32_t nonce,
                     std::uint8_t* out, std::size_t cap);
std::size_t BuildPaletteBlob(const PaletteBlobBody& body,
                             std::uint8_t* out, std::size_t cap);

// --- decode -----------------------------------------------------------------
// Cheap pre-filter for the recv stub: does this raw datagram look like one of
// ours? (reserved typeId + magic). Does NOT validate the body. Safe on any
// length. This is the ONLY check the hot recv path needs to decide whether to
// swallow the packet.
bool LooksLikeOverlayFrame(const std::uint8_t* datagram, std::size_t len);

// Parse a datagram known/expected to be ours. On success sets *outKind and,
// for the matching kind, fills the body out-param. Returns false (and consumes
// nothing) on any inconsistency (bad magic/version/length/kind/body size).
bool ParseKind(const std::uint8_t* datagram, std::size_t len, Kind* outKind);
bool ParseHello(const std::uint8_t* datagram, std::size_t len, HelloBody* out);
bool ParseAck(const std::uint8_t* datagram, std::size_t len, HelloBody* out);
bool ParsePaletteBlob(const std::uint8_t* datagram, std::size_t len,
                      PaletteBlobBody* out);
} // namespace netplay::interop::protocol
