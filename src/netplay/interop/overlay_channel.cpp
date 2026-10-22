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
    if (m_sink != nullptr && datagram != nullptr && len != 0)
    {
        return m_sink(datagram, len, m_sinkUser);
    }
    return false;
}

void OverlayChannel::Flush()
{
}

void OverlayChannel::SendHello(std::uint32_t nowMs)
{
    // Bias a literal-0 tick to 1 so the "never sent" sentinel stays unspoofable.
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
        // Malformed but addressed to us: swallow it so it never reaches
        // Revival's parser.
        return true;
    }
    switch (kind)
    {
    case protocol::Kind::Hello:
    {
        protocol::HelloBody hello{};
        if (protocol::ParseHello(datagram, len, &hello))
        {
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
        return;   // cache locally; transmit only to a confirmed modded peer
    }
    // Coalesce to the newest row per interval; the peer's monotonic seq guard
    // skips the elided intermediates.
    const std::uint32_t now = static_cast<std::uint32_t>(GetTickCount());
    if (m_lastPaletteSendMs != 0
        && (now - m_lastPaletteSendMs) < kPaletteSendMinIntervalMs)
    {
        return;
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
