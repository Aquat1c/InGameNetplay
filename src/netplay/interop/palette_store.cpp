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
    // Same as Clear for now: force a fresh re-exchange next char-select. Kept as
    // a distinct entry point so the call site (win->charselect edge) reads
    // intentionally and can diverge later (e.g. keep local, drop remote).
    Clear();
}

bool Store::SeqNewer(std::uint16_t incoming, std::uint16_t last)
{
    // 16-bit wrap-safe "incoming is newer than last": true when the forward
    // distance is in the first half of the space. Equal => not newer (dedupe).
    return static_cast<std::uint16_t>(incoming - last) != 0
        && static_cast<std::uint16_t>(incoming - last) < 0x8000u;
}

bool Store::AcceptInbound(const protocol::PaletteBlobBody& body)
{
    if (body.side > 1)
    {
        return false;
    }
    const int s = body.side;
    if (m_haveLastInSeq[s] && !SeqNewer(body.seq, m_lastInSeq[s]))
    {
        return false;   // stale or duplicate
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
    if (body == nullptr || body->side > 1)
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
        body->seq = m_local[s].body.seq;   // keep prior seq, no re-send
        return false;
    }
    body->seq = ++m_outSeq[s];
    m_local[s].body = *body;
    m_local[s].valid = true;
    return true;
}
} // namespace netplay::interop::palette
