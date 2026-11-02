#pragma once

#include <cstddef>
#include <cstdint>

#include "netplay/interop/overlay_protocol.h"
#include "netplay/interop/palette_store.h"

namespace netplay::interop
{
enum class HandshakeState : std::uint8_t
{
    Idle = 0,
    Probing,
    Confirmed,
    VanillaPeer,
};

using SendSink = bool (*)(const std::uint8_t* datagram, std::size_t len,
                          void* user);

class OverlayChannel
{
public:
    static OverlayChannel& Instance();

    // localSide: 0=P1, 1=P2, -1=spectator.
    void Begin(int localSide);
    void End();
    bool IsActive() const { return m_active; }
    HandshakeState State() const { return m_state; }

    void SetSendSink(SendSink sink, void* user);

    void Tick(std::uint32_t nowMs);

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
    void LogRejectedFrame(const char* reason, std::size_t len);

    bool           m_active = false;
    int            m_localSide = -1;
    HandshakeState m_state = HandshakeState::Idle;
    std::uint32_t  m_nonce = 0;
    std::uint32_t  m_lastHelloMs = 0;
    std::uint32_t  m_helloCount = 0;
    std::uint32_t  m_lastPaletteSendMs = 0;
    bool           m_localRowPending = false;
    std::uint32_t  m_lastPaletteLogMs[2] = {};
    std::uint32_t  m_lastRejectLogMs = 0;
    std::uint32_t  m_rejectedFrames = 0;

    SendSink m_sink = nullptr;
    void*    m_sinkUser = nullptr;

    palette::Store m_palettes;

    std::uint8_t m_scratch[protocol::kMaxFrameBytes] = {};
};

inline constexpr std::uint32_t kHelloIntervalMs = 300u;
inline constexpr std::uint32_t kMaxHelloProbes = 4u;
inline constexpr std::uint32_t kPaletteSendMinIntervalMs = 250u;
}
