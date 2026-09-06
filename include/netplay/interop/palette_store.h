#pragma once
// Per-match palette exchange cache for the overlay channel. Pure state: holds
// the latest accepted raw-.pal row for each side, enforces per-side monotonic
// sequencing (UDP reorders; a stale CLEAR must not wipe a newer custom row),
// and tracks the local row so we only re-broadcast on change. Thread-affinity:
// intended to be driven from the game (title) thread; the recv path hands rows
// in via AcceptInbound. No engine or socket dependencies.

#include <cstdint>

#include "netplay/interop/overlay_protocol.h"

namespace netplay::interop::palette
{
struct Row
{
    bool     valid = false;
    protocol::PaletteBlobBody body{};
};

class Store
{
public:
    // Drop everything (session end). ResetForRematch keeps nothing either but
    // is named for the win/gameplay -> char-select edge so a stale row from the
    // prior match cannot bleed into the next one.
    void Clear();
    void ResetForRematch();

    // Inbound peer row. Returns true iff accepted (newer seq for that side),
    // meaning a fresh apply is warranted. Rejects side>1 and stale/duplicate
    // seq. rawLen is clamp-validated by the protocol parser before this.
    bool AcceptInbound(const protocol::PaletteBlobBody& body);

    // Latest accepted row for a side (0/1). Returns false if none cached.
    bool GetRow(int side, protocol::PaletteBlobBody* out) const;

    // Local outbound row management: set what we currently render for our side,
    // returns true iff it changed vs the last set value (i.e. worth sending).
    // Assigns the next per-side outbound seq into *body.seq on a change.
    bool SetLocalRow(protocol::PaletteBlobBody* body);

private:
    static bool SeqNewer(std::uint16_t incoming, std::uint16_t last);

    Row           m_remote[2];
    Row           m_local[2];
    std::uint16_t m_outSeq[2] = {0, 0};
    bool          m_haveLastInSeq[2] = {false, false};
    std::uint16_t m_lastInSeq[2] = {0, 0};
};
} // namespace netplay::interop::palette
