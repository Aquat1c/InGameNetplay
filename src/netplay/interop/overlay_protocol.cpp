#include "netplay/interop/overlay_protocol.h"

#include <cstring>

namespace netplay::interop::protocol
{
namespace
{
// Common envelope + header writer. Returns total datagram size or 0.
std::size_t BuildFrame(Kind kind, const void* body, std::uint16_t bodyLen,
                       std::uint8_t* out, std::size_t cap)
{
    const std::size_t total =
        kEnvelopeBytes + sizeof(FrameHeader) + bodyLen;
    if (out == nullptr || cap < total || total > kMaxFrameBytes)
    {
        return 0;
    }
    out[0] = kEnvelopeFlagPlain;
    out[1] = kEnvelopeTypeId;

    FrameHeader hdr;
    hdr.magic = kOverlayMagic;
    hdr.version = kProtocolVersion;
    hdr.kind = static_cast<std::uint8_t>(kind);
    hdr.length = bodyLen;
    std::memcpy(out + kEnvelopeBytes, &hdr, sizeof(hdr));

    if (bodyLen != 0 && body != nullptr)
    {
        std::memcpy(out + kEnvelopeBytes + sizeof(FrameHeader), body, bodyLen);
    }
    return total;
}

// Validate envelope + header of an inbound datagram and return a pointer to the
// body (and its declared length) when everything is consistent for `expected`.
const std::uint8_t* ValidateFrame(const std::uint8_t* datagram, std::size_t len,
                                  Kind expected, std::uint16_t expectBodyLen)
{
    if (!LooksLikeOverlayFrame(datagram, len))
    {
        return nullptr;
    }
    FrameHeader hdr;
    std::memcpy(&hdr, datagram + kEnvelopeBytes, sizeof(hdr));
    if (hdr.version != kProtocolVersion
        || hdr.kind != static_cast<std::uint8_t>(expected))
    {
        return nullptr;
    }
    const std::size_t need =
        kEnvelopeBytes + sizeof(FrameHeader) + hdr.length;
    if (len < need || hdr.length != expectBodyLen)
    {
        return nullptr;
    }
    return datagram + kEnvelopeBytes + sizeof(FrameHeader);
}
} // namespace

std::size_t BuildHello(std::uint32_t featureBits, std::uint32_t nonce,
                       std::uint8_t* out, std::size_t cap)
{
    HelloBody b;
    b.featureBits = featureBits;
    b.nonce = nonce;
    return BuildFrame(Kind::Hello, &b, sizeof(b), out, cap);
}

std::size_t BuildAck(std::uint32_t featureBits, std::uint32_t nonce,
                     std::uint8_t* out, std::size_t cap)
{
    HelloBody b;
    b.featureBits = featureBits;
    b.nonce = nonce;
    return BuildFrame(Kind::Ack, &b, sizeof(b), out, cap);
}

std::size_t BuildPaletteBlob(const PaletteBlobBody& body,
                             std::uint8_t* out, std::size_t cap)
{
    return BuildFrame(Kind::PaletteBlob, &body, sizeof(body), out, cap);
}

bool LooksLikeOverlayFrame(const std::uint8_t* datagram, std::size_t len)
{
    if (datagram == nullptr
        || len < kEnvelopeBytes + sizeof(FrameHeader))
    {
        return false;
    }
    if (datagram[1] != kEnvelopeTypeId)
    {
        return false;
    }
    std::uint32_t magic = 0;
    std::memcpy(&magic, datagram + kEnvelopeBytes, sizeof(magic));
    return magic == kOverlayMagic;
}

bool ParseKind(const std::uint8_t* datagram, std::size_t len, Kind* outKind)
{
    if (outKind == nullptr || !LooksLikeOverlayFrame(datagram, len))
    {
        return false;
    }
    FrameHeader hdr;
    std::memcpy(&hdr, datagram + kEnvelopeBytes, sizeof(hdr));
    if (hdr.version != kProtocolVersion)
    {
        return false;
    }
    const std::size_t need =
        kEnvelopeBytes + sizeof(FrameHeader) + hdr.length;
    if (len < need)
    {
        return false;
    }
    *outKind = static_cast<Kind>(hdr.kind);
    return true;
}

bool ParseHello(const std::uint8_t* datagram, std::size_t len, HelloBody* out)
{
    const std::uint8_t* body =
        ValidateFrame(datagram, len, Kind::Hello, sizeof(HelloBody));
    if (body == nullptr || out == nullptr)
    {
        return false;
    }
    std::memcpy(out, body, sizeof(HelloBody));
    return true;
}

bool ParseAck(const std::uint8_t* datagram, std::size_t len, HelloBody* out)
{
    const std::uint8_t* body =
        ValidateFrame(datagram, len, Kind::Ack, sizeof(HelloBody));
    if (body == nullptr || out == nullptr)
    {
        return false;
    }
    std::memcpy(out, body, sizeof(HelloBody));
    return true;
}

bool ParsePaletteBlob(const std::uint8_t* datagram, std::size_t len,
                      PaletteBlobBody* out)
{
    const std::uint8_t* body =
        ValidateFrame(datagram, len, Kind::PaletteBlob, sizeof(PaletteBlobBody));
    if (body == nullptr || out == nullptr)
    {
        return false;
    }
    std::memcpy(out, body, sizeof(PaletteBlobBody));
    if (out->rawLen > kPaletteRawBytes)
    {
        return false;
    }
    return true;
}
} // namespace netplay::interop::protocol
