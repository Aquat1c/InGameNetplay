#include "netplay/interop/palette_store.h"

#include <cstring>

namespace netplay::interop::palette
{
void Store::Clear()
{
    for (int s = 0; s < 2; ++s)
    {
        m_remote[s] = Row{};
        m_local[s] = Row{};
        m_outSeq[s] = 0;
        m_haveLastInSeq[s] = false;
        m_lastInSeq[s] = 0;
    }
}

void Store::ResetForRematch()
{
    Clear();
}

bool Store::SeqNewer(std::uint16_t incoming, std::uint16_t last)
{
    // 16-bit serial arithmetic: newer iff forward distance < 0x8000; equal is not newer.
    return static_cast<std::uint16_t>(incoming - last) != 0
        && static_cast<std::uint16_t>(incoming - last) < 0x8000u;
}

bool Store::AcceptInbound(const protocol::PaletteBlobBody& body)
{
    if (!protocol::IsValidPaletteRow(body))
    {
        return false;
    }
    const int s = body.side;
    if (m_haveLastInSeq[s] && !SeqNewer(body.seq, m_lastInSeq[s]))
    {
        return false;
    }
    m_lastInSeq[s] = body.seq;
    m_haveLastInSeq[s] = true;
    m_remote[s].body = body;
    m_remote[s].valid = true;
    return true;
}

bool Store::GetRow(int side, protocol::PaletteBlobBody* out) const
{
    if (side < 0 || side > 1 || out == nullptr || !m_remote[side].valid)
    {
        return false;
    }
    *out = m_remote[side].body;
    return true;
}

bool Store::SetLocalRow(protocol::PaletteBlobBody* body)
{
    if (body == nullptr || !protocol::IsValidPaletteRow(*body))
    {
        return false;
    }
    const int s = body->side;
    // Compare everything except the seq field (which we own/assign).
    protocol::PaletteBlobBody prev = m_local[s].body;
    protocol::PaletteBlobBody cur = *body;
    prev.seq = 0;
    cur.seq = 0;
    const bool changed = !m_local[s].valid
        || std::memcmp(&prev, &cur, sizeof(cur)) != 0;
    if (!changed)
    {
        body->seq = m_local[s].body.seq;
        return false;
    }
    body->seq = ++m_outSeq[s];
    m_local[s].body = *body;
    m_local[s].valid = true;
    return true;
}
} // namespace netplay::interop::palette
