// Unit tests for the mod-interop overlay channel (netplay/interop/*).
// Covers the wire codec, the palette store's sequencing/edge cases, and the
// channel's handshake/gating state machine.
//
// The channel depends on two symbols we stub here so the test does not pull in
// the ini/Windows-logging layers: mod::Log (no-op) and
// mod_settings::AreOnlineCustomColorsEnabled (test-controlled). GetTickCount is
// the only real OS dependency, hence WIN32.

#include "netplay/interop/overlay_protocol.h"
#include "netplay/interop/palette_store.h"
#include "netplay/interop/overlay_channel.h"
#include "netplay/interop/palette_source.h"
#include "netplay/interop/palette_remap.h"
#include "netplay/interop/overlay_ipc.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <direct.h>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// --- stubs for the channel's external symbols -------------------------------
namespace mod
{
void Log(const char*, ...) {}
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
    b.keyDword = 0x01020304u;
    for (std::size_t i = 0; i < P::kPaletteRawBytes; ++i)
    {
        b.rawBgr[i] = static_cast<std::uint8_t>(i + charId);
    }
    return b;
}

// ---------------------------------------------------------------------------
void TestProtocol()
{
    std::uint8_t buf[P::kMaxFrameBytes];

    // Envelope + Hello round-trip.
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

    // Ack round-trip; cross-parse must fail (kind mismatch).
    n = P::BuildAck(P::kFeaturePalettes, 0x1234u, buf, sizeof buf);
    Expect(P::ParseAck(buf, n, &hb) && hb.nonce == 0x1234u, "parse Ack");
    Expect(!P::ParseHello(buf, n, &hb), "Ack must not parse as Hello");

    // PaletteBlob round-trip incl. raw payload integrity.
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

    // --- encode edge cases ---
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

    // --- decode edge cases ---
    Expect(!P::LooksLikeOverlayFrame(nullptr, 100), "null datagram");
    Expect(!P::LooksLikeOverlayFrame(buf, 0), "len 0");
    Expect(!P::LooksLikeOverlayFrame(buf, P::kEnvelopeBytes + sizeof(P::FrameHeader) - 1),
           "too short for header");
    // A real Revival datagram (typeId 1, arbitrary body) must never match.
    std::uint8_t vanilla[32];
    for (int i = 0; i < 32; ++i) vanilla[i] = static_cast<std::uint8_t>(i);
    vanilla[1] = 0x01;  // a real Revival typeId
    Expect(!P::LooksLikeOverlayFrame(vanilla, sizeof vanilla),
           "vanilla typeId not ours");
    // Right typeId, wrong magic.
    std::uint8_t badmagic[64];
    std::memcpy(badmagic, buf, sizeof(P::FrameHeader) + P::kEnvelopeBytes);
    badmagic[P::kEnvelopeBytes] ^= 0xFF;  // corrupt magic byte 0
    Expect(!P::LooksLikeOverlayFrame(badmagic, 64), "right typeId wrong magic");

    // Rebuild a clean Hello for structural tampering tests.
    n = P::BuildHello(P::kFeaturePalettes, 1u, buf, sizeof buf);
    // Version mismatch.
    std::uint8_t tampered[64];
    std::memcpy(tampered, buf, n);
    tampered[P::kEnvelopeBytes + 4] = 0xEE;  // FrameHeader.version
    Expect(P::LooksLikeOverlayFrame(tampered, n), "tampered still looks-like");
    Expect(!P::ParseHello(tampered, n, &hb), "bad version rejected");
    P::Kind k2;
    Expect(!P::ParseKind(tampered, n, &k2), "bad version -> ParseKind false");
    // Declared length exceeds datagram (truncated body).
    std::memcpy(tampered, buf, n);
    tampered[P::kEnvelopeBytes + 6] = 0xFF;  // FrameHeader.length low = 255
    tampered[P::kEnvelopeBytes + 7] = 0x00;
    Expect(!P::ParseHello(tampered, n, &hb), "declared length > datagram rejected");
    // Wrong body length for kind (Hello with length claiming palette size but
    // frame is actually Hello-sized -> length mismatch).
    std::memcpy(tampered, buf, n);
    tampered[P::kEnvelopeBytes + 6] =
        static_cast<std::uint8_t>(sizeof(P::PaletteBlobBody));  // 132
    Expect(!P::ParseHello(tampered, n, &hb),
           "Hello with non-Hello length rejected");

    // rawLen out of range in a PaletteBlob.
    pb = MakeRow(0, 3, 2, 9);
    pb.rawLen = 200;  // > kPaletteRawBytes
    n = P::BuildPaletteBlob(pb, buf, sizeof buf);
    Expect(!P::ParsePaletteBlob(buf, n, &got), "rawLen > 120 rejected");

    // Trailing bytes after a valid frame: still parses the prefix (documents
    // the lenient-length contract; the frame body is exact-copied).
    n = P::BuildHello(P::kFeaturePalettes, 5u, buf, sizeof buf);
    std::uint8_t padded[128];
    std::memcpy(padded, buf, n);
    std::memset(padded + n, 0xAB, 40);
    Expect(P::ParseHello(padded, n + 40, &hb) && hb.nonce == 5u,
           "trailing padding tolerated");

    // Unknown (future) kind still decodes via ParseKind but not a typed parse.
    std::memcpy(tampered, buf, n);
    tampered[P::kEnvelopeBytes + 5] = 99;  // FrameHeader.kind
    Expect(P::ParseKind(tampered, n, &k2)
               && static_cast<int>(k2) == 99,
           "unknown kind surfaces via ParseKind");
    Expect(!P::ParseHello(tampered, n, &hb), "unknown kind not a Hello");
}

