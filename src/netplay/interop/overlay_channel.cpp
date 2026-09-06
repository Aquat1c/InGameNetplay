#include "netplay/interop/overlay_channel.h"

#include <windows.h>

#include "logger.h"
#include "netplay/core/mod_settings.h"

namespace netplay::interop
{
OverlayChannel& OverlayChannel::Instance()
{
    static OverlayChannel s_instance;
    return s_instance;
}

void OverlayChannel::Begin(int localSide)
{
    if (!netplay::mod_settings::AreOnlineCustomColorsEnabled())
    {
        // Master flag off: whole subsystem inert. Also disarm defensively so a
        // flag toggled off mid-life followed by a re-Begin cannot leave a stale
        // armed session running (End() is a no-op when already inactive).
        End();
        return;
    }
    m_active = true;
    m_localSide = (localSide == 0 || localSide == 1) ? localSide : -1;
    m_state = HandshakeState::Probing;
    m_nonce = static_cast<std::uint32_t>(GetTickCount()) ^ 0x9E3779B9u;
    m_lastHelloMs = 0;
    m_helloCount = 0;
    m_lastPaletteSendMs = 0;
    m_localRowPending = false;
    m_palettes.Clear();
    mod::Log("OverlayChannel: begin localSide=%d nonce=0x%08lX",
             m_localSide, static_cast<unsigned long>(m_nonce));
}

void OverlayChannel::End()
{
    if (!m_active)
    {
        return;
    }
    mod::Log("OverlayChannel: end state=%d helloCount=%lu",
             static_cast<int>(m_state),
             static_cast<unsigned long>(m_helloCount));
    m_active = false;
    m_state = HandshakeState::Idle;
    m_palettes.Clear();
}

void OverlayChannel::SetSendSink(SendSink sink, void* user)
{
    m_sink = sink;
    m_sinkUser = user;
}

bool OverlayChannel::Enqueue(const std::uint8_t* datagram, std::size_t len)
{
    // Single-frame emit model: hand straight to the sink. If no sink is
    // installed yet (socket layer not wired), the frame is dropped - inert and
    // safe. Returns whether the frame was actually accepted by a sink. A real
    // ring can replace this if batching is ever needed.
    if (m_sink != nullptr && datagram != nullptr && len != 0)
    {
        return m_sink(datagram, len, m_sinkUser);
    }
    return false;
}

void OverlayChannel::Flush()
{
    // Reserved for a future batched queue; single-frame model flushes inline.
}

void OverlayChannel::SendHello(std::uint32_t nowMs)
{
    // Throttle the retry cadence regardless of whether a sink exists, so a
    // wired-but-quiet channel does not spin. Only advance the probe budget on
    // an ACTUAL send - otherwise a channel with no transport installed would
    // falsely "time out" to VanillaPeer without ever emitting a Hello. Bias a
    // literal-0 tick to 1 so the due-check's "0 == never sent" sentinel is not
    // spoofed by a real GetTickCount()==0 (the 1ms window every ~49.7 days).
    m_lastHelloMs = (nowMs != 0u) ? nowMs : 1u;
    const std::size_t n = protocol::BuildHello(
        protocol::kFeaturePalettes, m_nonce, m_scratch, sizeof(m_scratch));
    if (n != 0 && Enqueue(m_scratch, n))
    {
        ++m_helloCount;
    }
}

void OverlayChannel::SendAck(std::uint32_t nonce)
{
    const std::size_t n = protocol::BuildAck(
        protocol::kFeaturePalettes, nonce, m_scratch, sizeof(m_scratch));
    if (n != 0)
    {
        Enqueue(m_scratch, n);
    }
}

void OverlayChannel::Tick(std::uint32_t nowMs)
{
    if (!m_active)
    {
        return;
    }
    if (!netplay::mod_settings::AreOnlineCustomColorsEnabled())
    {
        // Master flag flipped off mid-session (e.g. via the debug settings):
        // quiesce the whole subsystem now rather than at the next session.
        End();
        return;
    }
    if (m_state == HandshakeState::Probing)
    {
        const bool due = (m_lastHelloMs == 0)
            || (nowMs - m_lastHelloMs) >= kHelloIntervalMs;
        if (due)
        {
            if (m_helloCount >= kMaxHelloProbes)
            {
                // No Ack after the probe budget: treat the peer as vanilla and
                // go permanently silent for this session (never spray).
                m_state = HandshakeState::VanillaPeer;
                mod::Log("OverlayChannel: peer assumed vanilla (no ack after "
                         "%lu probes) - going silent",
                         static_cast<unsigned long>(m_helloCount));
            }
            else
            {
                SendHello(nowMs);
            }
        }
    }
    // Palette upload is change-driven (SubmitLocalPaletteRow). A spectator that is
    // present through char-select receives each side's row as it is picked; one
    // that only catches the LIVE game later gets its colors from Revival's own
    // native palette sync (loading screen on) - we deliberately do NOT re-stream
    // the char-select history to late joiners.
    (void)nowMs;
}

bool OverlayChannel::OnInboundDatagram(const std::uint8_t* datagram,
                                       std::size_t len)
{
    if (!m_active)
    {
        return false;
    }
    protocol::Kind kind = protocol::Kind::Invalid;
    if (!protocol::ParseKind(datagram, len, &kind))
    {
        // Looked like ours (typeId+magic) but malformed: still swallow it so a
        // corrupt/forged frame can't reach Revival's parser.
        return true;
    }
    switch (kind)
    {
    case protocol::Kind::Hello:
    {
        protocol::HelloBody hello{};
        if (protocol::ParseHello(datagram, len, &hello))
        {
            // A modded peer exists. Confirm and Ack (echo their nonce). Also
            // promotes us out of Probing if we were still sending Hellos.
            SendAck(hello.nonce);
            if (m_state != HandshakeState::Confirmed)
            {
                m_state = HandshakeState::Confirmed;
                mod::Log("OverlayChannel: peer modded (hello feat=0x%08lX) - "
                         "confirmed",
                         static_cast<unsigned long>(hello.featureBits));
            }
        }
        return true;
    }
    case protocol::Kind::Ack:
    {
        protocol::HelloBody ack{};
        if (protocol::ParseAck(datagram, len, &ack) && ack.nonce == m_nonce)
        {
            if (m_state != HandshakeState::Confirmed)
            {
                m_state = HandshakeState::Confirmed;
                mod::Log("OverlayChannel: peer modded (ack) - confirmed");
            }
        }
        return true;
    }
    case protocol::Kind::PaletteBlob:
    {
        protocol::PaletteBlobBody blob{};
        if (protocol::ParsePaletteBlob(datagram, len, &blob))
        {
            if (m_palettes.AcceptInbound(blob))
            {
                mod::Log("OverlayChannel: rx palette side=%u char=%u slot=%u "
                         "src=%u seq=%u",
                         blob.side, blob.charId, blob.colorSlot,
                         blob.sourceFlag, blob.seq);
            }
        }
        return true;
    }
    default:
        return true;   // unknown kind from a future peer: ignore, still consume
    }
}

void OverlayChannel::SubmitLocalPaletteRow(const protocol::PaletteBlobBody& row)
{
    if (!m_active || m_localSide < 0)
    {
        return;   // inactive, or spectator (never advertises a local row)
    }
    protocol::PaletteBlobBody body = row;
    body.side = static_cast<std::uint8_t>(m_localSide);
    // SetLocalRow stamps `body` with the current outbound seq either way, so the
    // pending path can transmit the LATEST row without re-reading the store.
    if (m_palettes.SetLocalRow(&body))
    {
        m_localRowPending = true;
    }
    if (!m_localRowPending)
    {
        return;   // unchanged and nothing pending - nothing to send
    }
    if (m_state != HandshakeState::Confirmed)
    {
        return;   // cache locally, but only transmit to a confirmed modded peer
    }
    // Rate-limit. Char-select colour navigation changes the row many times a
    // second and the 60Hz poll reports every one of them; each send injects a
    // datagram into Revival's OWN netcode socket. Unthrottled this emitted ~300
    // datagrams across a single char-select. Coalesce to at most one frame per
    // kPaletteSendMinIntervalMs and transmit the newest row - the peer's
    // per-side monotonic seq guard simply skips the elided intermediates.
    const std::uint32_t now = static_cast<std::uint32_t>(GetTickCount());
    if (m_lastPaletteSendMs != 0
        && (now - m_lastPaletteSendMs) < kPaletteSendMinIntervalMs)
    {
        return;   // stays pending; goes out once the interval elapses
    }
    const std::size_t n =
        protocol::BuildPaletteBlob(body, m_scratch, sizeof(m_scratch));
    if (n != 0 && Enqueue(m_scratch, n))
    {
        m_lastPaletteSendMs = (now != 0u) ? now : 1u;
        m_localRowPending = false;
    }
}

bool OverlayChannel::GetPeerPaletteRow(int side,
                                       protocol::PaletteBlobBody* out) const
{
    if (!m_active)
    {
        return false;
    }
    return m_palettes.GetRow(side, out);
}

void OverlayChannel::ResetPaletteExchangeForRematch()
{
    if (!m_active)
    {
        return;
    }
    m_palettes.ResetForRematch();
}
} // namespace netplay::interop
