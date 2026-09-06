#pragma once
// Mod-interop overlay channel - the flag-gated coordinator that sits between
// the mod's game-thread logic and the (later) socket interposition layer.
//
// It owns the handshake state machine (Hello/Ack), the palette exchange cache,
// and the outbound frame queue. It is deliberately TRANSPORT-AGNOSTIC: it never
// touches a socket. The socket layer (a future helper-IAT WSARecvFrom/WSASendTo
// stub) plugs in by (a) setting a send sink and (b) feeding raw inbound
// datagrams via OnInboundDatagram. That keeps the invasive netcode interposition
// physically separate from this pure coordinator and lets everything here be
// unit-tested and shipped inert behind the master flag.
//
// EVERY public entry point is a no-op unless the master flag
// (mod_settings::AreOnlineCustomColorsEnabled) is set AND Begin() has armed a
// session. So compiling/linking this in is safe with the feature off.

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

// The channel calls this to actually transmit a fully-framed datagram (envelope
// + header + body, ready for WSASendTo). Installed by the socket layer; when
// none is set, outbound frames are dropped (still safe/inert).
using SendSink = bool (*)(const std::uint8_t* datagram, std::size_t len,
                          void* user);

class OverlayChannel
{
public:
    static OverlayChannel& Instance();

    // Session lifecycle. Begin() is a no-op when the master flag is off; it is
    // safe to call unconditionally at session start. localSide: 0=P1, 1=P2,
    // -1=spectator (accept both sides, never advertise a local row).
    void Begin(int localSide);
    void End();
    bool IsActive() const { return m_active; }
    HandshakeState State() const { return m_state; }

    // Transport wiring (installed by the socket-interposition layer).
    void SetSendSink(SendSink sink, void* user);

    // Per-frame pump from the title/char-select thread. Drives the handshake
    // (Hello cadence, probe timeout) and, once Confirmed, the palette upload
    // cadence. `nowMs` = GetTickCount(). No-op unless active.
    void Tick(std::uint32_t nowMs);

    // Inbound entry from the recv stub: a raw datagram already identified as
    // ours (protocol::LooksLikeOverlayFrame == true). Returns true if consumed
    // (always true for well-formed ours; the caller swallows it either way to
    // keep it out of Revival's parser). No-op/false unless active.
    bool OnInboundDatagram(const std::uint8_t* datagram, std::size_t len);

    // --- palette feature surface (used by the palette module) ---------------
    // Publish our current local palette row for `side`; queued for send if it
    // changed and the peer is Confirmed. Safe to call every char-select frame.
    void SubmitLocalPaletteRow(const protocol::PaletteBlobBody& row);
    // Latest accepted peer row for a side, if any.
    bool GetPeerPaletteRow(int side, protocol::PaletteBlobBody* out) const;
    // Win/gameplay -> char-select edge: force fresh re-exchange next match.
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
    // A local row changed but the send was throttled: keep it pending so the
    // LATEST value still goes out once the interval elapses (the 60Hz poll only
    // reports "changed" on the transition, so without this the coalesced row
    // would never be transmitted).
    bool           m_localRowPending = false;

    SendSink m_sink = nullptr;
    void*    m_sinkUser = nullptr;

    palette::Store m_palettes;

    // Small outbound staging buffer (single frame at a time; the channel emits
    // one frame per event). Kept here to avoid per-call stack framing.
    std::uint8_t m_scratch[protocol::kMaxFrameBytes] = {};
};

// Handshake tunables (public for tests).
inline constexpr std::uint32_t kHelloIntervalMs = 300u;   // ~3 probes over ~1s
inline constexpr std::uint32_t kMaxHelloProbes = 4u;
inline constexpr std::uint32_t kPaletteSendMinIntervalMs = 250u;
} // namespace netplay::interop