// ---------------------------------------------------------------------------
void TestStore()
{
    palette::Store st;

    // Bad side rejected on both paths.
    P::PaletteBlobBody bad = MakeRow(2, 0, 0, 1);
    Expect(!st.AcceptInbound(bad), "AcceptInbound side>1 rejected");
    P::PaletteBlobBody badLocal = MakeRow(2, 0, 0, 1);
    Expect(!st.SetLocalRow(&badLocal), "SetLocalRow side>1 rejected");

    // First accept always; dup/stale rejected; newer accepted.
    Expect(st.AcceptInbound(MakeRow(0, 5, 1, 100)), "first inbound accepted");
    Expect(!st.AcceptInbound(MakeRow(0, 5, 1, 100)), "duplicate seq rejected");
    Expect(!st.AcceptInbound(MakeRow(0, 5, 1, 99)), "stale (reordered) rejected");
    Expect(st.AcceptInbound(MakeRow(0, 5, 1, 101)), "newer seq accepted");
    P::PaletteBlobBody out{};
    Expect(st.GetRow(0, &out) && out.seq == 101, "GetRow returns latest");

    // Sides are independent.
    Expect(!st.GetRow(1, &out), "other side empty");
    Expect(st.AcceptInbound(MakeRow(1, 8, 2, 1)), "side1 first accepted");
    Expect(st.GetRow(1, &out) && out.charId == 8, "side1 row cached");
    Expect(st.GetRow(0, &out) && out.seq == 101, "side0 unaffected by side1");

    // 16-bit wrap: last=65535, incoming=0 is newer; backward is stale.
    palette::Store w;
    Expect(w.AcceptInbound(MakeRow(0, 1, 0, 65535)), "seq 65535 accepted");
    Expect(w.AcceptInbound(MakeRow(0, 1, 0, 0)), "wrap 65535->0 is newer");
    Expect(!w.AcceptInbound(MakeRow(0, 1, 0, 65535)), "backward across wrap stale");

    // GetRow bounds/null.
    Expect(!st.GetRow(-1, &out), "GetRow side -1 rejected");
    Expect(!st.GetRow(2, &out), "GetRow side 2 rejected");
    Expect(!st.GetRow(0, nullptr), "GetRow null out rejected");

    // Clear / ResetForRematch wipe rows.
    st.Clear();
    Expect(!st.GetRow(0, &out) && !st.GetRow(1, &out), "Clear wipes both sides");
    Expect(st.AcceptInbound(MakeRow(0, 5, 1, 50)), "post-Clear seq resets (any accepted)");
    st.ResetForRematch();
    Expect(!st.GetRow(0, &out), "ResetForRematch wipes");

    // Local change detection + per-side monotonic seq assignment.
    palette::Store lo;
    P::PaletteBlobBody L = MakeRow(0, 3, 1, 999);
    Expect(lo.SetLocalRow(&L) && L.seq == 1, "first local -> seq 1");
    P::PaletteBlobBody L2 = L;  // identical content
    Expect(!lo.SetLocalRow(&L2), "unchanged local -> no send");
    Expect(L2.seq == 1, "unchanged keeps prior seq");
    L2.colorSlot = 4;  // content change
    Expect(lo.SetLocalRow(&L2) && L2.seq == 2, "changed local -> seq 2");
    // A change in ONLY the seq field must not count as a content change.
    P::PaletteBlobBody L3 = L2;
    L3.seq = 12345;
    Expect(!lo.SetLocalRow(&L3), "seq-only change is not a content change");
    // Per-side outbound seq independence.
    P::PaletteBlobBody R = MakeRow(1, 9, 0, 0);
    Expect(lo.SetLocalRow(&R) && R.seq == 1, "side1 outbound seq independent");
}

