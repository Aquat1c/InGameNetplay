
#include "netplay/interop/overlay_protocol.h"
#include "netplay/interop/palette_store.h"
#include "netplay/interop/overlay_channel.h"
#include "netplay/interop/palette_source.h"
#include "netplay/interop/palette_remap.h"
#include "netplay/interop/palette_apply.h"
#include "netplay/interop/overlay_ipc.h"
#include "netplay/core/version_compare.h"

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <direct.h>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mod
{
std::vector<std::string> logLines;
void Log(const char* format, ...)
{
    char line[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    logLines.emplace_back(line);
}
}

namespace netplay::mod_settings
{
static bool g_testFlag = false;
bool AreOnlineCustomColorsEnabled() { return g_testFlag; }
}

namespace
{
using namespace netplay::interop;
namespace P = netplay::interop::protocol;

int g_failures = 0;
void Expect(bool cond, const char* name)
{
    if (!cond)
    {
        std::cerr << "FAIL: " << name << "\n";
        ++g_failures;
    }
}

void SetFlag(bool on) { netplay::mod_settings::g_testFlag = on; }

P::PaletteBlobBody MakeRow(std::uint8_t side, std::uint8_t charId,
                           std::uint8_t slot, std::uint16_t seq)
{
    P::PaletteBlobBody b{};
    b.side = side;
    b.charId = charId;
    b.colorSlot = slot;
    b.sourceFlag = 1;
    b.slotByte = 1;
    b.rawLen = P::kPaletteRawBytes;
    b.seq = seq;
    b.keyDword = source::PackKeyDword(charId, b.sourceFlag, slot);
    for (std::size_t i = 0; i < P::kPaletteRawBytes; ++i)
    {
        b.rawBgr[i] = static_cast<std::uint8_t>(i + charId);
    }
    return b;
}

void TestProtocol()
{
    std::uint8_t buf[P::kMaxFrameBytes];

    std::size_t n = P::BuildHello(P::kFeaturePalettes, 0xDEADBEEFu, buf, sizeof buf);
    Expect(n == P::kEnvelopeBytes + sizeof(P::FrameHeader) + sizeof(P::HelloBody),
           "Hello frame size");
    Expect(buf[0] == P::kEnvelopeFlagPlain, "envelope flag byte");
    Expect(buf[1] == P::kEnvelopeTypeId, "envelope reserved typeId 0x7F");
    Expect(P::LooksLikeOverlayFrame(buf, n), "Hello looks-like ours");
    P::Kind k = P::Kind::Invalid;
    Expect(P::ParseKind(buf, n, &k) && k == P::Kind::Hello, "Hello kind");
    P::HelloBody hb{};
    Expect(P::ParseHello(buf, n, &hb), "parse Hello");
    Expect(hb.featureBits == P::kFeaturePalettes && hb.nonce == 0xDEADBEEFu,
           "Hello body fields");

    n = P::BuildAck(P::kFeaturePalettes, 0x1234u, buf, sizeof buf);
    Expect(P::ParseAck(buf, n, &hb) && hb.nonce == 0x1234u, "parse Ack");
    Expect(!P::ParseHello(buf, n, &hb), "Ack must not parse as Hello");

    P::PaletteBlobBody pb = MakeRow(1, 7, 3, 42);
    n = P::BuildPaletteBlob(pb, buf, sizeof buf);
    Expect(n == P::kMaxFrameBytes, "PaletteBlob is the max frame size");
    P::PaletteBlobBody got{};
    Expect(P::ParsePaletteBlob(buf, n, &got), "parse PaletteBlob");
    Expect(got.side == 1 && got.charId == 7 && got.colorSlot == 3
               && got.seq == 42 && got.rawLen == P::kPaletteRawBytes,
           "PaletteBlob header fields");
    Expect(std::memcmp(got.rawBgr, pb.rawBgr, P::kPaletteRawBytes) == 0,
           "PaletteBlob raw bytes preserved");
    Expect(!P::ParsePaletteBlob(buf, n, nullptr),
           "ParsePaletteBlob null out rejected");

    Expect(P::BuildHello(0, 0, nullptr, 64) == 0, "Build null out -> 0");
    Expect(P::BuildHello(0, 0, buf, 0) == 0, "Build zero cap -> 0");
    Expect(P::BuildHello(0, 0, buf,
                         P::kEnvelopeBytes + sizeof(P::FrameHeader)
                             + sizeof(P::HelloBody) - 1) == 0,
           "Build one-under cap -> 0");
    Expect(P::BuildHello(0, 0, buf,
                         P::kEnvelopeBytes + sizeof(P::FrameHeader)
                             + sizeof(P::HelloBody)) != 0,
           "Build exact cap -> ok");

    Expect(!P::LooksLikeOverlayFrame(nullptr, 100), "null datagram");
    Expect(!P::LooksLikeOverlayFrame(buf, 0), "len 0");
    Expect(!P::LooksLikeOverlayFrame(buf, P::kEnvelopeBytes + sizeof(P::FrameHeader) - 1),
           "too short for header");
    std::uint8_t vanilla[32];
    for (int i = 0; i < 32; ++i) vanilla[i] = static_cast<std::uint8_t>(i);
    vanilla[1] = 0x01;  // a real Revival typeId
    Expect(!P::LooksLikeOverlayFrame(vanilla, sizeof vanilla),
           "vanilla typeId not ours");
    std::uint8_t badmagic[64];
    std::memcpy(badmagic, buf, sizeof(P::FrameHeader) + P::kEnvelopeBytes);
    badmagic[P::kEnvelopeBytes] ^= 0xFF;  // corrupt magic byte 0
    Expect(!P::LooksLikeOverlayFrame(badmagic, 64), "right typeId wrong magic");

    n = P::BuildHello(P::kFeaturePalettes, 1u, buf, sizeof buf);
    std::uint8_t tampered[64];
    std::memcpy(tampered, buf, n);
    tampered[P::kEnvelopeBytes + 4] = 0xEE;  // FrameHeader.version
    Expect(P::LooksLikeOverlayFrame(tampered, n), "tampered still looks-like");
    Expect(!P::ParseHello(tampered, n, &hb), "bad version rejected");
    P::Kind k2;
    Expect(!P::ParseKind(tampered, n, &k2), "bad version -> ParseKind false");
    std::memcpy(tampered, buf, n);
    tampered[P::kEnvelopeBytes + 6] = 0xFF;  // FrameHeader.length low = 255
    tampered[P::kEnvelopeBytes + 7] = 0x00;
    Expect(!P::ParseHello(tampered, n, &hb), "declared length > datagram rejected");
    std::memcpy(tampered, buf, n);
    tampered[P::kEnvelopeBytes + 6] =
        static_cast<std::uint8_t>(sizeof(P::PaletteBlobBody));  // 132
    Expect(!P::ParseHello(tampered, n, &hb),
           "Hello with non-Hello length rejected");

    pb = MakeRow(0, 3, 2, 9);
    pb.rawLen = 200;  // > kPaletteRawBytes
    n = P::BuildPaletteBlob(pb, buf, sizeof buf);
    Expect(!P::ParsePaletteBlob(buf, n, &got), "rawLen > 120 rejected");

    n = P::BuildHello(P::kFeaturePalettes, 5u, buf, sizeof buf);
    std::uint8_t padded[128];
    std::memcpy(padded, buf, n);
    std::memset(padded + n, 0xAB, 40);
    Expect(!P::ParseHello(padded, n + 40, &hb),
           "trailing bytes rejected");

    std::memcpy(tampered, buf, n);
    tampered[P::kEnvelopeBytes + 5] = 99;  // FrameHeader.kind
    Expect(P::ParseKind(tampered, n, &k2)
               && static_cast<int>(k2) == 99,
           "unknown kind surfaces via ParseKind");
    Expect(!P::ParseHello(tampered, n, &hb), "unknown kind not a Hello");
}

void TestUntrustedPalettes()
{
    const P::PaletteBlobBody valid = MakeRow(1, 23, 5, 100);
    std::uint8_t frame[256] = {};
    const std::size_t size = P::BuildPaletteBlob(valid, frame, sizeof(frame));
    P::PaletteBlobBody out{};
    for (std::size_t len = 0; len <= sizeof(frame); ++len)
    {
        out = MakeRow(0, 0, 0, 7);
        const auto before = out;
        const bool accepted = P::ParsePaletteBlob(frame, len, &out);
        Expect(accepted == (len == size), "palette requires exact datagram length");
        if (!accepted) Expect(std::memcmp(&out, &before, sizeof(out)) == 0,
                              "rejected frame leaves output unchanged");
    }
    for (unsigned flag = 1; flag <= 255; ++flag)
    {
        frame[0] = static_cast<std::uint8_t>(flag);
        P::Kind kind{};
        Expect(!P::ParsePaletteBlob(frame, size, &out)
                   && !P::ParseKind(frame, size, &kind),
               "non-plain envelope rejected");
    }
    frame[0] = 0;
    for (int field = 0; field < 7; ++field)
    {
        auto bad = valid;
        switch (field)
        {
        case 0: bad.side = 2; break;
        case 1: bad.charId = 24; break;
        case 2: bad.colorSlot = 6; break;
        case 3: bad.sourceFlag = 2; break;
        case 4: bad.slotByte = 2; break;
        case 5: bad.rawLen = 119; break;
        case 6: bad.keyDword ^= 0x100u; break;
        }
        P::BuildPaletteBlob(bad, frame, sizeof(frame));
        out = valid;
        Expect(!P::ParsePaletteBlob(frame, size, &out), "invalid row metadata rejected");
        Expect(std::memcmp(&out, &valid, sizeof(out)) == 0,
               "rejected metadata leaves output unchanged");
        palette::Store store;
        Expect(!store.AcceptInbound(bad), "store rejects invalid metadata before sequence update");
        Expect(store.AcceptInbound(valid), "rejected row cannot poison sequence tracking");
    }
    auto inconsistent = valid;
    inconsistent.slotByte = 0;
    P::BuildPaletteBlob(inconsistent, frame, sizeof(frame));
    Expect(!P::ParsePaletteBlob(frame, size, &out), "custom and stock flags must agree");
    for (unsigned len = 0; len <= 255; ++len)
    {
        auto row = valid;
        row.rawLen = static_cast<std::uint8_t>(len);
        P::BuildPaletteBlob(row, frame, sizeof(frame));
        Expect(P::ParsePaletteBlob(frame, size, &out) == (len == 120),
               "raw color count must be exactly 120 bytes");
    }
    auto stock = source::AssembleRow(0, 23, 5, false, nullptr);
    P::BuildPaletteBlob(stock, frame, sizeof(frame));
    Expect(P::ParsePaletteBlob(frame, size, &out) && out.slotByte == 0,
           "canonical stock clear remains accepted");
}

void TestPaletteWriteBounds()
{
    std::uint32_t random = 0x12345678u;
    for (unsigned charId = 0; charId < 24; ++charId)
    {
        auto row = MakeRow(0, static_cast<std::uint8_t>(charId), 0, 1);
        for (int sample = 0; sample < 64; ++sample)
        {
            for (auto& value : row.rawBgr)
            {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                value = sample == 0 ? 0 : sample == 1 ? 255 : static_cast<std::uint8_t>(random);
            }
            struct {
                std::uint32_t before;
                remap::PortraitColor colors[remap::kMaxPortraitColors];
                std::uint32_t after;
            } mapped;
            std::memset(&mapped, 0xCC, sizeof(mapped));
            const std::size_t cap = sample % 2 == 0 ? 1 : remap::kMaxPortraitColors;
            const auto n = remap::RemapSpriteToPortrait(row.rawBgr,
                source::CharFolderName(charId), mapped.colors, cap);
            Expect(n <= cap && mapped.before == 0xCCCCCCCCu && mapped.after == 0xCCCCCCCCu,
                   "arbitrary colors cannot overrun mapping output");
            for (std::size_t i = 0; i < n; ++i)
                Expect(mapped.colors[i].portIdx >= 1 && mapped.colors[i].portIdx <= 40,
                       "mapping destination stays inside portrait bank");
            if (cap == 1)
            {
                const auto* remaining = reinterpret_cast<const std::uint8_t*>(&mapped.colors[1]);
                bool untouched = true;
                for (std::size_t i = 0; i < sizeof(mapped.colors) - sizeof(mapped.colors[0]); ++i)
                    untouched = untouched && remaining[i] == 0xCC;
                Expect(untouched, "mapping respects caller capacity");
            }
        }
        std::uint8_t gameData[5000];
        std::memset(gameData, 0xCC, sizeof(gameData));
        row.keyDword ^= 0x100u;
        Expect(!apply::ApplyRowToWinScreen(reinterpret_cast<std::uint32_t>(gameData), 0, row),
               "application rejects malformed row even if parser bypassed");
        bool untouched = true;
        for (auto value : gameData) untouched = untouched && value == 0xCC;
        Expect(untouched, "invalid row cannot modify engine palette memory");
    }
}

void TestStore()
{
    palette::Store st;

    P::PaletteBlobBody bad = MakeRow(2, 0, 0, 1);
    Expect(!st.AcceptInbound(bad), "AcceptInbound side>1 rejected");
    P::PaletteBlobBody badLocal = MakeRow(2, 0, 0, 1);
    Expect(!st.SetLocalRow(&badLocal), "SetLocalRow side>1 rejected");

    Expect(st.AcceptInbound(MakeRow(0, 5, 1, 100)), "first inbound accepted");
    Expect(!st.AcceptInbound(MakeRow(0, 5, 1, 100)), "duplicate seq rejected");
    Expect(!st.AcceptInbound(MakeRow(0, 5, 1, 99)), "stale (reordered) rejected");
    Expect(st.AcceptInbound(MakeRow(0, 5, 1, 101)), "newer seq accepted");
    P::PaletteBlobBody out{};
    Expect(st.GetRow(0, &out) && out.seq == 101, "GetRow returns latest");

    Expect(!st.GetRow(1, &out), "other side empty");
    Expect(st.AcceptInbound(MakeRow(1, 8, 2, 1)), "side1 first accepted");
    Expect(st.GetRow(1, &out) && out.charId == 8, "side1 row cached");
    Expect(st.GetRow(0, &out) && out.seq == 101, "side0 unaffected by side1");

    palette::Store w;
    Expect(w.AcceptInbound(MakeRow(0, 1, 0, 65535)), "seq 65535 accepted");
    Expect(w.AcceptInbound(MakeRow(0, 1, 0, 0)), "wrap 65535->0 is newer");
    Expect(!w.AcceptInbound(MakeRow(0, 1, 0, 65535)), "backward across wrap stale");

    Expect(!st.GetRow(-1, &out), "GetRow side -1 rejected");
    Expect(!st.GetRow(2, &out), "GetRow side 2 rejected");
    Expect(!st.GetRow(0, nullptr), "GetRow null out rejected");

    st.Clear();
    Expect(!st.GetRow(0, &out) && !st.GetRow(1, &out), "Clear wipes both sides");
    Expect(st.AcceptInbound(MakeRow(0, 5, 1, 50)), "post-Clear seq resets (any accepted)");
    st.ResetForRematch();
    Expect(!st.GetRow(0, &out), "ResetForRematch wipes");

    palette::Store lo;
    P::PaletteBlobBody L = MakeRow(0, 3, 1, 999);
    Expect(lo.SetLocalRow(&L) && L.seq == 1, "first local -> seq 1");
    P::PaletteBlobBody L2 = L;  // identical content
    Expect(!lo.SetLocalRow(&L2), "unchanged local -> no send");
    Expect(L2.seq == 1, "unchanged keeps prior seq");
    L2.colorSlot = 4;  // content change
    L2.keyDword = source::PackKeyDword(L2.charId, L2.sourceFlag, L2.colorSlot);
    L2.keyDword = source::PackKeyDword(L2.charId, L2.sourceFlag, L2.colorSlot);
    Expect(lo.SetLocalRow(&L2) && L2.seq == 2, "changed local -> seq 2");
    P::PaletteBlobBody L3 = L2;
    L3.seq = 12345;
    Expect(!lo.SetLocalRow(&L3), "seq-only change is not a content change");
    P::PaletteBlobBody R = MakeRow(1, 9, 0, 0);
    Expect(lo.SetLocalRow(&R) && R.seq == 1, "side1 outbound seq independent");
}

std::vector<std::vector<std::uint8_t>> g_sent;
bool CaptureSink(const std::uint8_t* d, std::size_t n, void*)
{
    g_sent.emplace_back(d, d + n);
    return true;
}
bool DropSink(const std::uint8_t*, std::size_t, void*) { return false; }

void ArmChannel(OverlayChannel& ch, int side)
{
    ch.End();
    ch.SetSendSink(&CaptureSink, nullptr);
    g_sent.clear();
    ch.Begin(side);
}

void TestChannel()
{
    OverlayChannel& ch = OverlayChannel::Instance();

    SetFlag(false);
    ch.SetSendSink(&CaptureSink, nullptr);
    g_sent.clear();
    ch.Begin(0);
    Expect(!ch.IsActive(), "flag off: Begin does not arm");
    ch.Tick(1000);
    P::PaletteBlobBody row = MakeRow(0, 1, 0, 0);
    ch.SubmitLocalPaletteRow(row);
    std::uint8_t hello[P::kMaxFrameBytes];
    std::size_t hn = P::BuildHello(P::kFeaturePalettes, 1u, hello, sizeof hello);
    Expect(!ch.OnInboundDatagram(hello, hn), "flag off: inbound ignored");
    Expect(g_sent.empty(), "flag off: nothing sent");

    SetFlag(true);
    ArmChannel(ch, 0);
    Expect(ch.IsActive() && ch.State() == HandshakeState::Probing,
           "armed -> Probing");

    ch.Tick(1000);
    Expect(g_sent.size() == 1, "first Tick emits one Hello");
    P::HelloBody ourHello{};
    Expect(P::ParseHello(g_sent[0].data(), g_sent[0].size(), &ourHello),
           "emitted frame is a Hello");
    const std::uint32_t ourNonce = ourHello.nonce;

    ch.Tick(1100);
    Expect(g_sent.size() == 1, "throttled: no re-emit before interval");

    g_sent.clear();
    std::uint8_t ack[P::kMaxFrameBytes];
    std::size_t an = P::BuildAck(P::kFeaturePalettes, ourNonce ^ 0x1u, ack, sizeof ack);
    Expect(ch.OnInboundDatagram(ack, an), "inbound consumed (wrong-nonce ack)");
    Expect(ch.State() == HandshakeState::Probing, "wrong-nonce ack: still Probing");

    an = P::BuildAck(P::kFeaturePalettes, ourNonce, ack, sizeof ack);
    Expect(ch.OnInboundDatagram(ack, an), "inbound consumed (correct ack)");
    Expect(ch.State() == HandshakeState::Confirmed, "correct ack -> Confirmed");

    g_sent.clear();
    P::PaletteBlobBody local = MakeRow(0, 4, 2, 0);
    ch.SubmitLocalPaletteRow(local);
    Expect(g_sent.size() == 1, "Confirmed: palette submit transmits");
    P::PaletteBlobBody sent{};
    Expect(P::ParsePaletteBlob(g_sent[0].data(), g_sent[0].size(), &sent)
               && sent.side == 0 && sent.charId == 4 && sent.seq == 1,
           "transmitted palette body correct, seq assigned");
    ch.SubmitLocalPaletteRow(local);
    Expect(g_sent.size() == 1, "unchanged palette: no re-transmit");
    local.colorSlot = 5;
    local.keyDword = source::PackKeyDword(local.charId, local.sourceFlag, local.colorSlot);
    ch.SubmitLocalPaletteRow(local);
    Expect(g_sent.size() == 1, "changed palette inside interval: coalesced");
    std::this_thread::sleep_for(
        std::chrono::milliseconds(kPaletteSendMinIntervalMs + 40u));
    ch.SubmitLocalPaletteRow(local);
    Expect(g_sent.size() == 2, "pending row transmits after the interval");
    P::PaletteBlobBody coalesced{};
    Expect(P::ParsePaletteBlob(g_sent[1].data(), g_sent[1].size(), &coalesced)
               && coalesced.colorSlot == 5,
           "coalesced send carries the newest row");

    P::PaletteBlobBody peer = MakeRow(1, 11, 3, 7);
    std::uint8_t pbuf[P::kMaxFrameBytes];
    std::size_t pn = P::BuildPaletteBlob(peer, pbuf, sizeof pbuf);
    Expect(ch.OnInboundDatagram(pbuf, pn), "peer palette consumed");
    P::PaletteBlobBody peerGot{};
    Expect(ch.GetPeerPaletteRow(1, &peerGot) && peerGot.charId == 11,
           "peer palette cached");
    ch.ResetPaletteExchangeForRematch();
    Expect(!ch.GetPeerPaletteRow(1, &peerGot), "rematch reset clears peer rows");

    std::uint8_t junk[P::kMaxFrameBytes];
    std::memcpy(junk, hello, hn);
    junk[P::kEnvelopeBytes + 4] = 0xEE;  // bad version
    Expect(ch.OnInboundDatagram(junk, hn), "malformed ours still consumed");

    ArmChannel(ch, 1);
    std::uint8_t peerHello[P::kMaxFrameBytes];
    std::size_t phn = P::BuildHello(P::kFeaturePalettes, 0x55AA55AAu, peerHello, sizeof peerHello);
    Expect(ch.OnInboundDatagram(peerHello, phn), "peer hello consumed");
    Expect(ch.State() == HandshakeState::Confirmed, "peer hello -> Confirmed");
    Expect(g_sent.size() == 1, "peer hello -> we emit an Ack");
    P::HelloBody ourAck{};
    Expect(P::ParseAck(g_sent[0].data(), g_sent[0].size(), &ourAck)
               && ourAck.nonce == 0x55AA55AAu,
           "our Ack echoes the peer nonce");

    ArmChannel(ch, -1);
    ch.OnInboundDatagram(peerHello, phn);
    g_sent.clear();
    P::PaletteBlobBody specRow = MakeRow(0, 2, 1, 0);
    ch.SubmitLocalPaletteRow(specRow);
    Expect(g_sent.empty(), "spectator never transmits a local row");

    ArmChannel(ch, 0);
    ch.Tick(1000);   // hello 1
    ch.Tick(1400);   // hello 2
    ch.Tick(1800);   // hello 3
    ch.Tick(2200);   // hello 4 (== kMaxHelloProbes)
    Expect(g_sent.size() == kMaxHelloProbes, "emitted exactly the probe budget");
    ch.Tick(2600);   // budget exhausted -> VanillaPeer
    Expect(ch.State() == HandshakeState::VanillaPeer, "no ack -> VanillaPeer");
    ch.Tick(3000);
    Expect(g_sent.size() == kMaxHelloProbes, "VanillaPeer: silent thereafter");

    ch.End();
    ch.SetSendSink(&DropSink, nullptr);
    ch.Begin(0);
    for (std::uint32_t t = 1000; t <= 5000; t += 400) ch.Tick(t);
    Expect(ch.State() == HandshakeState::Probing,
           "no transport: stays Probing, never false-times-out");

    ch.End();
    ch.SetSendSink(nullptr, nullptr);
    ch.Begin(0);
    ch.Tick(1000);
    ch.SubmitLocalPaletteRow(MakeRow(0, 1, 0, 0));
    Expect(ch.State() == HandshakeState::Probing, "null sink: no crash, Probing");

    ArmChannel(ch, 0);
    Expect(ch.IsActive(), "armed before toggle");
    SetFlag(false);
    ch.Tick(1000);
    Expect(!ch.IsActive(), "flag toggled off -> Tick disarms");
    SetFlag(true);  // restore for any later tests

    ch.End();
    ch.End();
    Expect(!ch.IsActive(), "double End safe");
}

void TestInboundFloodLogs()
{
    auto& ch = OverlayChannel::Instance();
    SetFlag(true);
    ArmChannel(ch, -1);
    mod::logLines.clear();
    std::uint8_t frame[P::kMaxFrameBytes];
    auto row = MakeRow(1, 14, 0, 0);
    for (unsigned i = 1; i <= 5000; ++i)
    {
        row.seq = static_cast<std::uint16_t>(i);
        row.rawLen = 119;
        auto n = P::BuildPaletteBlob(row, frame, sizeof(frame));
        ch.OnInboundDatagram(frame, n);
        P::PaletteBlobBody out{};
        if (i == 1) Expect(!ch.GetPeerPaletteRow(1, &out), "invalid palette never cached");
        row.rawLen = 120;
        n = P::BuildPaletteBlob(row, frame, sizeof(frame));
        ch.OnInboundDatagram(frame, n);
    }
    P::PaletteBlobBody out{};
    Expect(ch.GetPeerPaletteRow(1, &out) && out.seq == 5000,
           "spectator receives valid rows before handshake despite rejected traffic");
    Expect(mod::logLines.size() >= 2 && mod::logLines.size() <= 4,
           "received and rejected packet logs are rate limited");
    ch.End();
}
void WriteBytes(const char* path, int count, int start, int step)
{
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;
    for (int i = 0; i < count; ++i)
    {
        unsigned char b = static_cast<unsigned char>((start + i * step) & 0xFF);
        std::fwrite(&b, 1, 1, f);
    }
    std::fclose(f);
}

void WritePalWithHeader(const char* path, unsigned char headerByte, int trailer)
{
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) return;
    std::fwrite(&headerByte, 1, 1, f);
    for (int i = 0; i < 120; ++i)
    {
        unsigned char b = static_cast<unsigned char>(i & 0xFF);
        std::fwrite(&b, 1, 1, f);
    }
    for (int i = 0; i < trailer; ++i)
    {
        unsigned char z = 0;
        std::fwrite(&z, 1, 1, f);
    }
    std::fclose(f);
}

void TestSource()
{
    namespace SRC = netplay::interop::source;

    Expect(SRC::CharFolderName(0) && std::strcmp(SRC::CharFolderName(0), "nanase") == 0,
           "char 0 -> nanase");
    Expect(SRC::CharFolderName(12) && std::strcmp(SRC::CharFolderName(12), "exnanase") == 0,
           "char 12 -> exnanase");
    Expect(SRC::CharFolderName(23) && std::strcmp(SRC::CharFolderName(23), "misuzu") == 0,
           "char 23 -> misuzu");
    Expect(SRC::CharFolderName(-1) == nullptr, "char -1 -> null");
    Expect(SRC::CharFolderName(24) == nullptr, "char 24 -> null");
    Expect(SRC::CharFolderName(999) == nullptr, "char 999 -> null");

    // keyDword packing (slot | src<<16 | char<<24)
    Expect(SRC::PackKeyDword(7, 1, 3) == 0x07010003u, "packKey char7/src1/slot3");
    Expect(SRC::PackKeyDword(0, 0, 0) == 0u, "packKey zero");
    Expect(SRC::PackKeyDword(24, 0, 5) == 0x18000005u, "packKey char24/slot5");

    std::string p;
    Expect(SRC::BuildPalRelPath(0, 0, &p) && p == "nanase\\nanase1.pal",
           "path nanase slot0 -> nanase1.pal");
    Expect(SRC::BuildPalRelPath(1, 5, &p) && p == "ayu\\ayu6.pal",
           "path ayu slot5 -> ayu6.pal (1-based on disk)");
    Expect(!SRC::BuildPalRelPath(0, 6, &p), "out-of-range slot rejected");
    Expect(!SRC::BuildPalRelPath(99, 0, &p), "path unknown char -> false");
    Expect(!SRC::BuildPalRelPath(0, 0, nullptr), "path null out -> false");

    std::uint8_t raw[netplay::interop::protocol::kPaletteRawBytes];

    WriteBytes("interop_tmp_120.bin", 120, 0, 1);
    Expect(SRC::LoadRawPalFile("interop_tmp_120.bin", raw), "load 120-byte pal");
    Expect(raw[0] == 0 && raw[119] == 119, "120-byte: read from offset 0");

    WritePalWithHeader("interop_tmp_121.bin", 0xAA, /*trailer=*/0);
    Expect(SRC::LoadRawPalFile("interop_tmp_121.bin", raw)
               && raw[0] == 0 && raw[119] == 119,
           "121-byte: header skipped, body from offset 1");

    WritePalWithHeader("interop_tmp_123.bin", 0x00, /*trailer=*/2);
    Expect(SRC::LoadRawPalFile("interop_tmp_123.bin", raw)
               && raw[0] == 0 && raw[119] == 119,
           "123-byte header+body+trailer: reads the body (nanase3 fix)");

    Expect(!SRC::LoadRawPalFile("interop_tmp_missing.bin", raw), "missing -> false");
    Expect(!SRC::LoadRawPalFile(nullptr, raw), "null path -> false");
    Expect(!SRC::LoadRawPalFile("interop_tmp_120.bin", nullptr), "null out -> false");
    WriteBytes("interop_tmp_short.bin", 50, 0, 1);
    Expect(!SRC::LoadRawPalFile("interop_tmp_short.bin", raw), "short (<120) -> false");
    std::remove("interop_tmp_120.bin");
    std::remove("interop_tmp_121.bin");
    std::remove("interop_tmp_123.bin");
    std::remove("interop_tmp_short.bin");

    std::uint8_t body[120];
    for (int i = 0; i < 120; ++i) body[i] = static_cast<std::uint8_t>(i + 1);
    P::PaletteBlobBody cr = SRC::AssembleRow(1, 5, 2, true, body);
    Expect(cr.side == 1 && cr.charId == 5 && cr.colorSlot == 2
               && cr.sourceFlag == 1 && cr.slotByte == 1,
           "custom row flags");
    Expect(cr.rawLen == 120 && std::memcmp(cr.rawBgr, body, 120) == 0,
           "custom row raw bytes");
    Expect(cr.keyDword == SRC::PackKeyDword(5, 1, 2), "custom row keyDword");
    P::PaletteBlobBody sr = SRC::AssembleRow(0, 3, 1, false, nullptr);
    Expect(sr.sourceFlag == 0 && sr.slotByte == 0, "stock/CLEAR row flags");
    bool allZero = true;
    for (int i = 0; i < 120; ++i) if (sr.rawBgr[i] != 0) allZero = false;
    Expect(allZero, "stock row rawBgr zeroed");
    P::PaletteBlobBody cn = SRC::AssembleRow(0, 1, 0, true, nullptr);
    allZero = true;
    for (int i = 0; i < 120; ++i) if (cn.rawBgr[i] != 0) allZero = false;
    Expect(allZero, "custom flag + null bytes leaves rawBgr zeroed");

    _mkdir("interop_tmp_base");
    _mkdir("interop_tmp_base\\nanase");
    WriteBytes("interop_tmp_base\\nanase\\nanase1.pal", 120, 200, -1);
    P::PaletteBlobBody lr{};
    Expect(SRC::LoadLocalRow("interop_tmp_base", 0, 0, 0, &lr) && lr.sourceFlag == 1,
           "LoadLocalRow finds custom .pal");
    Expect(lr.rawBgr[0] == 200 && lr.charId == 0 && lr.colorSlot == 0,
           "LoadLocalRow loaded bytes + fields");
    P::PaletteBlobBody lr2{};
    Expect(SRC::LoadLocalRow("interop_tmp_base\\", 0, 0, 0, &lr2)
               && lr2.sourceFlag == 1,
           "LoadLocalRow tolerates trailing slash");
    P::PaletteBlobBody mr{};
    Expect(!SRC::LoadLocalRow("interop_tmp_base", 1, 2, 4, &mr)
               && mr.sourceFlag == 0 && mr.charId == 2 && mr.colorSlot == 4,
           "LoadLocalRow missing -> stock CLEAR, fields set");
    P::PaletteBlobBody ur{};
    Expect(!SRC::LoadLocalRow("interop_tmp_base", 0, 99, 0, &ur)
               && ur.sourceFlag == 0,
           "LoadLocalRow unknown char -> stock CLEAR");
    std::remove("interop_tmp_base\\nanase\\nanase1.pal");
    _rmdir("interop_tmp_base\\nanase");
    _rmdir("interop_tmp_base");

    // .pal header-byte detection (script files are 1 pad + 120 data).
    WriteBytes("interop_tmp_hdr.bin", 121, 7, 1);   // file[0]=7, file[1]=8...
    Expect(SRC::LoadRawPalFile("interop_tmp_hdr.bin", raw), "121-byte pal loads");
    Expect(raw[0] == 8 && raw[119] == 127,
           "121-byte pal skips the 1-byte header");
    WriteBytes("interop_tmp_raw.bin", 120, 7, 1);
    Expect(SRC::LoadRawPalFile("interop_tmp_raw.bin", raw) && raw[0] == 7,
           "120-byte raw pal reads from byte 0");
    WriteBytes("interop_tmp_h2.bin", 100, 0, 1);
    Expect(!SRC::LoadRawPalFile("interop_tmp_h2.bin", raw),
           "100-byte (hdr + short data) rejected");
    std::remove("interop_tmp_hdr.bin");
    std::remove("interop_tmp_raw.bin");
    std::remove("interop_tmp_h2.bin");
}

void TestRemap()
{
    namespace RM = netplay::interop::remap;
    RM::PortraitColor out[RM::kMaxPortraitColors];

    // raw[i*3+0]=B, +1=G, +2=R for sprite index i+1 (1-based indices).
    std::uint8_t pal[120];
    for (int i = 0; i < 40; ++i)
    {
        pal[i * 3 + 0] = static_cast<std::uint8_t>(10 + i);        // B
        pal[i * 3 + 1] = static_cast<std::uint8_t>(60 + i);        // G
        pal[i * 3 + 2] = static_cast<std::uint8_t>(160 + i);       // R
    }

    Expect(RM::RemapSpriteToPortrait(nullptr, "akiko", out, 40) == 0,
           "remap null pal -> 0");
    Expect(RM::RemapSpriteToPortrait(pal, nullptr, out, 40) == 0,
           "remap null name -> 0");
    Expect(RM::RemapSpriteToPortrait(pal, "notachar", out, 40) == 0,
           "remap unknown char -> 0");
    Expect(RM::RemapSpriteToPortrait(pal, "akiko", nullptr, 40) == 0,
           "remap null out -> 0");
    Expect(RM::RemapSpriteToPortrait(pal, "akiko", out, 0) == 0,
           "remap zero cap -> 0");

    const std::size_t nAkiko = RM::RemapSpriteToPortrait(pal, "akiko", out, 40);
    Expect(nAkiko > 0, "akiko remap produces writes");
    RM::PortraitColor out2[RM::kMaxPortraitColors];
    Expect(RM::RemapSpriteToPortrait(pal, "AKIKO", out2, 40) == nAkiko,
           "case-insensitive char name");

    bool saw17 = false, saw18 = false;
    for (std::size_t i = 0; i < nAkiko; ++i)
    {
        if (out[i].portIdx == 17)
        {
            saw17 = true;
            Expect(out[i].b == pal[16 * 3 + 0] && out[i].g == pal[16 * 3 + 1]
                       && out[i].r == pal[16 * 3 + 2],
                   "direct 1:1 copies sprite 17 exactly");
        }
        if (out[i].portIdx == 18) saw18 = true;
    }
    Expect(saw17 && saw18, "akiko eyes area emitted both portrait slots");

    for (std::size_t i = 0; i < nAkiko; ++i)
    {
        if (out[i].portIdx == 2)
        {
            Expect(std::abs(int(out[i].b) - int(pal[1 * 3 + 0])) <= 2
                       && std::abs(int(out[i].r) - int(pal[1 * 3 + 2])) <= 2,
                   "gradient start ~= first control point (HSL round-trip)");
        }
        if (out[i].portIdx == 4)
        {
            Expect(out[i].b == pal[4 * 3 + 0] && out[i].r == pal[4 * 3 + 2],
                   "gradient end == last control point");
        }
    }

    const std::size_t nAkane = RM::RemapSpriteToPortrait(pal, "akane", out, 40);
    Expect(nAkane > 0, "akane remap produces writes");
    bool saw13 = false, saw9 = false;
    for (std::size_t i = 0; i < nAkane; ++i)
    {
        if (out[i].portIdx == 13)
        {
            saw13 = true;
            Expect(std::abs(int(out[i].b) - int(pal[22 * 3 + 0])) <= 2
                       && std::abs(int(out[i].r) - int(pal[22 * 3 + 2])) <= 2,
                   "reversed area start ~= sprite 23");
        }
        if (out[i].portIdx == 9)
        {
            saw9 = true;
            Expect(out[i].b == pal[24 * 3 + 0] && out[i].r == pal[24 * 3 + 2],
                   "reversed area end == sprite 25");
        }
    }
    Expect(saw13 && saw9, "reversed portrait range covers both ends");

    std::uint8_t palB[120];
    std::memcpy(palB, pal, sizeof(palB));
    for (int idx : {26, 27})   // sprite indices 27,28 are 0-based 26,27
    {
        palB[idx * 3 + 0] = 0; palB[idx * 3 + 1] = 0; palB[idx * 3 + 2] = 0;
    }
    const std::size_t nBlack = RM::RemapSpriteToPortrait(palB, "akiko", out, 40);
    Expect(nBlack > 0 && nBlack < nAkiko,
           "black-unset area skipped, rest still emitted");
    for (std::size_t i = 0; i < nBlack; ++i)
    {
        Expect(!(out[i].portIdx == 27 || out[i].portIdx == 28),
               "no writes into the skipped acc area");
    }

    std::uint8_t palZ[120] = {};
    Expect(RM::RemapSpriteToPortrait(palZ, "akiko", out, 40) == 0,
           "all-black palette -> 0 writes");

    RM::PortraitColor a1[40], a2[40];
    const std::size_t n1 = RM::RemapSpriteToPortrait(pal, "nanase", a1, 40);
    const std::size_t n2 = RM::RemapSpriteToPortrait(pal, "rumi", a2, 40);
    Expect(n1 == n2 && n1 > 0
               && std::memcmp(a1, a2, n1 * sizeof(RM::PortraitColor)) == 0,
           "nanase/rumi aliases identical");

    Expect(RM::RemapSpriteToPortrait(pal, "akiko", out, 3) == 3,
           "cap truncates output");

    const std::uint8_t rgbs[][3] = {
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255},
        {200, 150, 100}, {17, 200, 90}, {255, 255, 255},
    };
    for (const auto& c : rgbs)
    {
        float h, s, l;
        RM::RgbToHsl(c[0], c[1], c[2], &h, &s, &l);
        std::uint8_t r2, g2, b2;
        RM::HslToRgb(h, s, l, &r2, &g2, &b2);
        Expect(std::abs(int(c[0]) - int(r2)) <= 2
                   && std::abs(int(c[1]) - int(g2)) <= 2
                   && std::abs(int(c[2]) - int(b2)) <= 2,
               "HSL round-trip within tolerance");
    }

    {
        RM::PortraitColor mf[40];
        const std::size_t nMf = RM::RemapSpriteToPortrait(pal, "minagi", mf, 40);
        const RM::PortraitColor *m9 = nullptr, *m10 = nullptr, *m11 = nullptr;
        for (std::size_t i = 0; i < nMf; ++i)
        {
            if (mf[i].portIdx == 9)  m9 = &mf[i];
            if (mf[i].portIdx == 10) m10 = &mf[i];
            if (mf[i].portIdx == 11) m11 = &mf[i];
        }
        auto scl = [](std::uint8_t c, float f) -> std::uint8_t {
            float v = static_cast<float>(c) * f + 0.5f;
            if (v > 255.0f) v = 255.0f;
            return static_cast<std::uint8_t>(v);
        };
        Expect(m11 && m9
                   && m9->r == scl(m11->r, 0.8f) && m9->g == scl(m11->g, 0.8f)
                   && m9->b == scl(m11->b, 0.8f),
               "ff: slot 9 = slot 11 * 0.8 (overwrote the area value)");
        Expect(m10 && m9
                   && m10->r == scl(m9->r, 0.6f) && m10->g == scl(m9->g, 0.6f)
                   && m10->b == scl(m9->b, 0.6f),
               "ff cascade: slot 10 = updated slot 9 * 0.6");
    }

    {
        RM::PortraitColor mio[40];
        const std::size_t nMio = RM::RemapSpriteToPortrait(pal, "mio", mio, 40);
        bool sawBow = false;
        for (std::size_t i = 0; i < nMio; ++i)
        {
            if (mio[i].portIdx >= 37 && mio[i].portIdx <= 40) sawBow = true;
        }
        Expect(sawBow, "mio bow area produces portrait writes at 37-40");
    }
}
void TestIpcRing()
{
    namespace R = netplay::interop::ipc;
    auto ring = std::make_unique<R::Ring>();
    std::memset(ring.get(), 0, sizeof(R::Ring));

    std::uint8_t out[R::kSlotBytes];

    Expect(R::Pop(*ring, out, sizeof(out)) == 0, "ipc: pop empty -> 0");

    const std::uint8_t f1[] = {1, 2, 3, 4, 5};
    Expect(R::Push(*ring, f1, sizeof(f1)), "ipc: push ok");
    const std::uint32_t n1 = R::Pop(*ring, out, sizeof(out));
    Expect(n1 == sizeof(f1) && std::memcmp(out, f1, n1) == 0,
           "ipc: round-trip content");
    Expect(R::Pop(*ring, out, sizeof(out)) == 0, "ipc: drained -> empty");
    Expect(R::Push(*ring, f1, sizeof(f1)), "ipc: push larger than destination");
    std::memset(out, 0xCC, sizeof(out));
    Expect(R::Pop(*ring, out, sizeof(f1) - 1) == 0 && out[0] == 0xCC,
           "ipc: oversized frame discarded without partial copy");
    Expect(R::Push(*ring, f1, sizeof(f1)), "ipc: push after oversized frame");
    Expect(R::Pop(*ring, out, sizeof(out)) == sizeof(f1), "ipc: recovers after oversized frame");

    Expect(!R::Push(*ring, nullptr, 4), "ipc: push null -> false");
    Expect(!R::Push(*ring, f1, 0), "ipc: push zero-len -> false");
    std::uint8_t big[R::kSlotBytes + 1] = {};
    Expect(!R::Push(*ring, big, sizeof(big)), "ipc: push oversize -> false");

    std::memset(ring.get(), 0, sizeof(R::Ring));
    std::uint32_t pushed = 0;
    for (std::uint32_t i = 0; i < R::kRingSlots + 4u; ++i)
    {
        const std::uint8_t b = static_cast<std::uint8_t>(i);
        if (R::Push(*ring, &b, 1)) ++pushed;
    }
    Expect(pushed == R::kRingSlots - 1u, "ipc: capacity is slots-1");

    bool order = true;
    for (std::uint32_t i = 0; i < pushed; ++i)
    {
        const std::uint32_t n = R::Pop(*ring, out, sizeof(out));
        if (n != 1 || out[0] != static_cast<std::uint8_t>(i)) order = false;
    }
    Expect(order, "ipc: FIFO order preserved");
    Expect(R::Pop(*ring, out, sizeof(out)) == 0, "ipc: empty after drain");

    std::memset(ring.get(), 0, sizeof(R::Ring));
    bool wrapOk = true;
    for (std::uint32_t i = 0; i < R::kRingSlots * 8u; ++i)
    {
        const std::uint8_t b = static_cast<std::uint8_t>(i * 7u + 1u);
        if (!R::Push(*ring, &b, 1)) { wrapOk = false; break; }
        std::uint8_t got = 0;
        if (R::Pop(*ring, &got, 1) != 1 || got != b) { wrapOk = false; break; }
    }
    Expect(wrapOk, "ipc: wrap-around round-trips");

    std::memset(ring.get(), 0, sizeof(R::Ring));
    Expect(R::Push(*ring, f1, sizeof(f1)), "ipc: push before corrupt");
    ring->slots[ring->tail & R::kRingMask].len = R::kSlotBytes + 99u;  // corrupt
    Expect(R::Pop(*ring, out, sizeof(out)) == 0, "ipc: corrupt len -> skipped");
    Expect(R::Pop(*ring, out, sizeof(out)) == 0, "ipc: nothing left after skip");
}
void TestVersionCompare()
{
    namespace V = netplay::version;
    V::ParsedVersion p;
    Expect(V::Parse("0.4.2_beta1", &p) && p.partCount == 3 && p.parts[0] == 0 && p.parts[1] == 4
               && p.parts[2] == 2 && p.suffix == "_beta1",
           "version: parse numeric tuple + suffix");
    Expect(V::Parse("v1.2", &p) && p.partCount == 2 && p.parts[0] == 1 && p.parts[1] == 2
               && p.suffix.empty(),
           "version: leading v stripped");
    Expect(!V::Parse("beta", &p), "version: no leading number -> invalid");
    Expect(!V::Parse("", &p), "version: empty -> invalid");

    Expect(V::Compare("0.3.5", "0.5.0") < 0, "version: 0.3.5 < 0.5.0");
    Expect(V::Compare("0.5.0", "0.3.5") > 0, "version: 0.5.0 > 0.3.5");
    Expect(V::Compare("0.5.0", "0.5.0") == 0, "version: equal");
    Expect(V::Compare("0.5", "0.5.0") == 0, "version: missing component = 0");
    Expect(V::Compare("0.10.0", "0.9.9") > 0, "version: numeric not lexicographic");
    Expect(V::Compare("0.4.2_beta1", "0.4.2") < 0, "version: pre-release < final");
    Expect(V::Compare("0.4.2", "0.4.2_beta1") > 0, "version: final > pre-release");
    Expect(V::Compare("0.4.2_beta1", "0.4.2_beta2") < 0, "version: beta1 < beta2");
    Expect(V::Compare("0.4.2_beta2", "0.4.2_beta10") < 0, "version: beta2 < beta10");
    Expect(V::Compare("0.4.2_alpha3", "0.4.2_beta1") < 0, "version: alpha < beta");
    Expect(V::Compare("0.4.2_beta1", "0.4.2-rc1") < 0, "version: beta < rc");
    Expect(V::Compare("0.4.3", "0.4.2_beta1") > 0, "version: next patch > pre-release");
    Expect(V::Compare("garbage", "0.1") < 0, "version: invalid ranks below valid");
    Expect(V::Compare("garbage", "junk") == 0, "version: invalid == invalid");

    Expect(V::IsNewer("0.6.0", "0.5.0"), "version: IsNewer newer");
    Expect(!V::IsNewer("0.3.5", "0.5.0"), "version: IsNewer older");
    Expect(!V::IsNewer("0.5.0", "0.5.0"), "version: IsNewer same");
    Expect(!V::IsNewer("0.5.0_beta1", "0.5.0"), "version: IsNewer pre-release of current");
    Expect(V::IsNewer("0.5.1_beta1", "0.5.0"), "version: IsNewer next pre-release");
    Expect(!V::IsNewer("", "0.5.0"), "version: IsNewer empty candidate");
    Expect(!V::IsNewer("latest", "0.5.0"), "version: IsNewer non-numeric candidate");
}
} // namespace

int main()
{
    TestProtocol();
    TestUntrustedPalettes();
    TestPaletteWriteBounds();
    TestStore();
    TestSource();
    TestRemap();
    TestChannel();
    TestInboundFloodLogs();
    TestIpcRing();
    TestVersionCompare();
    if (g_failures != 0)
    {
        std::cerr << g_failures << " interop test(s) failed\n";
        return 1;
    }
    std::cout << "interop tests passed\n";
    return 0;
}
