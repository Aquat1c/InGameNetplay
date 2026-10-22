#pragma once
// Transport-agnostic coordinator (Hello/Ack handshake, palette cache, outbound queue); inert unless armed.

#include <cstddef>
#include <cstdint>

#include "netplay/interop/overlay_protocol.h"
#include "netplay/interop/palette_store.h"

namespace netplay::interop
{
enum class HandshakeState : std::uint8_t
{
    Idle = 0,       // no session / flag off
    Probing,        // sent Hello(s), awaiting Ack
    Confirmed,      // peer is a modded client (Ack seen) - full exchange live
    VanillaPeer,    // probes exhausted with no Ack - go silent for the session
};

using SendSink = bool (*)(const std::uint8_t* datagram, std::size_t len,
                          void* user);

class OverlayChannel
{
public:
    static OverlayChannel& Instance();

    // localSide: 0=P1, 1=P2, -1=spectator; no-op with the feature flag off.
    void Begin(int localSide);
    void End();
    bool IsActive() const { return m_active; }
    HandshakeState State() const { return m_state; }

    void SetSendSink(SendSink sink, void* user);

    void Tick(std::uint32_t nowMs);

    // Caller swallows the datagram either way to keep it out of Revival's parser.
    bool OnInboundDatagram(const std::uint8_t* datagram, std::size_t len);

    void SubmitLocalPaletteRow(const protocol::PaletteBlobBody& row);
    bool GetPeerPaletteRow(int side, protocol::PaletteBlobBody* out) const;
    void ResetPaletteExchangeForRematch();

private:
    OverlayChannel() = default;

    bool Enqueue(const std::uint8_t* datagram, std::size_t len);
    void Flush();
    void SendHello(std::uint32_t nowMs);
    void SendAck(std::uint32_t nonce);

    bool           m_active = false;
    int            m_localSide = -1;
    HandshakeState m_state = HandshakeState::Idle;
    std::uint32_t  m_nonce = 0;
    std::uint32_t  m_lastHelloMs = 0;
    std::uint32_t  m_helloCount = 0;
    std::uint32_t  m_lastPaletteSendMs = 0;
    bool           m_localRowPending = false;

    SendSink m_sink = nullptr;
    void*    m_sinkUser = nullptr;

    palette::Store m_palettes;

    std::uint8_t m_scratch[protocol::kMaxFrameBytes] = {};
};

inline constexpr std::uint32_t kHelloIntervalMs = 300u;   // ~3 probes over ~1s
inline constexpr std::uint32_t kMaxHelloProbes = 4u;
inline constexpr std::uint32_t kPaletteSendMinIntervalMs = 250u;
} // namespace netplay::interop