// --- channel test scaffolding ----------------------------------------------
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

    // --- gating: flag OFF means fully inert ---
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

    // --- arm with flag ON ---
    SetFlag(true);
    ArmChannel(ch, 0);
    Expect(ch.IsActive() && ch.State() == HandshakeState::Probing,
           "armed -> Probing");

    // Tick emits a Hello; capture our nonce from it.
    ch.Tick(1000);
    Expect(g_sent.size() == 1, "first Tick emits one Hello");
    P::HelloBody ourHello{};
    Expect(P::ParseHello(g_sent[0].data(), g_sent[0].size(), &ourHello),
           "emitted frame is a Hello");
    const std::uint32_t ourNonce = ourHello.nonce;

    // A too-soon Tick does not re-emit (throttle).
    ch.Tick(1100);
    Expect(g_sent.size() == 1, "throttled: no re-emit before interval");

    // Ack with a WRONG nonce does not confirm.
    g_sent.clear();
    std::uint8_t ack[P::kMaxFrameBytes];
    std::size_t an = P::BuildAck(P::kFeaturePalettes, ourNonce ^ 0x1u, ack, sizeof ack);
    Expect(ch.OnInboundDatagram(ack, an), "inbound consumed (wrong-nonce ack)");
    Expect(ch.State() == HandshakeState::Probing, "wrong-nonce ack: still Probing");

    // Ack with the correct nonce confirms.
    an = P::BuildAck(P::kFeaturePalettes, ourNonce, ack, sizeof ack);
    Expect(ch.OnInboundDatagram(ack, an), "inbound consumed (correct ack)");
    Expect(ch.State() == HandshakeState::Confirmed, "correct ack -> Confirmed");

    // Now local palette submissions transmit.
    g_sent.clear();
    P::PaletteBlobBody local = MakeRow(0, 4, 2, 0);
    ch.SubmitLocalPaletteRow(local);
    Expect(g_sent.size() == 1, "Confirmed: palette submit transmits");
    P::PaletteBlobBody sent{};
    Expect(P::ParsePaletteBlob(g_sent[0].data(), g_sent[0].size(), &sent)
               && sent.side == 0 && sent.charId == 4 && sent.seq == 1,
           "transmitted palette body correct, seq assigned");
    // Unchanged resubmit does not transmit.
    ch.SubmitLocalPaletteRow(local);
    Expect(g_sent.size() == 1, "unchanged palette: no re-transmit");
    // Changed resubmit transmits with next seq.
    local.colorSlot = 5;
    ch.SubmitLocalPaletteRow(local);
    Expect(g_sent.size() == 2, "changed palette: re-transmit");

    // Inbound peer palette is cached and retrievable.
    P::PaletteBlobBody peer = MakeRow(1, 11, 3, 7);
    std::uint8_t pbuf[P::kMaxFrameBytes];
    std::size_t pn = P::BuildPaletteBlob(peer, pbuf, sizeof pbuf);
    Expect(ch.OnInboundDatagram(pbuf, pn), "peer palette consumed");
    P::PaletteBlobBody peerGot{};
    Expect(ch.GetPeerPaletteRow(1, &peerGot) && peerGot.charId == 11,
           "peer palette cached");
    ch.ResetPaletteExchangeForRematch();
    Expect(!ch.GetPeerPaletteRow(1, &peerGot), "rematch reset clears peer rows");

    // Malformed-but-ours inbound is still consumed (kept from Revival), no crash.
    std::uint8_t junk[P::kMaxFrameBytes];
    std::memcpy(junk, hello, hn);
    junk[P::kEnvelopeBytes + 4] = 0xEE;  // bad version
    Expect(ch.OnInboundDatagram(junk, hn), "malformed ours still consumed");

    // --- peer-initiated handshake: their Hello -> our Ack + Confirmed ---
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

    // --- spectator (side -1) never advertises a local row ---
    ArmChannel(ch, -1);
    // Force Confirmed via a peer hello, then try to submit.
    ch.OnInboundDatagram(peerHello, phn);
    g_sent.clear();
    P::PaletteBlobBody specRow = MakeRow(0, 2, 1, 0);
    ch.SubmitLocalPaletteRow(specRow);
    Expect(g_sent.empty(), "spectator never transmits a local row");

    // --- probe timeout to VanillaPeer (with a working sink) ---
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

    // --- no sink installed: no spin to VanillaPeer, stays Probing ---
    ch.End();
    ch.SetSendSink(&DropSink, nullptr);
    ch.Begin(0);
    for (std::uint32_t t = 1000; t <= 5000; t += 400) ch.Tick(t);
    Expect(ch.State() == HandshakeState::Probing,
           "no transport: stays Probing, never false-times-out");

    // --- no sink at all (null): entry points do not crash ---
    ch.End();
    ch.SetSendSink(nullptr, nullptr);
    ch.Begin(0);
    ch.Tick(1000);
    ch.SubmitLocalPaletteRow(MakeRow(0, 1, 0, 0));
    Expect(ch.State() == HandshakeState::Probing, "null sink: no crash, Probing");

    // --- mid-session flag toggle-off quiesces at next Tick ---
    ArmChannel(ch, 0);
    Expect(ch.IsActive(), "armed before toggle");
    SetFlag(false);
    ch.Tick(1000);
    Expect(!ch.IsActive(), "flag toggled off -> Tick disarms");
    SetFlag(true);  // restore for any later tests

    // --- End() is idempotent ---
    ch.End();
    ch.End();
    Expect(!ch.IsActive(), "double End safe");
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

