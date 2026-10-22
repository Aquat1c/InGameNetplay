#pragma once

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
    void Clear();
    void ResetForRematch();

    bool AcceptInbound(const protocol::PaletteBlobBody& body);

    bool GetRow(int side, protocol::PaletteBlobBody* out) const;

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