void TestSource()
{
    namespace SRC = netplay::interop::source;

    // char-id table
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

    // relative path
    std::string p;
    Expect(SRC::BuildPalRelPath(0, 0, &p) && p == "nanase\\nanase1.pal",
           "path nanase slot0 -> nanase1.pal");
    Expect(SRC::BuildPalRelPath(1, 5, &p) && p == "ayu\\ayu6.pal",
           "path ayu slot5 -> ayu6.pal (1-based on disk)");
    Expect(!SRC::BuildPalRelPath(99, 0, &p), "path unknown char -> false");
    Expect(!SRC::BuildPalRelPath(0, 0, nullptr), "path null out -> false");

    // raw .pal file IO
    std::uint8_t raw[netplay::interop::protocol::kPaletteRawBytes];
    WriteBytes("interop_tmp_pal.bin", 120, 0, 1);
    Expect(SRC::LoadRawPalFile("interop_tmp_pal.bin", raw), "load 120-byte pal");
    Expect(raw[0] == 0 && raw[119] == 119, "pal bytes correct");
    Expect(!SRC::LoadRawPalFile("interop_tmp_missing.bin", raw), "missing -> false");
    Expect(!SRC::LoadRawPalFile(nullptr, raw), "null path -> false");
    Expect(!SRC::LoadRawPalFile("interop_tmp_pal.bin", nullptr), "null out -> false");
    WriteBytes("interop_tmp_short.bin", 50, 0, 1);
    Expect(!SRC::LoadRawPalFile("interop_tmp_short.bin", raw), "short (<120) -> false");
    WriteBytes("interop_tmp_big.bin", 200, 0, 1);
    Expect(SRC::LoadRawPalFile("interop_tmp_big.bin", raw) && raw[119] == 119,
           "larger file reads first 120");
    std::remove("interop_tmp_pal.bin");
    std::remove("interop_tmp_short.bin");
    std::remove("interop_tmp_big.bin");

    // AssembleRow custom vs stock/CLEAR
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
    // custom=true but null bytes -> still a stock-content row (no copy)
    P::PaletteBlobBody cn = SRC::AssembleRow(0, 1, 0, true, nullptr);
    allZero = true;
    for (int i = 0; i < 120; ++i) if (cn.rawBgr[i] != 0) allZero = false;
    Expect(allZero, "custom flag + null bytes leaves rawBgr zeroed");

    // LoadLocalRow against a nested temp dir
    _mkdir("interop_tmp_base");
    _mkdir("interop_tmp_base\\nanase");
    WriteBytes("interop_tmp_base\\nanase\\nanase1.pal", 120, 200, -1);
    P::PaletteBlobBody lr{};
    Expect(SRC::LoadLocalRow("interop_tmp_base", 0, 0, 0, &lr) && lr.sourceFlag == 1,
           "LoadLocalRow finds custom .pal");
    Expect(lr.rawBgr[0] == 200 && lr.charId == 0 && lr.colorSlot == 0,
           "LoadLocalRow loaded bytes + fields");
    // trailing-slash base dir also works
    P::PaletteBlobBody lr2{};
    Expect(SRC::LoadLocalRow("interop_tmp_base\\", 0, 0, 0, &lr2)
               && lr2.sourceFlag == 1,
           "LoadLocalRow tolerates trailing slash");
    // missing .pal -> stock CLEAR, out still filled, returns false
    P::PaletteBlobBody mr{};
    Expect(!SRC::LoadLocalRow("interop_tmp_base", 1, 2, 4, &mr)
               && mr.sourceFlag == 0 && mr.charId == 2 && mr.colorSlot == 4,
           "LoadLocalRow missing -> stock CLEAR, fields set");
    // unknown char -> stock CLEAR (no path), returns false
    P::PaletteBlobBody ur{};
    Expect(!SRC::LoadLocalRow("interop_tmp_base", 0, 99, 0, &ur)
               && ur.sourceFlag == 0,
           "LoadLocalRow unknown char -> stock CLEAR");
    std::remove("interop_tmp_base\\nanase\\nanase1.pal");
    _rmdir("interop_tmp_base\\nanase");
    _rmdir("interop_tmp_base");

    // --- .pal header-byte detection (script files are 1 pad + 120 data) ---
    // 121-byte file: byte 0 is padding, data starts at byte 1.
    WriteBytes("interop_tmp_hdr.bin", 121, 7, 1);   // file[0]=7, file[1]=8...
    Expect(SRC::LoadRawPalFile("interop_tmp_hdr.bin", raw), "121-byte pal loads");
    Expect(raw[0] == 8 && raw[119] == 127,
           "121-byte pal skips the 1-byte header");
    // 120-byte raw file: no header, data starts at byte 0.
    WriteBytes("interop_tmp_raw.bin", 120, 7, 1);
    Expect(SRC::LoadRawPalFile("interop_tmp_raw.bin", raw) && raw[0] == 7,
           "120-byte raw pal reads from byte 0");
    // 121-byte but too little data after header would be 120 exactly - ok; a
    // 100-byte file with %3==1 (i.e. 100%3==1) must fail (header + <120 data).
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

    // Baseline palette: distinct non-black colors everywhere.
    // raw[i*3+0]=B, +1=G, +2=R for sprite index i+1 (1-based indices).
    std::uint8_t pal[120];
    for (int i = 0; i < 40; ++i)
    {
        pal[i * 3 + 0] = static_cast<std::uint8_t>(10 + i);        // B
        pal[i * 3 + 1] = static_cast<std::uint8_t>(60 + i);        // G
        pal[i * 3 + 2] = static_cast<std::uint8_t>(160 + i);       // R
    }

    // Bad args / unknown character.
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

    // akiko produces writes; case-insensitive lookup matches.
    const std::size_t nAkiko = RM::RemapSpriteToPortrait(pal, "akiko", out, 40);
    Expect(nAkiko > 0, "akiko remap produces writes");
    RM::PortraitColor out2[RM::kMaxPortraitColors];
    Expect(RM::RemapSpriteToPortrait(pal, "AKIKO", out2, 40) == nAkiko,
           "case-insensitive char name");

    // Direct 1:1 parity: akiko 'eyes' = sprite {17,18} -> portrait [17,18].
    // Expect portIdx 17 to carry sprite 17's exact color (BGR -> r/g/b swap).
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

    // Gradient endpoints are exact control points: akiko 'skin' sprite
    // {2,3,4,5} -> portrait [2,4]; port 2 == sprite2 color, port 4 == sprite5.
    for (std::size_t i = 0; i < nAkiko; ++i)
    {
        if (out[i].portIdx == 2)
        {
            // The start point goes through an RGB->HSL->RGB round-trip (the
            // interpolate branch with localT=0, exactly like the offline mod),
            // so it is near-equal, not byte-exact.
            Expect(std::abs(int(out[i].b) - int(pal[1 * 3 + 0])) <= 2
                       && std::abs(int(out[i].r) - int(pal[1 * 3 + 2])) <= 2,
                   "gradient start ~= first control point (HSL round-trip)");
        }
        if (out[i].portIdx == 4)
        {
            // The end point hits the direct-copy branch: byte-exact.
            Expect(out[i].b == pal[4 * 3 + 0] && out[i].r == pal[4 * 3 + 2],
                   "gradient end == last control point");
        }
    }

    // Reversed portrait direction: akane 'acc' sprite {23,24,25} -> portrait
    // [13,9] (descending). Endpoints: port 13 == sprite 23, port 9 == sprite 25.
    const std::size_t nAkane = RM::RemapSpriteToPortrait(pal, "akane", out, 40);
    Expect(nAkane > 0, "akane remap produces writes");
    bool saw13 = false, saw9 = false;
    for (std::size_t i = 0; i < nAkane; ++i)
    {
        if (out[i].portIdx == 13)
        {
            saw13 = true;
            // Start point: HSL round-trip tolerance (see gradient note above).
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

    // Black-unset rule: blacken akiko 'acc' sprite colors {27,28} (>20 => unset)
    // -> that area is skipped, others still emit.
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

    // All-black palette: everything skipped.
    std::uint8_t palZ[120] = {};
    Expect(RM::RemapSpriteToPortrait(palZ, "akiko", out, 40) == 0,
           "all-black palette -> 0 writes");

    // Alias table: nanase==rumi, exnanase==doppel produce identical output.
    RM::PortraitColor a1[40], a2[40];
    const std::size_t n1 = RM::RemapSpriteToPortrait(pal, "nanase", a1, 40);
    const std::size_t n2 = RM::RemapSpriteToPortrait(pal, "rumi", a2, 40);
    Expect(n1 == n2 && n1 > 0
               && std::memcmp(a1, a2, n1 * sizeof(RM::PortraitColor)) == 0,
           "nanase/rumi aliases identical");

    // Cap enforcement.
    Expect(RM::RemapSpriteToPortrait(pal, "akiko", out, 3) == 3,
           "cap truncates output");

    // HSL round-trip sanity (float/script math tolerance: +-2 per channel).
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
}
// Exercises the cross-process SPSC ring in isolation (no shared mapping needed -
// Push/Pop operate on a caller-owned Ring). Covers round-trip, FIFO order, full
// (one slot reserved), wrap-around past the mask, oversize reject, empty, and a
// corrupt slot length being skipped.
void TestIpcRing()
{
    namespace R = netplay::interop::ipc;
    auto ring = std::make_unique<R::Ring>();
    std::memset(ring.get(), 0, sizeof(R::Ring));

    std::uint8_t out[R::kSlotBytes];

    // Empty pop.
    Expect(R::Pop(*ring, out, sizeof(out)) == 0, "ipc: pop empty -> 0");

    // Round-trip one frame.
    const std::uint8_t f1[] = {1, 2, 3, 4, 5};
    Expect(R::Push(*ring, f1, sizeof(f1)), "ipc: push ok");
    const std::uint32_t n1 = R::Pop(*ring, out, sizeof(out));
    Expect(n1 == sizeof(f1) && std::memcmp(out, f1, n1) == 0,
           "ipc: round-trip content");
    Expect(R::Pop(*ring, out, sizeof(out)) == 0, "ipc: drained -> empty");

    // Bad args.
    Expect(!R::Push(*ring, nullptr, 4), "ipc: push null -> false");
    Expect(!R::Push(*ring, f1, 0), "ipc: push zero-len -> false");
    std::uint8_t big[R::kSlotBytes + 1] = {};
    Expect(!R::Push(*ring, big, sizeof(big)), "ipc: push oversize -> false");

    // Fill to capacity: kRingSlots-1 usable (one reserved to disambiguate full).
    std::memset(ring.get(), 0, sizeof(R::Ring));
    std::uint32_t pushed = 0;
    for (std::uint32_t i = 0; i < R::kRingSlots + 4u; ++i)
    {
        const std::uint8_t b = static_cast<std::uint8_t>(i);
        if (R::Push(*ring, &b, 1)) ++pushed;
    }
    Expect(pushed == R::kRingSlots - 1u, "ipc: capacity is slots-1");

    // FIFO drain of the filled ring.
    bool order = true;
    for (std::uint32_t i = 0; i < pushed; ++i)
    {
        const std::uint32_t n = R::Pop(*ring, out, sizeof(out));
        if (n != 1 || out[0] != static_cast<std::uint8_t>(i)) order = false;
    }
    Expect(order, "ipc: FIFO order preserved");
    Expect(R::Pop(*ring, out, sizeof(out)) == 0, "ipc: empty after drain");

    // Wrap-around: many push/pop cycles exceed kRingSlots, exercising the mask.
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

    // Corrupt slot length is skipped (defends against a torn cross-process write).
    std::memset(ring.get(), 0, sizeof(R::Ring));
    Expect(R::Push(*ring, f1, sizeof(f1)), "ipc: push before corrupt");
    ring->slots[ring->tail & R::kRingMask].len = R::kSlotBytes + 99u;  // corrupt
    Expect(R::Pop(*ring, out, sizeof(out)) == 0, "ipc: corrupt len -> skipped");
    Expect(R::Pop(*ring, out, sizeof(out)) == 0, "ipc: nothing left after skip");
}
} // namespace

int main()
{
    TestProtocol();
    TestStore();
    TestSource();
    TestRemap();
    TestChannel();
    TestIpcRing();
    if (g_failures != 0)
    {
        std::cerr << g_failures << " interop test(s) failed\n";
        return 1;
    }
    std::cout << "interop tests passed\n";
    return 0;
}
