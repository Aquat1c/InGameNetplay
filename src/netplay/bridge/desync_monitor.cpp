// Desync detection and forensics. See include/netplay/bridge/desync_monitor.h.

// inet_addr is the XP-compatible parser (inet_pton needs Vista+).
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>

#include "netplay/bridge/desync_monitor.h"
#include "netplay/bridge/netplay_state_export.h"
#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/mod_settings.h"

#include "logger.h"
#include "mod_version.h"

#include <MinHook.h>

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <windows.h>

namespace netplay::bridge::desync_monitor
{
namespace
{
using netplay::bridge::takeover::SafeReadInt;
using netplay::bridge::takeover::SafeReadPtr;
using netplay::bridge::takeover::IsReadableRange;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// EFZ.exe fixed addresses (EFZ.exe never changes across Revival versions).
constexpr uintptr_t kScreenTableAddr = 0x00790110u;
constexpr uintptr_t kScreenIndexAddr = 0x00790148u;
constexpr uint32_t kOffsetGameSystem = 0x1C;
constexpr uint32_t kOffsetBattleP1Char = 12;
constexpr uint32_t kOffsetBattleP2Char = 16;

// Sync-relevant regions checksummed and recorded per frame.
constexpr uint32_t kGameSysSliceOffset = 4864;  // mode/winner/counter area
constexpr uint32_t kGameSysSliceSize = 160;
constexpr uint32_t kBattleSliceOffset = 0x430;  // battle screen state area
constexpr uint32_t kBattleSliceSize = 256;
constexpr uint32_t kCharSliceSize = 0x400;      // per-character state window

constexpr size_t kRecordRegionBytes =
    kGameSysSliceSize + kBattleSliceSize + 2u * kCharSliceSize;

// Ring depths. The causal window freezes at the first comparable selected-
// field/RNG difference. A separate fixed array below retains the complete
// selected-gameplay confirmation run even when that begins much later.
constexpr size_t kRegionRingDepth = 512;
constexpr size_t kSampleRingDepth = 512;
constexpr size_t kCompareLogDepth = 64;
constexpr size_t kPostEvidenceDepth = 64;
constexpr size_t kEffectTraceSlots = 64;

// A diagnostic report is armed after this many exact, contiguous gameplay-
// checksum mismatches.  This is not an RNG classifier: an RNG-only offset
// may reconverge or remain latent until later gameplay consumes randomness.
// The native warning remains non-fatal and the session is never modified.
// Must stay below kRegionRingDepth so the dump retains pre-divergence frames.
constexpr int kConfirmMismatchFrames = 96;
constexpr size_t kGameplayRunDepth = kConfirmMismatchFrames;
static_assert(
    kGameplayRunDepth >= static_cast<size_t>(kConfirmMismatchFrames),
    "gameplay confirmation evidence must retain the complete run");

// NOTE (2026-07-19 audit correction): the char-vs-char collision/spawn
// math consumes the SCREEN-space position pair char+32/+40 plus FACING
// char+80; the "+216/+224" tokens in calculateCharacterHitbox's decomp are
// stack aliases of a collision input struct, NOT the fields it reads.
// EFZ effect-ring layout inside gameSystem: the presentation-effect state
// that owns the RNG-consuming particles (type-47 bounce etc., see
// docs/NAYUKI_AWAKE_AIR_THROW_RNG_DESYNC.md).  A per-frame hash of the
// active slots is exchanged alongside the gameplay checksum so cross-peer
// comparison can attribute a divergence to the effect layer BEFORE it
// surfaces through the shared RNG stream.  Layout provenance: efz.c
// effect-ring allocation/processing (sub_4075B0 family).
constexpr uint32_t kEffectRingSlots = 640;
constexpr uintptr_t kEffectAllocCursorOffset = 4992;  // WORD next-alloc cursor
constexpr uintptr_t kEffectProcCursorOffset = 4994;   // WORD oldest/processing
constexpr uintptr_t kEffectActiveFlagBase = 4996;     // + 4*i DWORD per slot
constexpr uintptr_t kEffectStatusBase = 7556;         // + 4*i DWORD per slot
constexpr uintptr_t kEffectRecordBase = 10760;        // + 112*i per-slot record
constexpr uintptr_t kEffectRecordStride = 112;
constexpr size_t kEffectRecordArenaBytes =
    static_cast<size_t>(kEffectRingSlots) * kEffectRecordStride;
// Record-relative offsets of the deterministic per-particle fields.
constexpr uintptr_t kEffectFieldBehaviorId = 0;   // WORD
constexpr uintptr_t kEffectFieldAnimFrame = 2;    // WORD
constexpr uintptr_t kEffectFieldAnimTick = 4;     // WORD
constexpr uintptr_t kEffectFieldPosX = 24;        // double
constexpr uintptr_t kEffectFieldPosY = 32;        // double
constexpr uintptr_t kEffectFieldVelX = 40;        // double
constexpr uintptr_t kEffectFieldVelY = 48;        // double
constexpr uintptr_t kEffectFieldParameter = 76;   // DWORD, type-47 direction/branch data

// Narrow causal trace for the Awake-Nayuki fault. These fixed EFZ entry
// points are fingerprinted before MinHook is allowed to touch them. The
// hooks publish only into a preallocated ring; transport and disk work stay
// on the existing worker/teardown paths.
constexpr uintptr_t kInitializeEffectAddr = 0x00407100u;
constexpr uintptr_t kClearEffectAddr = 0x004072A0u;
constexpr uintptr_t kProcessEffectsAddr = 0x004075B0u;
constexpr uintptr_t kProcessEffectAddr = 0x00401C20u;
constexpr uint16_t kTargetEffectType = 47;
constexpr size_t kType47TraceDepth = 65536;
constexpr size_t kType47TraceMask = kType47TraceDepth - 1;
// Preserve half a ring before and half after the forensic trigger. After the
// post budget fills, callbacks keep executing the same capture path but write
// to a small discard ring so a long battle cannot overwrite the onset.
constexpr size_t kType47PostTriggerEvents = kType47TraceDepth / 2;
constexpr size_t kType47DiscardDepth = 256;
constexpr size_t kType47DiscardMask = kType47DiscardDepth - 1;
constexpr size_t kType47FrameAnchorDepth = 1024;
constexpr size_t kType47FrameAnchorMask = kType47FrameAnchorDepth - 1;
static_assert((kType47TraceDepth & kType47TraceMask) == 0,
              "type-47 trace depth must be a power of two");
static_assert((kType47DiscardDepth & kType47DiscardMask) == 0,
              "type-47 discard depth must be a power of two");
static_assert((kType47FrameAnchorDepth & kType47FrameAnchorMask) == 0,
              "type-47 frame-anchor depth must be a power of two");

enum Type47TracePhase : uint8_t
{
    kTracePassBegin = 1,
    kTraceBeforeUpdate = 2,
    kTraceAfterUpdate = 3,
    kTraceCreated = 4,
    kTraceCleared = 5,
    kTracePassEnd = 6,
    // Revival rollback savestate boundaries (profile-verified detours; 1.02h
    // only until other builds' entries are byte-verified).  snapshot_save is
    // published at save ENTRY (the state being captured); snapshot_load is
    // published after the restore RETURNS (the state as restored).  Together
    // with pass_begin/pass_end they show which effect passes ran between
    // which Save/Load pair - the desync4 question "what did the host's Load
    // restore vs what its Save captured".
    kTraceSnapshotSave = 7,
    kTraceSnapshotLoad = 8,
    // Character context at type-47 creation (one row per effect pass that
    // creates type-47s): published with the same field reuse as the
    // snapshot markers - anim_frame/anim_tick = P1 anchors, parameter = P2
    // anchors, x/y = P1 pos, vx/vy = P2 pos - PLUS the alloc/proc cursor
    // columns carry P1/P2 animation-state words (see PublishCharContext).
    // Purpose: the (-17,-15) spawn shift with equal positions and constant
    // anchors leaves the attacker's animation-frame-indexed PAT row as the
    // remaining variable input; these rows record that vintage per pass.
    kTraceCharContext = 9,
};

struct Type47TraceEvent
{
    volatile LONG stamp; // published last; logical sequence number
    uint32_t sessionGeneration;
    uint32_t passSerial; // local diagnostic ordinal, not peer-canonical
    int32_t frame;
    int32_t commitFrame;
    int32_t rngState;
    uint16_t slot;
    uint16_t allocCursor;
    uint16_t procCursor;
    uint16_t behavior;
    uint16_t animFrame;
    uint16_t animTick;
    uint8_t phase;
    uint8_t active;
    uint32_t status;
    uint32_t parameter;
    uint64_t xBits;
    uint64_t yBits;
    uint64_t vxBits;
    uint64_t vyBits;
    // Verbose char vintage (ExperimentalCaptureVerboseDump): both
    // characters' PAT-row-selecting animation state and its gating
    // counters, sampled at event time.  0xFFFF = not sampled.
    uint16_t p1Move;
    uint16_t p1AnimFrame;
    uint16_t p1AnimTick;
    uint16_t p1Freeze;
    uint16_t p1Contact;
    uint16_t p2Move;
    uint16_t p2AnimFrame;
    uint16_t p2AnimTick;
    uint16_t p2Freeze;
    uint16_t p2Contact;
    // Second character position representation: the SCREEN-space pair at
    // char+32/+40 (0..639 range; what Revival's Sync record logs), distinct
    // from the +216/+224 world pair the collision boxes use.  "Sync
    // positions equal" never proved the +216 pair equal - record both.
    uint64_t p1ScreenXBits;
    uint64_t p1ScreenYBits;
    uint64_t p2ScreenXBits;
    uint64_t p2ScreenYBits;
    // --- Full-audit batch (2026-07-19 logging audit) ---------------------
    // FPU control state at event time: the ONE spawn-math input a memcpy
    // savestate cannot restore; normalized only at batch boundaries, never
    // between re-simulated frames inside one rollback batch.
    uint16_t fpuX87Cw;
    uint32_t fpuMxcsr;
    // Persistent facing sign char+80: flips the attack-box side (midpoint X)
    // and the created effect's direction word; +232 in the decomp is only a
    // stack alias of this field inside the collision input struct.
    int32_t p1Facing;
    int32_t p2Facing;
    // Timing gates deciding WHICH tick contact resolves (companions to the
    // already-logged +330 freeze / +360 contact).
    uint16_t p1Hitstun;   // +316
    uint16_t p1Freeze2;   // +332 (opponent-secondary freeze)
    uint16_t p1MoveTimer; // +364 (active-frame/move timer)
    uint16_t p2Hitstun;
    uint16_t p2Freeze2;
    uint16_t p2MoveTimer;
    // GLOBAL collision-enable gate (critic's top find): the master switch
    // gating handlePlayerCollisions per sub-step (efz.c:198350-198355),
    // plus its round-flow neighbors.  A one-tick asymmetry here moves the
    // hit-effect creation to a different frame with everything else equal.
    uint8_t collGate40;   // gameSys+4940
    uint8_t collGate44;   // gameSys+4944
    uint32_t collGate48;  // gameSys+4948
    uint8_t roundNo;      // gameSys+4952
    uint32_t readyMask;   // gameSys+4956
    // Replay word-stream selector + reader counter (gameSys+82563/+82564).
    uint8_t wordstreamMode;
    uint32_t wordstreamCtr;
    // Physics velocities: equal position ENDPOINTS can hide a different
    // at-collision value; kb = cross-object knockback writes.
    uint64_t p1VxBits;    // +48
    uint64_t p1VyBits;    // +56
    uint64_t p1KbxBits;   // +192
    uint64_t p1KbyBits;   // +200
    uint64_t p1MomxBits;  // +176
    uint64_t p2VxBits;
    uint64_t p2VyBits;
    uint64_t p2KbxBits;
    uint64_t p2KbyBits;
    uint64_t p2MomxBits;
    uint32_t pushFlags;   // (+208 p1 << 16) | (+208 p2 & 0xFFFF)
    // Raw per-(move,animFrame) box anchors (packed 204<<16|206) + FNV of the
    // resolved 200-byte box-offset frame-data row per char: proves the box
    // geometry the midpoint intersects is byte-identical across passes.
    uint32_t p1Anchor;
    uint32_t p2Anchor;
    uint32_t p1BoxFnv;
    uint32_t p2BoxFnv;
    // Corroborators: victim reaction branch +300, throw/mash counter
    // +12600, meter +328.
    int32_t p1ReactFlag;
    int32_t p2ReactFlag;
    int32_t p1ThrowCtr;
    int32_t p2ThrowCtr;
    uint16_t p1Meter;
    uint16_t p2Meter;
    // Effect-record direction word (+76) on slot rows; ring occupancy count
    // on created/char_context rows (slot placement discriminator).
    int16_t effDir;
    uint16_t activeCount;
    // Per-marker char-object pointer identity: sim pair (bc+12/+16) vs the
    // savestate's double-indirect pair (*(bc+20)/*(bc+24)).  A wrong-object
    // restore is the cleanest mechanism for "Sync matches but the sim reads
    // unrestored state".
    uint64_t simPtrs;   // (p1 << 32) | p2
    uint64_t savePtrs;  // (saveP1 << 32) | saveP2
    // Session frame captured immediately BEFORE the snapshot restore ran
    // (snapshot_load rows only; 0xFFFFFFFF elsewhere).
    uint32_t loadFrom;
    // Rollback re-execution discriminator: 1 when this event's frame is
    // below the maximum frame already seen this session (a re-simulated
    // frame), 0 for first executions.
    uint8_t resim;
};

// Lock-free index from a logical frame to the first retained type-47 event
// published for that frame. It avoids scanning the 65k-event trace ring while
// the comparison lock is held at the exact moment a divergence is observed.
struct Type47FrameAnchor
{
    volatile LONG stamp; // event sequence, published last
    uint32_t sessionGeneration;
    int32_t frame;
    LONG firstSequence;
};

// The recorded windows contain heap pointers (character struct members,
// sprite/object pointers) that legitimately differ between the two machines.
// Checksums therefore only cover bytes that CHANGED at least once during the
// first N sampled frames of each battle: pointers are constant for the whole
// match and drop out, while sync-relevant state (positions, HP, timers,
// meters) mutates constantly and stays in.  While the peers are in sync they
// observe identical byte changes, so both sides derive the same mask.
constexpr unsigned kMaskCalibrationSamples = 64;

// UDP side channel: host listens on gamePort + this offset.
constexpr uint16_t kSideChannelPortOffset = 2;
constexpr uint32_t kPacketMagic = 0x445A4645u; // 'EFZD' little-endian
// v4 adds host-authoritative forensic Trigger/TriggerAck/TriggerNack controls.
// Mixed builds remain passive through the exact version/schema checks.
constexpr uint8_t kPacketVersion = 4;
constexpr uint32_t kSchemaLayoutId = 0x34474645u; // 'EFG4': reviewed v4 semantics
constexpr int kSamplesPerPacket = 4;
constexpr int kHandshakeLeadFrames = 120;
constexpr int kHandshakeSafetyFrames = 30;
constexpr int kReorderWaitFrames = 8;
// Normal session teardown gives the diagnostic UDP channel a short bounded
// tail in which to flush queued samples and converge the forensic trigger.
// This path is opt-in with the monitor and never performs socket I/O on the
// simulation thread.
// Keep the peer responsive through at least one 500 ms Trigger retry after
// its first ACK. This closes the one-way-ACK loss hole without adding a new
// protocol round trip.
constexpr DWORD kTransportDrainQuietMs = 650u;
constexpr DWORD kTransportDrainDeadlineMs = 1600u;
constexpr DWORD kTransportForcedStopWaitMs = 250u;

enum class PacketKind : uint8_t
{
    Hello = 1,
    HelloAck = 2,
    Ready = 3,
    Start = 4,
    StartAck = 5,
    Samples = 6,
    Abort = 7,
    Trigger = 8,
    TriggerAck = 9,
    TriggerNack = 10,
};

enum class EvidenceLayer : uint8_t
{
    SelectedGameplay = 1,
    EffectProjection = 2,
    RngScalar = 3,
    EffectAndRng = 4,
};

const char* EvidenceLayerName(EvidenceLayer layer)
{
    switch (layer)
    {
    case EvidenceLayer::SelectedGameplay:
        return "selected-gameplay";
    case EvidenceLayer::EffectProjection:
        return "effect-projection";
    case EvidenceLayer::RngScalar:
        return "rng-scalar";
    case EvidenceLayer::EffectAndRng:
        return "effect+rng";
    default:
        return "selected-state";
    }
}

bool IsEvidenceLayerCode(uint8_t code)
{
    return code >= static_cast<uint8_t>(EvidenceLayer::SelectedGameplay)
        && code <= static_cast<uint8_t>(EvidenceLayer::EffectAndRng);
}

#pragma pack(push, 1)
struct WirePacket
{
    uint32_t magic;
    uint8_t version;
    uint8_t kind;
    uint8_t role;
    uint8_t count; // sample count, or EvidenceLayer for Trigger controls
    uint32_t sessionNonce;
    uint32_t schemaId;
    int32_t startFrame; // negotiated start, or evidence frame for Trigger controls
    uint32_t maskHash;
    uint16_t maskByteCount;
    uint16_t battleEpoch;
    struct
    {
        int32_t frame;
        uint32_t checksum;
        uint32_t effectHash;
        int32_t rngState;
    } samples[kSamplesPerPacket];
};
#pragma pack(pop)
static_assert(sizeof(WirePacket) == 92, "wire ABI changed; bump protocol/schema");

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

struct EffectProjectionRecord
{
    uint16_t slot = 0;
    uint16_t behaviorId = 0;
    uint16_t animFrame = 0;
    uint16_t animTick = 0;
    uint32_t activeFlag = 0;
    uint32_t status = 0;
    uint32_t parameter = 0;
    double x = 0.0;
    double y = 0.0;
    double vx = 0.0;
    double vy = 0.0;
};

struct FrameRecord
{
    int frame = -1;
    uint32_t checksum = 0;
    uint16_t p1Input = 0;
    uint16_t p2Input = 0;
    uint8_t validMask = 0; // bit0 gameSys, bit1 battle, bit2 charP1, bit3 charP2
    uint32_t srcGameSys = 0;
    uint32_t srcBattle = 0;
    uint32_t srcCharP1 = 0;
    uint32_t srcCharP2 = 0;
    // Per-frame Revival session context for the dump.
    int commitFrame = -1;
    int syncFeed = -1;
    int localLen = -1;
    int remoteLen = -1;
    int pingMs = -1;
    uint32_t effectHash = 0;
    int32_t rngState = -1;
    uint16_t effectAllocCursor = 0;
    uint16_t effectProcCursor = 0;
    uint16_t effectTraceCount = 0;
    uint16_t effectTraceOverflow = 0;
    EffectProjectionRecord effectTrace[kEffectTraceSlots];
    uint8_t bytes[kRecordRegionBytes];
};

struct ChecksumSample
{
    int frame = -1;
    uint32_t checksum = 0;
    uint32_t effectHash = 0;
    int32_t rngState = -1;
    uint32_t maskHash = 0;
    uint16_t maskByteCount = 0;
};

struct CompareEntry
{
    int frame = -1;
    uint32_t localSum = 0;
    uint32_t remoteSum = 0;
    bool match = false;
    uint32_t localEffect = 0;
    uint32_t remoteEffect = 0;
    int32_t localRng = -1;
    int32_t remoteRng = -1;
};

CRITICAL_SECTION g_lock;
bool g_lockInited = false;

volatile LONG g_sessionActive = 0;
volatile LONG g_transportDraining = 0;
bool g_dumped = false;
bool g_dumpPending = false;
int g_role = -1;             // NetbridgeRole numeric (0 host, 1 join)
uint16_t g_hostPort = 0;
char g_peerAddress[64] = {};
char g_nickname[64] = {};

FrameRecord g_regionRing[kRegionRingDepth];
size_t g_regionRingNext = 0;

ChecksumSample g_localRing[kSampleRingDepth];
size_t g_localRingNext = 0;

ChecksumSample g_remoteRing[kSampleRingDepth];
size_t g_remoteRingNext = 0;

CompareEntry g_compareLog[kCompareLogDepth];
size_t g_compareLogNext = 0;

// The first comparable post-calibration selected-field/RNG difference freezes the rotating
// pre-window without copying it on the hot path. Subsequent records go into
// bounded append-only post arrays, so session-end disk I/O cannot lose the
// onset even when play continues for thousands of frames.
FrameRecord g_postEvidenceFrames[kPostEvidenceDepth];
size_t g_postEvidenceFrameCount = 0;
CompareEntry g_postEvidenceCompare[kPostEvidenceDepth];
size_t g_postEvidenceCompareCount = 0;
CompareEntry g_gameplayMismatchRun[kGameplayRunDepth];
size_t g_gameplayMismatchRunCount = 0;
bool g_gameplayMismatchRunFrozen = false;
bool g_evidenceTriggered = false;
int g_evidenceTriggerFrame = -1;
LONG g_evidenceBattleEpoch = 0;
char g_evidenceTriggerLayer[24] = {};
uint8_t g_evidenceTriggerLayerCode = 0;
bool g_evidenceTriggerLocallyRaised = false;
bool g_evidenceTriggerAcknowledged = false;
bool g_evidenceTriggerNacked = false;
DWORD g_lastEvidenceTriggerSendTick = 0;
size_t g_evidenceRegionNext = 0;
size_t g_evidenceCompareNext = 0;
uint8_t g_evidenceMask[kRecordRegionBytes] = {};
unsigned g_evidenceMaskByteCount = 0;
uint32_t g_evidenceMaskHash = 0;

int g_mismatchStreak = 0;
int g_firstMismatchFrame = -1;
int g_lastMismatchFrame = -1;
int g_lastComparedFrame = -1;
int g_nextCompareFrame = -1;
int g_highestLocalFrame = -1;
int g_highestRemoteFrame = -1;
volatile LONG g_desyncConfirmed = 0;
int g_confirmedFrame = -1;

// Layer-attribution runs (diagnostic only; window capture is driven solely
// by the selected gameplay checksum).  Begin/match-again pairs localize the
// first observed difference without claiming complete state convergence.
int g_effectLayerRun = 0;
int g_effectLayerFirstFrame = -1;
int g_rngLayerRun = 0;
int g_rngLayerFirstFrame = -1;

unsigned g_sampleCount = 0;
unsigned g_sentSampleCount = 0;
bool g_peerSeen = false;
DWORD g_sessionStartTick = 0;
bool g_peerAbsenceLogged = false;
uint32_t g_sessionNonce = 0;
int g_handshakePhase = 0;
DWORD g_lastControlSendTick = 0;
volatile LONG g_latestFrame = -1;
volatile LONG g_captureStartFrame = -1;
volatile LONG g_captureArmed = 0;
volatile LONG g_battleEpoch = 0;
bool g_inBattle = false;
bool g_maskMismatchLogged = false;
bool g_epochMismatchLogged = false;

// Render-owned bytes inside the recorded sim regions.  desync7 proved the
// gameplay projection can false-trigger on state that mutates every frame
// but tracks LOCAL render cadence, not synced simulation:
//   - gameSystem+4968: a frame-parity toggle that blinks 0/1 on both peers
//     with phase tied to local scheduling (peers ran in opposite phase for
//     hundreds of frames while effects, RNG, and native Sync all matched);
//   - battleScreen+0x04/+0x08/+0x0C: rendered-vs-elapsed frame accounting
//     (the +4/+C pair summed equal across peers while split 9 apart).
// These bytes always mutate, so calibration would always admit them; they
// are denied here instead.  Region layout: [0,160) = gameSystem slice from
// +4864, [160,416) = battleScreen slice from +0x430... note the battle
// slice starts at kBattleSliceOffset (0x430), so battleScreen+0x04 is NOT
// in the recorded window - only the gameSystem toggle needs denying from
// the recorded regions; the battleScreen counters were observed via the
// raw frame dump, not the checksum window.  Deny the toggle dword.
bool IsRenderOwnedRegionByte(size_t index)
{
    // gameSystem slice starts at gameSystem+4864; +4968 is slice offset 104.
    constexpr size_t kGameSysRenderToggleBegin = 4968 - 4864;
    constexpr size_t kGameSysRenderToggleEnd = kGameSysRenderToggleBegin + 4;
    return index >= kGameSysRenderToggleBegin
        && index < kGameSysRenderToggleEnd;
}

// Change-mask calibration state (single writer: the game thread).
uint8_t g_changeMask[kRecordRegionBytes];
uint8_t g_prevRegionBytes[kRecordRegionBytes];
bool g_prevRegionValid = false;
unsigned g_maskSamples = 0;
bool g_maskFrozen = false;
unsigned g_maskByteCount = 0;
uint32_t g_maskHash = 0;
bool g_leftBattleSinceLastSample = false;
int g_lastSampledFrame = -1;

// Validated once per stable battle object. The game thread is the sole
// writer/reader, so no synchronization is needed for these caches.
uintptr_t g_validatedEffectGameSys = 0;
bool g_effectArenaReadable = false;
uintptr_t g_validatedRngAddress = 0;
bool g_rngAddressReadable = false;

SOCKET g_socket = INVALID_SOCKET;
bool g_wsaStarted = false;
sockaddr_in g_peerEndpoint = {};
volatile LONG g_peerEndpointValid = 0;
HANDLE g_workerThread = nullptr;
// 0 = running, 1 = graceful drain requested, 2 = forced stop.
volatile LONG g_workerStop = 0;
volatile LONG g_workerDrainQuietUntilTick = 0;
volatile LONG g_workerDrainDeadlineTick = 0;

// Detailed trace state. EFZ membership is deliberately not mirrored in a
// side table: Revival restores the effect ring without replaying create/clear
// hooks, so every update derives type-47 membership from the live active flag
// and behavior ID. This keeps the observer rollback-safe.
Type47TraceEvent g_type47Trace[kType47TraceDepth];
Type47TraceEvent g_type47DiscardTrace[kType47DiscardDepth];
Type47FrameAnchor g_type47FrameAnchors[kType47FrameAnchorDepth];
volatile LONG g_type47TraceWriteSequence = 0;
volatile LONG g_type47TraceEnabled = 0;
volatile LONG g_type47TraceEpoch = 0;
volatile LONG g_type47TraceSealed = 1;
volatile LONG g_type47EvidenceTriggerSequence = 0;
volatile LONG g_type47SessionGeneration = 0;
volatile LONG g_type47SessionStartSequence = 1;
volatile LONG g_type47SessionEndSequence = 0;
volatile LONG g_type47PassSerial = 0;
volatile LONG g_type47DetoursActive = 0;
volatile LONG g_charContextLastPass = -1;
// Per-session re-armable one-shot diagnostic latches.  Hoisted from function
// scope so the session-start reset can re-arm them; otherwise their one-liner
// diagnostics (char-context pointer identity, save-object presence) only ever
// print for the FIRST session of the process and go dark for exactly the
// 2nd/3rd-session desync being investigated.
volatile LONG g_charContextDiagOnce = 0;
volatile LONG g_saveObjectDiagOnce = 0;
// Highest event frame seen this session (resim discriminator) and the
// session frame captured just before a snapshot restore (load_from column).
volatile LONG g_maxEventFrame = -1;
volatile LONG g_snapshotLoadFromFrame = -1;

// --- Per-call RNG tracer (2026-07-19 RNG-path audit) --------------------
// Every logical game rand() call passes through Revival's replacement fn
// (sub_1006E1A0 @ efz 0x777D61).  We detour it and record the call: the
// efz.exe return address (which game code drew), the minstd engine state
// before/after (rngEngineStateOffset), the per-pass ordinal, and the
// effect slot in context.  This is the per-CALL attribution the state
// columns cannot give.  Hot path: bounded ring, armed only in the capture
// window, discard ring after the trigger, gated by ExperimentalRngCallTrace.
struct RngCallEvent
{
    volatile LONG stamp;
    uint32_t sessionGeneration;
    uint32_t passSerial;
    int32_t frame;
    int32_t commitFrame;
    uint32_t returnAddr;   // efz.exe callsite ([esp] at detour entry)
    int32_t stateBefore;   // engine dword before the original ran
    int32_t stateAfter;    // engine dword after
    int32_t internalAdvances; // minstd steps this logical call consumed
    uint16_t effectSlot;   // slot being processed, 0xFFFF if none
    uint16_t effectBehavior;
    uint8_t resim;
    uint8_t phaseContext;  // 0 none, 1 in process-one, 2 in init/create
    // Calling thread: an rng draw from any thread OTHER than the sim thread
    // (sound callback, winmm timer, DirectSound notify) is an asynchronous
    // engine pollutant - inherently wall-clock-timed and the cleanest
    // explanation for a timing-dependent one-draw split.
    uint32_t threadId;
    // Effect Y state when the draw was an effect-slot draw: anim frame +
    // Y pos/vel doubles.  For type47 (the variable-draw-count particle) the
    // per-frame anim-advance and per-bounce draws are gated by exactly these
    // (bounce test y+vy>=0), so a peer that draws one extra time will show a
    // one-ULP Y difference here on the offset frame.  0xFFFF/0 when no slot.
    uint16_t effectAnimFrame;
    uint64_t effectYPosBits;
    uint64_t effectYVelBits;
};
constexpr uint32_t kRngTraceDepth = 1u << 16;   // 65536
constexpr uint32_t kRngTraceMask = kRngTraceDepth - 1u;
constexpr uint32_t kRngDiscardDepth = 1u << 12;
constexpr uint32_t kRngDiscardMask = kRngDiscardDepth - 1u;
constexpr LONG kRngPostTriggerEvents = kRngTraceDepth / 2;
RngCallEvent g_rngTrace[kRngTraceDepth];
RngCallEvent g_rngDiscardTrace[kRngDiscardDepth];
volatile LONG g_rngTraceWriteSequence = 0;
volatile LONG g_rngTraceEvidenceTrigger = 0;

// Effect-processing context so each rand() draw is attributed to a slot.
// Written by the effect detours (same thread as the sim tick), read by the
// rand detour; a plain volatile is sufficient (single sim thread).
volatile LONG g_rngCurrentEffectSlot = 0xFFFF;
volatile LONG g_rngCurrentEffectPhase = 0;

// Rand-replacement detour state.
using RngReplacementFn = char* (__cdecl*)();
RngReplacementFn g_origRngReplacement = nullptr;
uintptr_t g_rngReplacementAddr = 0;
uintptr_t g_rngEngineStateAddr = 0;
bool g_rngHookInstalled = false;
bool g_rngHookFailed = false;
bool g_rngTraceAvailableForSession = false;
uint8_t g_rngOwnedHookMask = 0;

// Seed-apply detour: traces every engine (re)seed - the DLL-load default,
// the synced StartInitPlayer seed, spectator init, and the savestate-restore
// reapply - deduped by seed value so per-frame idempotent restores collapse
// while any distinct/changed seed is logged with before/after engine state.
using RngSeedApplyFn = char(__stdcall*)(unsigned int);
RngSeedApplyFn g_origRngSeedApply = nullptr;
uintptr_t g_rngSeedApplyAddr = 0;
bool g_rngSeedHookInstalled = false;
bool g_rngSeedHookFailed = false;
uint8_t g_rngSeedOwnedHookMask = 0;
volatile LONG g_lastAppliedSeed = 0;
volatile LONG g_seedApplyLogCount = 0;
uintptr_t g_traceSessionPtrAddress = 0;
uintptr_t g_traceRngStateAddress = 0;
uintptr_t g_traceCurrentFrameOffset = 0;
uintptr_t g_traceGameModeSnapshotOffset = 0;
bool g_type47HooksInstalled = false;
bool g_type47HooksFailed = false;
bool g_type47TraceAvailableForSession = false;
uint8_t g_type47OwnedHookMask = 0;

// Revival rollback savestate SAVE/LOAD boundary detours.  Resolved from the
// active profile's byte-verified RVAs (1.02h only for now) against the live
// EfzRevival.dll base; fail-closed on signature mismatch.  Gated by
// [Others] ExperimentalSnapshotBoundaryMarkers.
using SnapshotSaveFn = void (__thiscall*)(void*, char);
using SnapshotLoadFn = void* (__thiscall*)(void*);
SnapshotSaveFn g_origSnapshotSave = nullptr;
SnapshotLoadFn g_origSnapshotLoad = nullptr;
uintptr_t g_snapshotSaveAddr = 0;
uintptr_t g_snapshotLoadAddr = 0;
uintptr_t g_traceGameSysPtrAddress = 0;
bool g_snapshotHooksInstalled = false;
bool g_snapshotHooksFailed = false;
bool g_snapshotMarkersAvailableForSession = false;
uint8_t g_snapshotOwnedHookMask = 0;

using InitializeEffectFn = uint16_t* (__thiscall*)(uint16_t*, int);
using ClearEffectFn = int (__fastcall*)(int, int, int16_t);
using ProcessEffectsFn = int (__thiscall*)(int16_t*);
using ProcessEffectFn = int (__thiscall*)(int, int16_t);
InitializeEffectFn g_origInitializeEffect = nullptr;
ClearEffectFn g_origClearEffect = nullptr;
ProcessEffectsFn g_origProcessEffects = nullptr;
ProcessEffectFn g_origProcessEffect = nullptr;

struct Type47DetourActivity
{
    Type47DetourActivity()
    {
        InterlockedIncrement(&g_type47DetoursActive);
    }
    ~Type47DetourActivity()
    {
        InterlockedDecrement(&g_type47DetoursActive);
    }
};

void EnsureLock()
{
    if (!g_lockInited)
    {
        InitializeCriticalSection(&g_lock);
        g_lockInited = true;
    }
}

bool TraceBytesMatch(uintptr_t address, const uint8_t* expected, size_t size)
{
    bool match = false;
    __try
    {
        match = std::memcmp(
            reinterpret_cast<const void*>(address), expected, size) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        match = false;
    }
    return match;
}

bool ReadType47TraceContext(int32_t* outFrame, int32_t* outCommit, int32_t* outRng)
{
    if (outFrame == nullptr || outCommit == nullptr || outRng == nullptr
        || g_traceSessionPtrAddress == 0 || g_traceRngStateAddress == 0)
    {
        return false;
    }

    bool ok = false;
    __try
    {
        const uintptr_t session =
            *reinterpret_cast<const volatile uintptr_t*>(g_traceSessionPtrAddress);
        if (session != 0)
        {
            *outFrame = *reinterpret_cast<const volatile int32_t*>(
                session + g_traceCurrentFrameOffset);
            *outCommit = *reinterpret_cast<const volatile int32_t*>(
                session + g_traceGameModeSnapshotOffset + 16);
            *outRng = *reinterpret_cast<const volatile int32_t*>(
                g_traceRngStateAddress);
            ok = true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    return ok;
}

bool IsLiveType47Slot(uintptr_t gameSys, int16_t slot)
{
    if (gameSys == 0 || slot < 0
        || slot >= static_cast<int16_t>(kEffectRingSlots))
    {
        return false;
    }

    bool target = false;
    __try
    {
        const bool active =
            *reinterpret_cast<const volatile uint32_t*>(
                gameSys + kEffectActiveFlagBase + 4u * static_cast<uint16_t>(slot))
            != 0;
        const uint16_t behavior =
            *reinterpret_cast<const volatile uint16_t*>(
                gameSys + kEffectRecordBase
                + kEffectRecordStride * static_cast<uint16_t>(slot)
                + kEffectFieldBehaviorId);
        target = active && behavior == kTargetEffectType;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        target = false;
    }
    return target;
}

bool CaptureType47TraceEpoch(LONG* outEpoch)
{
    if (outEpoch == nullptr)
    {
        return false;
    }
    const LONG epoch =
        InterlockedCompareExchange(&g_type47TraceEpoch, 0, 0);
    if (epoch <= 0
        || InterlockedCompareExchange(&g_type47TraceEnabled, 0, 0) == 0)
    {
        return false;
    }
    MemoryBarrier();
    if (epoch != InterlockedCompareExchange(&g_type47TraceEpoch, 0, 0)
        || InterlockedCompareExchange(&g_type47TraceEnabled, 0, 0) == 0)
    {
        return false;
    }
    *outEpoch = epoch;
    return true;
}

void InvalidateType47TraceProducers()
{
    // Epoch invalidation rejects a detour that entered before a session or
    // teardown boundary but returns from the original EFZ function after it.
    InterlockedExchange(&g_type47TraceEnabled, 0);
    MemoryBarrier();
    (void)InterlockedIncrement(&g_type47TraceEpoch);
}

void SealType47TraceSession()
{
    InterlockedExchange(&g_type47TraceSealed, 1);
    InvalidateType47TraceProducers();
    const LONG end =
        InterlockedCompareExchange(&g_type47TraceWriteSequence, 0, 0);
    (void)InterlockedCompareExchange(&g_type47SessionEndSequence, end, 0);
}

void PublishType47FrameAnchor(
    const Type47TraceEvent& event,
    LONG sequence,
    bool preservedInDetailedRing)
{
    if (!preservedInDetailedRing || event.frame < 0 || sequence <= 0)
    {
        return;
    }

    Type47FrameAnchor& anchor = g_type47FrameAnchors[
        static_cast<uint32_t>(event.frame) & kType47FrameAnchorMask];
    const LONG before = InterlockedCompareExchange(&anchor.stamp, 0, 0);
    if (before > 0)
    {
        const uint32_t generation = anchor.sessionGeneration;
        const int32_t frame = anchor.frame;
        MemoryBarrier();
        const LONG after = InterlockedCompareExchange(&anchor.stamp, 0, 0);
        if (after == before
            && generation == event.sessionGeneration
            && frame == event.frame)
        {
            // Keep the first event for this frame; later phases can be found
            // by walking forward from the anchor in the dump.
            return;
        }
    }

    InterlockedExchange(&anchor.stamp, 0);
    anchor.sessionGeneration = event.sessionGeneration;
    anchor.frame = event.frame;
    anchor.firstSequence = sequence;
    MemoryBarrier();
    InterlockedExchange(&anchor.stamp, sequence);
}

// Snapshot marker rows have no effect slot, so they reuse the per-slot CSV
// fields for character context instead: anim_frame/anim_tick = P1 anchor
// words +204/+206, parameter = P2 anchors packed (204<<16|206), x/y bits =
// P1 position doubles (+216/+224), vx/vy bits = P2 position doubles.
// These are the exact inputs of EFZ's calculateCharacterHitbox, whose
// output midpoint is the hit-effect spawn origin that shifted by a constant
// (-17,-15) between the first execution and the restored re-execution in
// the desync4/desync5 captures.  Comparing these fields across a
// snapshot_save row and the snapshot_load row that rewinds it names the
// exact field the restore fails to round-trip.
// Read a game double as raw bits via two volatile dword loads.  The
// word-sized volatile reads in these fills demonstrably work from every
// detour context while 8-byte memcpy captured zeros (capture 8); use the
// proven access pattern for the 64-bit fields too.
// FNV-1a over a raw game-memory region.  Used by the snapshot marker rows
// to prove (or refute) byte-level save->load round-trip per copied region;
// callers run under SEH so a faulting page degrades to a partial hash on
// the marker row rather than a crash.
uint32_t HashRegion(uintptr_t base, size_t size)
{
    uint32_t hash = 2166136261u;
    const volatile uint8_t* bytes =
        reinterpret_cast<const volatile uint8_t*>(base);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

uint64_t ReadGameDoubleBits(uintptr_t address)
{
    const uint32_t lo =
        *reinterpret_cast<const volatile uint32_t*>(address);
    const uint32_t hi =
        *reinterpret_cast<const volatile uint32_t*>(address + 4u);
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void FillSnapshotMarkerCharContext(Type47TraceEvent* event)
{
    // DECOMP CORRECTION (2026-07-20 audit): char+204/206/216/224 are NOT
    // character fields (unused by the whole binary); the collision context's
    // "+0xCC/0xCE anchors" are PAT-table words and "+0xD8/0xE0" are stack
    // copies of char+0x20/+0x28. Real world positions are +32/+40.
    constexpr uintptr_t kCharAnchorXOffset = 204;   // UNUSED char bytes (kept: column compat)
    constexpr uintptr_t kCharAnchorYOffset = 206;   // UNUSED char bytes
    constexpr uintptr_t kCharPosXOffset = 32;       // double, REAL world X
    constexpr uintptr_t kCharPosYOffset = 40;       // double, REAL world Y
    __try
    {
        const uintptr_t battleScreen =
            *reinterpret_cast<const volatile uintptr_t*>(
                kScreenTableAddr + 4u * 3u);
        if (battleScreen == 0)
        {
            return;
        }
        const uintptr_t p1 =
            *reinterpret_cast<const volatile uintptr_t*>(
                battleScreen + kOffsetBattleP1Char);
        const uintptr_t p2 =
            *reinterpret_cast<const volatile uintptr_t*>(
                battleScreen + kOffsetBattleP2Char);
        if (p1 != 0
            && InterlockedExchange(&g_charContextDiagOnce, 1) == 0)
        {
            // One-shot: raw pointers (incl. the SECOND char-pointer pair at
            // +20/+24 that Revival's savestate copies - identity vs the sim
            // pair at +12/+16 is settled by this line) + position dwords so
            // a zeroed capture column can be attributed from the log alone.
            mod::Log(
                "TYPE47_TRACE: char-context diag battleScreen=0x%08lX "
                "p1=0x%08lX p2=0x%08lX save20=0x%08lX save24=0x%08lX "
                "p1move=%u p1x=0x%08lX%08lX",
                static_cast<unsigned long>(battleScreen),
                static_cast<unsigned long>(p1),
                static_cast<unsigned long>(p2),
                static_cast<unsigned long>(
                    *reinterpret_cast<const volatile uint32_t*>(
                        battleScreen + 20u)),
                static_cast<unsigned long>(
                    *reinterpret_cast<const volatile uint32_t*>(
                        battleScreen + 24u)),
                static_cast<unsigned>(
                    *reinterpret_cast<const volatile uint16_t*>(p1 + 8)),
                static_cast<unsigned long>(
                    *reinterpret_cast<const volatile uint32_t*>(
                        p1 + kCharPosXOffset + 4u)),
                static_cast<unsigned long>(
                    *reinterpret_cast<const volatile uint32_t*>(
                        p1 + kCharPosXOffset)));
        }
        if (p1 != 0)
        {
            event->xBits = ReadGameDoubleBits(p1 + kCharPosXOffset);
            event->yBits = ReadGameDoubleBits(p1 + kCharPosYOffset);
        }
        if (p2 != 0)
        {
            event->vxBits = ReadGameDoubleBits(p2 + kCharPosXOffset);
            event->vyBits = ReadGameDoubleBits(p2 + kCharPosYOffset);
        }
        // Per-marker char-object pointer identity (critic finding): the
        // savestate restores through the double-indirect pair at
        // battleContext+20/+24 while the sim mutates +12/+16.  A restore
        // that targets a different object than the sim reads is the
        // cleanest mechanism for "Sync matches but the sim reads
        // unrestored state" - verified here on EVERY save/load, not once.
        {
            const uintptr_t s20 =
                *reinterpret_cast<const volatile uint32_t*>(
                    battleScreen + 20u);
            const uintptr_t s24 =
                *reinterpret_cast<const volatile uint32_t*>(
                    battleScreen + 24u);
            const uintptr_t saveP1 = s20 != 0
                ? *reinterpret_cast<const volatile uint32_t*>(s20)
                : 0;
            const uintptr_t saveP2 = s24 != 0
                ? *reinterpret_cast<const volatile uint32_t*>(s24)
                : 0;
            event->simPtrs =
                (static_cast<uint64_t>(p1) << 32) | static_cast<uint32_t>(p2);
            event->savePtrs =
                (static_cast<uint64_t>(saveP1) << 32)
                | static_cast<uint32_t>(saveP2);
        }
        // Region round-trip hashes (marker rows only).  Field reuse:
        //   status = charP1 hash, parameter = charP2 hash,
        //   alloc/proc = gameSystem hash hi/lo,
        //   anim_frame/anim_tick = battleContext hash hi/lo.
        // A snapshot_load row whose hashes equal the preceding
        // snapshot_save row proves the restore reproduced those regions
        // byte-for-byte; any inequality localizes restore infidelity to
        // that region.  0x3448 = minimum per-character copy size from
        // Revival's own Size[] table.
        {
            const uintptr_t gameSys =
                *reinterpret_cast<const volatile uintptr_t*>(
                    battleScreen + kOffsetGameSystem);
            if (p1 != 0)
            {
                event->status = HashRegion(p1, 0x3448u);
            }
            if (p2 != 0)
            {
                event->parameter = HashRegion(p2, 0x3448u);
            }
            if (gameSys != 0)
            {
                const uint32_t gameSysHash = HashRegion(gameSys, 0x142F0u);
                event->allocCursor =
                    static_cast<uint16_t>(gameSysHash >> 16);
                event->procCursor =
                    static_cast<uint16_t>(gameSysHash & 0xFFFFu);
            }
            const uint32_t battleHash = HashRegion(battleScreen, 0x598u);
            event->animFrame = static_cast<uint16_t>(battleHash >> 16);
            event->animTick = static_cast<uint16_t>(battleHash & 0xFFFFu);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// char_context rows (published once per effect pass that creates a type-47)
// record the PAT-row-selecting animation vintage of both characters at the
// moment of creation.  Field reuse (slot=0xFFFE):
//   alloc/proc cursor columns = P1 anim frame (+10) / P1 anim tick (+12)
//   anim_frame/anim_tick columns = P2 anim frame / P2 anim tick
//   status = (P1 move ID (+8) << 16) | P2 move ID
//   parameter = P2 anchors packed;  x/y = P1 pos;  vx/vy = P2 pos.
// If the (-17,-15) spawn shift comes from the re-execution reading an
// adjacent PAT row, the pass-A and pass-B char_context rows will differ in
// exactly these anim words.
void FillCharContextRow(Type47TraceEvent* event)
{
    constexpr uintptr_t kCharMoveIdOffset = 8;    // WORD
    constexpr uintptr_t kCharAnimFrameOffset = 10; // WORD
    constexpr uintptr_t kCharAnimTickOffset = 12;  // WORD
    // DECOMP CORRECTION: 216/224 were unused bytes; real world pos = 32/40.
    constexpr uintptr_t kCharAnchorXOffset = 204;   // UNUSED char bytes (column compat)
    constexpr uintptr_t kCharAnchorYOffset = 206;   // UNUSED char bytes
    constexpr uintptr_t kCharPosXOffset = 32;       // double, REAL world X
    constexpr uintptr_t kCharPosYOffset = 40;       // double, REAL world Y
    __try
    {
        const uintptr_t battleScreen =
            *reinterpret_cast<const volatile uintptr_t*>(
                kScreenTableAddr + 4u * 3u);
        if (battleScreen == 0)
        {
            return;
        }
        const uintptr_t p1 =
            *reinterpret_cast<const volatile uintptr_t*>(
                battleScreen + kOffsetBattleP1Char);
        const uintptr_t p2 =
            *reinterpret_cast<const volatile uintptr_t*>(
                battleScreen + kOffsetBattleP2Char);
        uint32_t p1Move = 0;
        uint32_t p2Move = 0;
        if (p1 != 0)
        {
            p1Move = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharMoveIdOffset);
            event->allocCursor = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharAnimFrameOffset);
            event->procCursor = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharAnimTickOffset);
            event->xBits = ReadGameDoubleBits(p1 + kCharPosXOffset);
            event->yBits = ReadGameDoubleBits(p1 + kCharPosYOffset);
        }
        if (p2 != 0)
        {
            p2Move = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharMoveIdOffset);
            event->animFrame = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharAnimFrameOffset);
            event->animTick = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharAnimTickOffset);
            const uint32_t p2AnchorX =
                *reinterpret_cast<const volatile uint16_t*>(
                    p2 + kCharAnchorXOffset);
            const uint32_t p2AnchorY =
                *reinterpret_cast<const volatile uint16_t*>(
                    p2 + kCharAnchorYOffset);
            event->parameter = (p2AnchorX << 16) | p2AnchorY;
            event->vxBits = ReadGameDoubleBits(p2 + kCharPosXOffset);
            event->vyBits = ReadGameDoubleBits(p2 + kCharPosYOffset);
        }
        event->status = (p1Move << 16) | p2Move;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// Sample both characters' anim/gating vintage into the verbose columns.
// Reads: +8 move ID, +10 anim frame, +12 anim tick (the PAT-row selectors),
// +330 freeze/hitstop counter (gates anim advance), +360 contact result
// (low word; 3 = throw contact accepted).
void FillCharVintage(Type47TraceEvent* event)
{
    constexpr uintptr_t kCharMoveIdOffset = 8;
    constexpr uintptr_t kCharAnimFrameOffset = 10;
    constexpr uintptr_t kCharAnimTickOffset = 12;
    constexpr uintptr_t kCharScreenXOffset = 32;
    constexpr uintptr_t kCharScreenYOffset = 40;
    constexpr uintptr_t kCharVxOffset = 48;
    constexpr uintptr_t kCharVyOffset = 56;
    constexpr uintptr_t kCharFacingOffset = 80;
    constexpr uintptr_t kCharMomentumXOffset = 176;
    constexpr uintptr_t kCharKnockbackXOffset = 192;
    constexpr uintptr_t kCharKnockbackYOffset = 200;
    constexpr uintptr_t kCharAnchorXOffset = 204;
    constexpr uintptr_t kCharAnchorYOffset = 206;
    constexpr uintptr_t kCharPushFlagOffset = 208;
    constexpr uintptr_t kCharReactFlagOffset = 300;
    constexpr uintptr_t kCharHitstunOffset = 316;
    constexpr uintptr_t kCharMeterOffset = 328;
    constexpr uintptr_t kCharFreezeOffset = 330;
    constexpr uintptr_t kCharFreeze2Offset = 332;
    constexpr uintptr_t kCharBoxTableOffset = 356;
    constexpr uintptr_t kCharContactOffset = 360;
    constexpr uintptr_t kCharMoveTimerOffset = 364;
    constexpr uintptr_t kCharThrowCtrOffset = 12600;
    constexpr uintptr_t kGsCollGate40 = 4940;
    constexpr uintptr_t kGsCollGate44 = 4944;
    constexpr uintptr_t kGsCollGate48 = 4948;
    constexpr uintptr_t kGsRoundNo = 4952;
    constexpr uintptr_t kGsReadyMask = 4956;
    constexpr uintptr_t kGsWordstreamMode = 82563;
    constexpr uintptr_t kGsWordstreamCtr = 82564 + 12; // reader-struct counter

    // FPU control state: pure register reads, sampled first so a later
    // faulting memory read cannot lose them.
#if defined(_MSC_VER) && defined(_M_IX86)
    {
        uint16_t x87Cw = 0;
        uint32_t mxcsr = 0;
        __asm fnstcw x87Cw
        __asm stmxcsr mxcsr
        event->fpuX87Cw = x87Cw;
        event->fpuMxcsr = mxcsr;
    }
#endif

    __try
    {
        const uintptr_t battleScreen =
            *reinterpret_cast<const volatile uintptr_t*>(
                kScreenTableAddr + 4u * 3u);
        if (battleScreen == 0)
        {
            return;
        }
        const uintptr_t p1 =
            *reinterpret_cast<const volatile uintptr_t*>(
                battleScreen + kOffsetBattleP1Char);
        const uintptr_t p2 =
            *reinterpret_cast<const volatile uintptr_t*>(
                battleScreen + kOffsetBattleP2Char);
        const uintptr_t gameSys =
            *reinterpret_cast<const volatile uintptr_t*>(
                battleScreen + kOffsetGameSystem);
        if (p1 != 0)
        {
            event->p1Move = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharMoveIdOffset);
            event->p1AnimFrame = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharAnimFrameOffset);
            event->p1AnimTick = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharAnimTickOffset);
            event->p1Freeze = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharFreezeOffset);
            event->p1Contact = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharContactOffset);
            event->p1Facing = *reinterpret_cast<const volatile int32_t*>(
                p1 + kCharFacingOffset);
            event->p1Hitstun = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharHitstunOffset);
            event->p1Freeze2 = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharFreeze2Offset);
            event->p1MoveTimer = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharMoveTimerOffset);
            event->p1ReactFlag = *reinterpret_cast<const volatile int32_t*>(
                p1 + kCharReactFlagOffset);
            event->p1ThrowCtr = *reinterpret_cast<const volatile int32_t*>(
                p1 + kCharThrowCtrOffset);
            event->p1Meter = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharMeterOffset);
            const uint32_t a1x = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharAnchorXOffset);
            const uint32_t a1y = *reinterpret_cast<const volatile uint16_t*>(
                p1 + kCharAnchorYOffset);
            event->p1Anchor = (a1x << 16) | a1y;
            event->p1ScreenXBits = ReadGameDoubleBits(p1 + kCharScreenXOffset);
            event->p1ScreenYBits = ReadGameDoubleBits(p1 + kCharScreenYOffset);
            event->p1VxBits = ReadGameDoubleBits(p1 + kCharVxOffset);
            event->p1VyBits = ReadGameDoubleBits(p1 + kCharVyOffset);
            event->p1KbxBits = ReadGameDoubleBits(p1 + kCharKnockbackXOffset);
            event->p1KbyBits = ReadGameDoubleBits(p1 + kCharKnockbackYOffset);
            event->p1MomxBits = ReadGameDoubleBits(p1 + kCharMomentumXOffset);
        }
        if (p2 != 0)
        {
            event->p2Move = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharMoveIdOffset);
            event->p2AnimFrame = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharAnimFrameOffset);
            event->p2AnimTick = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharAnimTickOffset);
            event->p2Freeze = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharFreezeOffset);
            event->p2Contact = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharContactOffset);
            event->p2Facing = *reinterpret_cast<const volatile int32_t*>(
                p2 + kCharFacingOffset);
            event->p2Hitstun = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharHitstunOffset);
            event->p2Freeze2 = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharFreeze2Offset);
            event->p2MoveTimer = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharMoveTimerOffset);
            event->p2ReactFlag = *reinterpret_cast<const volatile int32_t*>(
                p2 + kCharReactFlagOffset);
            event->p2ThrowCtr = *reinterpret_cast<const volatile int32_t*>(
                p2 + kCharThrowCtrOffset);
            event->p2Meter = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharMeterOffset);
            const uint32_t a2x = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharAnchorXOffset);
            const uint32_t a2y = *reinterpret_cast<const volatile uint16_t*>(
                p2 + kCharAnchorYOffset);
            event->p2Anchor = (a2x << 16) | a2y;
            event->p2ScreenXBits = ReadGameDoubleBits(p2 + kCharScreenXOffset);
            event->p2ScreenYBits = ReadGameDoubleBits(p2 + kCharScreenYOffset);
            event->p2VxBits = ReadGameDoubleBits(p2 + kCharVxOffset);
            event->p2VyBits = ReadGameDoubleBits(p2 + kCharVyOffset);
            event->p2KbxBits = ReadGameDoubleBits(p2 + kCharKnockbackXOffset);
            event->p2KbyBits = ReadGameDoubleBits(p2 + kCharKnockbackYOffset);
            event->p2MomxBits = ReadGameDoubleBits(p2 + kCharMomentumXOffset);
        }
        if (p1 != 0 && p2 != 0)
        {
            const uint32_t pf1 = *reinterpret_cast<const volatile uint32_t*>(
                p1 + kCharPushFlagOffset);
            const uint32_t pf2 = *reinterpret_cast<const volatile uint32_t*>(
                p2 + kCharPushFlagOffset);
            event->pushFlags = ((pf1 & 0xFFFFu) << 16) | (pf2 & 0xFFFFu);
        }
        // Resolved box-geometry FNV per char: [char+356] -> table;
        // entry = *(u32*)(table + 8*move + 4); block = entry + 200*animFrame.
        // Proves the hitbox row the midpoint intersects is byte-identical.
        if (p1 != 0)
        {
            const uintptr_t table =
                *reinterpret_cast<const volatile uint32_t*>(
                    p1 + kCharBoxTableOffset);
            if (table != 0)
            {
                const uintptr_t entry =
                    *reinterpret_cast<const volatile uint32_t*>(
                        table + 8u * event->p1Move + 4u);
                if (entry != 0)
                {
                    event->p1BoxFnv = HashRegion(
                        entry + 200u * event->p1AnimFrame, 200u);
                }
            }
        }
        if (p2 != 0)
        {
            const uintptr_t table =
                *reinterpret_cast<const volatile uint32_t*>(
                    p2 + kCharBoxTableOffset);
            if (table != 0)
            {
                const uintptr_t entry =
                    *reinterpret_cast<const volatile uint32_t*>(
                        table + 8u * event->p2Move + 4u);
                if (entry != 0)
                {
                    event->p2BoxFnv = HashRegion(
                        entry + 200u * event->p2AnimFrame, 200u);
                }
            }
        }
        // Global temporal-control gates + word-stream state off gameSys.
        if (gameSys != 0)
        {
            event->collGate40 = *reinterpret_cast<const volatile uint8_t*>(
                gameSys + kGsCollGate40);
            event->collGate44 = *reinterpret_cast<const volatile uint8_t*>(
                gameSys + kGsCollGate44);
            event->collGate48 = *reinterpret_cast<const volatile uint32_t*>(
                gameSys + kGsCollGate48);
            event->roundNo = *reinterpret_cast<const volatile uint8_t*>(
                gameSys + kGsRoundNo);
            event->readyMask = *reinterpret_cast<const volatile uint32_t*>(
                gameSys + kGsReadyMask);
            event->wordstreamMode = *reinterpret_cast<const volatile uint8_t*>(
                gameSys + kGsWordstreamMode);
            event->wordstreamCtr = *reinterpret_cast<const volatile uint32_t*>(
                gameSys + kGsWordstreamCtr);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void PublishType47Event(
    uintptr_t gameSys,
    uint16_t slot,
    uint8_t phase,
    LONG traceEpoch,
    uint32_t passSerial)
{
    if (traceEpoch <= 0
        || traceEpoch != InterlockedCompareExchange(&g_type47TraceEpoch, 0, 0)
        || InterlockedCompareExchange(&g_type47TraceEnabled, 0, 0) == 0
        || gameSys == 0)
    {
        return;
    }

    Type47TraceEvent next = {};
    next.p1Move = 0xFFFF;
    next.p1AnimFrame = 0xFFFF;
    next.p1AnimTick = 0xFFFF;
    next.p1Freeze = 0xFFFF;
    next.p1Contact = 0xFFFF;
    next.p2Move = 0xFFFF;
    next.p2AnimFrame = 0xFFFF;
    next.p2AnimTick = 0xFFFF;
    next.p2Freeze = 0xFFFF;
    next.p2Contact = 0xFFFF;
    next.loadFrom = 0xFFFFFFFFu;
    next.effDir = -1;
    next.activeCount = 0xFFFF;
    next.sessionGeneration = static_cast<uint32_t>(traceEpoch);
    next.passSerial = passSerial;
    next.frame = -1;
    next.commitFrame = -1;
    next.rngState = -1;
    next.slot = slot;
    next.allocCursor = 0xFFFF;
    next.procCursor = 0xFFFF;
    next.phase = phase;
    (void)ReadType47TraceContext(&next.frame, &next.commitFrame, &next.rngState);

    __try
    {
        next.allocCursor = *reinterpret_cast<const volatile uint16_t*>(
            gameSys + kEffectAllocCursorOffset);
        next.procCursor = *reinterpret_cast<const volatile uint16_t*>(
            gameSys + kEffectProcCursorOffset);
        if (slot < kEffectRingSlots)
        {
            const uintptr_t record =
                gameSys + kEffectRecordBase + kEffectRecordStride * slot;
            next.active =
                *reinterpret_cast<const volatile uint32_t*>(
                    gameSys + kEffectActiveFlagBase + 4u * slot) != 0
                    ? 1
                    : 0;
            next.status = *reinterpret_cast<const volatile uint32_t*>(
                gameSys + kEffectStatusBase + 4u * slot);
            next.behavior = *reinterpret_cast<const volatile uint16_t*>(
                record + kEffectFieldBehaviorId);
            next.animFrame = *reinterpret_cast<const volatile uint16_t*>(
                record + kEffectFieldAnimFrame);
            next.animTick = *reinterpret_cast<const volatile uint16_t*>(
                record + kEffectFieldAnimTick);
            next.parameter = *reinterpret_cast<const volatile uint32_t*>(
                record + kEffectFieldParameter);
            std::memcpy(&next.xBits,
                        reinterpret_cast<const void*>(record + kEffectFieldPosX), 8);
            std::memcpy(&next.yBits,
                        reinterpret_cast<const void*>(record + kEffectFieldPosY), 8);
            std::memcpy(&next.vxBits,
                        reinterpret_cast<const void*>(record + kEffectFieldVelX), 8);
            std::memcpy(&next.vyBits,
                        reinterpret_cast<const void*>(record + kEffectFieldVelY), 8);
            // Effect direction word (+76): (attacker facing < 0) at
            // creation; steers each type-47 child's vx sign.
            next.effDir = *reinterpret_cast<const volatile int16_t*>(
                record + 76u);
        }
        // Ring occupancy at creation/context time: how many slots are live
        // decides where advanceToNextFreeEntitySlot places new effects, so
        // pass-A vs pass-B slot placement is directly comparable.
        if (phase == kTraceCreated || phase == kTraceCharContext)
        {
            uint16_t liveCount = 0;
            for (uint32_t i = 0; i < kEffectRingSlots; ++i)
            {
                if (*reinterpret_cast<const volatile uint32_t*>(
                        gameSys + kEffectActiveFlagBase + 4u * i) != 0)
                {
                    ++liveCount;
                }
            }
            next.activeCount = liveCount;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }

    if (phase == kTraceSnapshotSave || phase == kTraceSnapshotLoad)
    {
        FillSnapshotMarkerCharContext(&next);
    }
    else if (phase == kTraceCharContext)
    {
        FillCharContextRow(&next);
    }
    if (netplay::mod_settings::IsCaptureVerboseDumpEnabled())
    {
        FillCharVintage(&next);
    }
    if (phase == kTraceSnapshotLoad)
    {
        // Session frame captured just before the restore ran (stashed by
        // HookSnapshotLoad); the row's frame column is the post-restore
        // value, so the pair shows the rollback distance directly.
        next.loadFrom = static_cast<uint32_t>(
            InterlockedExchange(&g_snapshotLoadFromFrame, -1));
    }
    // Rollback re-execution discriminator: below the session's frame
    // high-water mark means this event belongs to a re-simulated frame.
    if (next.frame >= 0)
    {
        const LONG maxSeen =
            InterlockedCompareExchange(&g_maxEventFrame, 0, 0);
        next.resim = (next.frame < maxSeen) ? 1 : 0;
        if (next.frame > maxSeen)
        {
            InterlockedExchange(&g_maxEventFrame, next.frame);
        }
    }

    // Do not reserve a sequence after a session boundary. A boundary can
    // still race immediately after this check; the event retains its entry
    // epoch and the dump filters it out of any newer session.
    MemoryBarrier();
    if (traceEpoch != InterlockedCompareExchange(&g_type47TraceEpoch, 0, 0)
        || InterlockedCompareExchange(&g_type47TraceEnabled, 0, 0) == 0)
    {
        return;
    }

    const LONG sequence = InterlockedIncrement(&g_type47TraceWriteSequence);
    const LONG triggerSequence = InterlockedCompareExchange(
        &g_type47EvidenceTriggerSequence, 0, 0);
    const bool preserveEvidence =
        triggerSequence > 0
        && static_cast<int64_t>(sequence)
                - static_cast<int64_t>(triggerSequence)
            > static_cast<int64_t>(kType47PostTriggerEvents);
    Type47TraceEvent& destination = preserveEvidence
        ? g_type47DiscardTrace[
            (static_cast<uint32_t>(sequence) - 1u) & kType47DiscardMask]
        : g_type47Trace[
            (static_cast<uint32_t>(sequence) - 1u) & kType47TraceMask];
    InterlockedExchange(&destination.stamp, 0);
    destination.sessionGeneration = next.sessionGeneration;
    destination.passSerial = next.passSerial;
    destination.frame = next.frame;
    destination.commitFrame = next.commitFrame;
    destination.rngState = next.rngState;
    destination.slot = next.slot;
    destination.allocCursor = next.allocCursor;
    destination.procCursor = next.procCursor;
    destination.behavior = next.behavior;
    destination.animFrame = next.animFrame;
    destination.animTick = next.animTick;
    destination.phase = next.phase;
    destination.active = next.active;
    destination.status = next.status;
    destination.parameter = next.parameter;
    destination.xBits = next.xBits;
    destination.yBits = next.yBits;
    destination.p1Move = next.p1Move;
    destination.p1AnimFrame = next.p1AnimFrame;
    destination.p1AnimTick = next.p1AnimTick;
    destination.p1Freeze = next.p1Freeze;
    destination.p1Contact = next.p1Contact;
    destination.p2Move = next.p2Move;
    destination.p2AnimFrame = next.p2AnimFrame;
    destination.p2AnimTick = next.p2AnimTick;
    destination.p2Freeze = next.p2Freeze;
    destination.p2Contact = next.p2Contact;
    destination.p1ScreenXBits = next.p1ScreenXBits;
    destination.p1ScreenYBits = next.p1ScreenYBits;
    destination.p2ScreenXBits = next.p2ScreenXBits;
    destination.p2ScreenYBits = next.p2ScreenYBits;
    destination.fpuX87Cw = next.fpuX87Cw;
    destination.fpuMxcsr = next.fpuMxcsr;
    destination.p1Facing = next.p1Facing;
    destination.p2Facing = next.p2Facing;
    destination.p1Hitstun = next.p1Hitstun;
    destination.p1Freeze2 = next.p1Freeze2;
    destination.p1MoveTimer = next.p1MoveTimer;
    destination.p2Hitstun = next.p2Hitstun;
    destination.p2Freeze2 = next.p2Freeze2;
    destination.p2MoveTimer = next.p2MoveTimer;
    destination.collGate40 = next.collGate40;
    destination.collGate44 = next.collGate44;
    destination.collGate48 = next.collGate48;
    destination.roundNo = next.roundNo;
    destination.readyMask = next.readyMask;
    destination.wordstreamMode = next.wordstreamMode;
    destination.wordstreamCtr = next.wordstreamCtr;
    destination.p1VxBits = next.p1VxBits;
    destination.p1VyBits = next.p1VyBits;
    destination.p1KbxBits = next.p1KbxBits;
    destination.p1KbyBits = next.p1KbyBits;
    destination.p1MomxBits = next.p1MomxBits;
    destination.p2VxBits = next.p2VxBits;
    destination.p2VyBits = next.p2VyBits;
    destination.p2KbxBits = next.p2KbxBits;
    destination.p2KbyBits = next.p2KbyBits;
    destination.p2MomxBits = next.p2MomxBits;
    destination.pushFlags = next.pushFlags;
    destination.p1Anchor = next.p1Anchor;
    destination.p2Anchor = next.p2Anchor;
    destination.p1BoxFnv = next.p1BoxFnv;
    destination.p2BoxFnv = next.p2BoxFnv;
    destination.p1ReactFlag = next.p1ReactFlag;
    destination.p2ReactFlag = next.p2ReactFlag;
    destination.p1ThrowCtr = next.p1ThrowCtr;
    destination.p2ThrowCtr = next.p2ThrowCtr;
    destination.p1Meter = next.p1Meter;
    destination.p2Meter = next.p2Meter;
    destination.effDir = next.effDir;
    destination.activeCount = next.activeCount;
    destination.simPtrs = next.simPtrs;
    destination.savePtrs = next.savePtrs;
    destination.loadFrom = next.loadFrom;
    destination.resim = next.resim;
    destination.vxBits = next.vxBits;
    destination.vyBits = next.vyBits;
    MemoryBarrier();
    InterlockedExchange(&destination.stamp, sequence);
    PublishType47FrameAnchor(next, sequence, !preserveEvidence);
}

void ReadInitializeTraceFields(
    const uint16_t* gameSys,
    int entityData,
    uint16_t* outSlot,
    uint16_t* outBehavior)
{
    *outSlot = 0xFFFF;
    *outBehavior = 0;
    __try
    {
        *outSlot = *reinterpret_cast<const volatile uint16_t*>(
            reinterpret_cast<uintptr_t>(gameSys) + kEffectAllocCursorOffset);
        *outBehavior = *reinterpret_cast<const volatile uint16_t*>(
            entityData + 4);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *outSlot = 0xFFFF;
        *outBehavior = 0;
    }
}

uint16_t* __fastcall HookInitializeEffect(
    uint16_t* gameSys,
    void* /*edx*/,
    int entityData)
{
    Type47DetourActivity activity;
    uint16_t slot = 0xFFFF;
    uint16_t requestedBehavior = 0;
    LONG traceEpoch = 0;
    const bool tracing = CaptureType47TraceEpoch(&traceEpoch);
    uint32_t passSerial = 0;
    if (tracing)
    {
        passSerial = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_type47PassSerial, 0, 0));
        ReadInitializeTraceFields(
            gameSys, entityData, &slot, &requestedBehavior);
    }

    uint16_t* result = g_origInitializeEffect(gameSys, entityData);
    if (tracing && slot < kEffectRingSlots
        && requestedBehavior == kTargetEffectType)
    {
        // One char_context row per creating pass: the PAT-row-selecting
        // animation vintage of both characters at creation time.
        const LONG passAsLong = static_cast<LONG>(passSerial);
        if (InterlockedExchange(&g_charContextLastPass, passAsLong)
            != passAsLong)
        {
            PublishType47Event(
                reinterpret_cast<uintptr_t>(gameSys), 0xFFFE,
                kTraceCharContext, traceEpoch, passSerial);
        }
        PublishType47Event(
            reinterpret_cast<uintptr_t>(gameSys), slot, kTraceCreated,
            traceEpoch, passSerial);
    }
    return result;
}

int __fastcall HookClearEffect(int gameSys, int edxValue, int16_t slot)
{
    Type47DetourActivity activity;
    LONG traceEpoch = 0;
    const bool target =
        CaptureType47TraceEpoch(&traceEpoch)
        && IsLiveType47Slot(static_cast<uintptr_t>(gameSys), slot);
    uint32_t passSerial = 0;
    if (target)
    {
        passSerial = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_type47PassSerial, 0, 0));
        PublishType47Event(
            static_cast<uintptr_t>(gameSys),
            static_cast<uint16_t>(slot),
            kTraceCleared,
            traceEpoch,
            passSerial);
    }
    return g_origClearEffect(gameSys, edxValue, slot);
}

int __fastcall HookProcessEffect(int gameSys, void* /*edx*/, int16_t slot)
{
    Type47DetourActivity activity;
    LONG traceEpoch = 0;
    const bool target =
        CaptureType47TraceEpoch(&traceEpoch)
        && IsLiveType47Slot(static_cast<uintptr_t>(gameSys), slot);
    uint32_t passSerial = 0;
    if (target)
    {
        passSerial = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_type47PassSerial, 0, 0));
        PublishType47Event(
            static_cast<uintptr_t>(gameSys),
            static_cast<uint16_t>(slot),
            kTraceBeforeUpdate,
            traceEpoch,
            passSerial);
    }
    InterlockedExchange(&g_rngCurrentEffectSlot, static_cast<LONG>(
        static_cast<uint16_t>(slot)));
    InterlockedExchange(&g_rngCurrentEffectPhase, 1);
    const int result = g_origProcessEffect(gameSys, slot);
    InterlockedExchange(&g_rngCurrentEffectSlot, 0xFFFF);
    InterlockedExchange(&g_rngCurrentEffectPhase, 0);
    if (target)
    {
        // Publish even if the update cleared/retyped the slot; the active and
        // behavior fields then expose the transition without a shadow table.
        PublishType47Event(
            static_cast<uintptr_t>(gameSys),
            static_cast<uint16_t>(slot),
            kTraceAfterUpdate,
            traceEpoch,
            passSerial);
    }
    return result;
}

int __fastcall HookProcessEffects(int16_t* gameSys, void* /*edx*/)
{
    Type47DetourActivity activity;
    LONG traceEpoch = 0;
    if (!CaptureType47TraceEpoch(&traceEpoch))
    {
        return g_origProcessEffects(gameSys);
    }

    const uint32_t passSerial = static_cast<uint32_t>(
        InterlockedIncrement(&g_type47PassSerial));
    const uintptr_t base = reinterpret_cast<uintptr_t>(gameSys);
    PublishType47Event(
        base, 0xFFFF, kTracePassBegin, traceEpoch, passSerial);
    if (netplay::mod_settings::IsCaptureVerboseDumpEnabled())
    {
        // Verbose dump: both characters' positions/anchors for EVERY pass,
        // not only creating passes - shows the mid-batch position vintage
        // each re-simulated frame actually used.
        PublishType47Event(
            base, 0xFFFE, kTraceCharContext, traceEpoch, passSerial);
    }
    const int result = g_origProcessEffects(gameSys);
    PublishType47Event(
        base, 0xFFFF, kTracePassEnd, traceEpoch, passSerial);
    return result;
}

// Dereference Revival's global-state pointer (= the EFZ gameSystem the effect
// ring lives in) for the snapshot boundary events, which fire from Revival
// code and have no gameSys argument of their own.
uintptr_t ReadTraceGameSys()
{
    if (g_traceGameSysPtrAddress == 0)
    {
        return 0;
    }
    uintptr_t gameSys = 0;
    __try
    {
        gameSys = *reinterpret_cast<const volatile uintptr_t*>(
            g_traceGameSysPtrAddress);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        gameSys = 0;
    }
    return gameSys;
}

// One-shot at first traced snapshot save: conditional savestate sub-object
// presence (the replay object at gameSys+4988 is copied only when non-null)
// plus the effects gates - rules out a whole-region coverage or gate
// mismatch from the log alone.  Separate function because SEH __try cannot
// share a frame with the hook's C++ RAII object.
void LogSaveObjectDiagOnce()
{
    if (InterlockedExchange(&g_saveObjectDiagOnce, 1) != 0)
    {
        return;
    }
    const uintptr_t gameSysProbe = ReadTraceGameSys();
    uint32_t replayObj = 0;
    uint32_t replayTag = 0;
    uint8_t effectsGate = 0xFF;
    uint32_t effectsGate72 = 0xFFFFFFFFu;
    __try
    {
        if (gameSysProbe != 0)
        {
            replayObj = *reinterpret_cast<const volatile uint32_t*>(
                gameSysProbe + 4988u);
            if (replayObj != 0)
            {
                replayTag = *reinterpret_cast<const volatile uint32_t*>(
                    replayObj + 4u);
            }
            effectsGate = *reinterpret_cast<const volatile uint8_t*>(
                gameSysProbe + 4966u);
            effectsGate72 = *reinterpret_cast<const volatile uint32_t*>(
                gameSysProbe + 4972u);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    mod::Log(
        "TYPE47_TRACE: save-object diag replayObj=0x%08lX tag=%lu "
        "effectsGate4966=%u gate4972=%lu",
        static_cast<unsigned long>(replayObj),
        static_cast<unsigned long>(replayTag),
        static_cast<unsigned>(effectsGate),
        static_cast<unsigned long>(effectsGate72));
}

void __fastcall HookSnapshotSave(void* thisPtr, void* /*edx*/, char mode)
{
    Type47DetourActivity activity;
    LONG traceEpoch = 0;
    if (CaptureType47TraceEpoch(&traceEpoch))
    {
        LogSaveObjectDiagOnce();
        // Entry marker: this is the state the savestate is about to capture.
        const uint32_t passSerial = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_type47PassSerial, 0, 0));
        const uintptr_t gameSys = ReadTraceGameSys();
        if (gameSys != 0)
        {
            PublishType47Event(
                gameSys, 0xFFFF, kTraceSnapshotSave, traceEpoch, passSerial);
        }
    }
    g_origSnapshotSave(thisPtr, mode);
}

void* __fastcall HookSnapshotLoad(void* thisPtr, void* /*edx*/)
{
    Type47DetourActivity activity;
    {
        // Stash the session frame BEFORE the restore rewinds it; the
        // snapshot_load row pairs it with the post-restore frame column.
        int32_t preFrame = -1;
        int32_t preCommit = -1;
        int32_t preRng = -1;
        if (ReadType47TraceContext(&preFrame, &preCommit, &preRng))
        {
            InterlockedExchange(&g_snapshotLoadFromFrame, preFrame);
        }
    }
    void* const result = g_origSnapshotLoad(thisPtr);
    LONG traceEpoch = 0;
    if (CaptureType47TraceEpoch(&traceEpoch))
    {
        // Exit marker: this is the state as the restore left it.  Comparing
        // it with the preceding snapshot_save row (and the passes between)
        // shows directly whether Load reproduced what Save captured.
        const uint32_t passSerial = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_type47PassSerial, 0, 0));
        const uintptr_t gameSys = ReadTraceGameSys();
        if (gameSys != 0)
        {
            PublishType47Event(
                gameSys, 0xFFFF, kTraceSnapshotLoad, traceEpoch, passSerial);
        }
    }
    return result;
}

// Install the snapshot SAVE/LOAD boundary detours.  Independent of the EFZ
// type-47 hook set: unsupported profiles or a disabled setting leave the
// type-47 trace fully functional with snapshotBoundaryMarkers=0.
int32_t ReadRngEngineStateRaw()
{
    if (g_rngEngineStateAddr == 0)
    {
        return -1;
    }
    int32_t v = -1;
    __try
    {
        v = *reinterpret_cast<const volatile int32_t*>(g_rngEngineStateAddr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        v = -1;
    }
    return v;
}

// Count minstd (Park-Miller x48271 mod 2^31-1) advances from before->after.
int CountRngAdvances(int32_t before, int32_t after, int maxAdvances)
{
    if (before <= 0 || after <= 0)
    {
        return -1;
    }
    if (before == after)
    {
        return 0;
    }
    int64_t state = before;
    for (int i = 1; i <= maxAdvances; ++i)
    {
        state = (state * 48271) % 2147483647;
        if (static_cast<int32_t>(state) == after)
        {
            return i;
        }
    }
    return -1;
}

void PublishRngCallEvent(uint32_t returnAddr, int32_t before, int32_t after)
{
    const LONG traceEpoch = InterlockedCompareExchange(&g_type47TraceEpoch, 0, 0);
    if (traceEpoch <= 0
        || InterlockedCompareExchange(&g_type47TraceEnabled, 0, 0) == 0)
    {
        return;
    }

    RngCallEvent next = {};
    next.sessionGeneration = static_cast<uint32_t>(traceEpoch);
    next.passSerial = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_type47PassSerial, 0, 0));
    next.frame = -1;
    next.commitFrame = -1;
    int32_t ctxRng = -1;
    (void)ReadType47TraceContext(&next.frame, &next.commitFrame, &ctxRng);
    next.returnAddr = returnAddr;
    next.stateBefore = before;
    next.stateAfter = after;
    next.internalAdvances = CountRngAdvances(before, after, 64);
    next.effectSlot = static_cast<uint16_t>(
        InterlockedCompareExchange(&g_rngCurrentEffectSlot, 0xFFFF, 0xFFFF));
    next.effectBehavior = 0xFFFF;
    next.phaseContext = static_cast<uint8_t>(
        InterlockedCompareExchange(&g_rngCurrentEffectPhase, 0, 0));
    next.threadId = GetCurrentThreadId();
    next.effectAnimFrame = 0xFFFF;
    if (next.effectSlot < kEffectRingSlots && g_traceGameSysPtrAddress != 0)
    {
        __try
        {
            const uintptr_t gameSys =
                *reinterpret_cast<const volatile uintptr_t*>(
                    g_traceGameSysPtrAddress);
            if (gameSys != 0)
            {
                const uintptr_t record = gameSys + kEffectRecordBase
                    + kEffectRecordStride * next.effectSlot;
                next.effectAnimFrame =
                    *reinterpret_cast<const volatile uint16_t*>(
                        record + kEffectFieldAnimFrame);
                next.effectYPosBits = ReadGameDoubleBits(
                    record + kEffectFieldPosY);
                next.effectYVelBits = ReadGameDoubleBits(
                    record + kEffectFieldVelY);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
    if (next.frame >= 0)
    {
        const LONG maxSeen =
            InterlockedCompareExchange(&g_maxEventFrame, 0, 0);
        next.resim = (next.frame < maxSeen) ? 1 : 0;
    }

    MemoryBarrier();
    if (traceEpoch != InterlockedCompareExchange(&g_type47TraceEpoch, 0, 0)
        || InterlockedCompareExchange(&g_type47TraceEnabled, 0, 0) == 0)
    {
        return;
    }
    const LONG sequence = InterlockedIncrement(&g_rngTraceWriteSequence);
    const LONG trigger =
        InterlockedCompareExchange(&g_rngTraceEvidenceTrigger, 0, 0);
    const bool preserve =
        trigger > 0
        && static_cast<int64_t>(sequence) - static_cast<int64_t>(trigger)
               > static_cast<int64_t>(kRngPostTriggerEvents);
    RngCallEvent& dest = preserve
        ? g_rngDiscardTrace[
            (static_cast<uint32_t>(sequence) - 1u) & kRngDiscardMask]
        : g_rngTrace[(static_cast<uint32_t>(sequence) - 1u) & kRngTraceMask];
    InterlockedExchange(&dest.stamp, 0);
    const LONG savedStamp = dest.stamp;
    (void)savedStamp;
    std::memcpy(reinterpret_cast<char*>(&dest) + sizeof(LONG),
                reinterpret_cast<char*>(&next) + sizeof(LONG),
                sizeof(RngCallEvent) - sizeof(LONG));
    MemoryBarrier();
    InterlockedExchange(&dest.stamp, sequence);
}

char* __cdecl HookRngReplacement()
{
    Type47DetourActivity activity;
    // Cheapest possible fast-out when not armed - this is the game's rand().
    if (InterlockedCompareExchange(&g_type47TraceEnabled, 0, 0) == 0)
    {
        return g_origRngReplacement();
    }
    const uint32_t returnAddr =
        reinterpret_cast<uint32_t>(_ReturnAddress());
    const int32_t before = ReadRngEngineStateRaw();
    char* const result = g_origRngReplacement();
    const int32_t after = ReadRngEngineStateRaw();
    PublishRngCallEvent(returnAddr, before, after);
    return result;
}

char __stdcall HookRngSeedApply(unsigned int seed)
{
    Type47DetourActivity activity;
    if (!netplay::mod_settings::IsRngCallTraceEnabled())
    {
        return g_origRngSeedApply(seed);
    }
    const int32_t before = ReadRngEngineStateRaw();
    const char result = g_origRngSeedApply(seed);
    const int32_t after = ReadRngEngineStateRaw();

    // Dedup: log only when the applied seed value CHANGES (battle-start seed,
    // a genuine reseed, or a restore that reapplied a different value) - an
    // idempotent per-frame restore of the same value logs once then goes
    // quiet.  A hard cap prevents any pathological reseed loop from flooding.
    const LONG seedL = static_cast<LONG>(seed);
    const bool changed =
        InterlockedExchange(&g_lastAppliedSeed, seedL) != seedL;
    if (changed
        && InterlockedIncrement(&g_seedApplyLogCount) <= 4096)
    {
        int32_t frame = -1;
        int32_t commit = -1;
        int32_t ctxRng = -1;
        (void)ReadType47TraceContext(&frame, &commit, &ctxRng);
        mod::Log(
            "RNG_SEED_APPLY seed=%ld (0x%08lX) engineBefore=%ld engineAfter=%ld "
            "frame=%d commit=%d thread=%lu",
            static_cast<long>(seedL),
            static_cast<unsigned long>(seed),
            static_cast<long>(before),
            static_cast<long>(after),
            frame, commit,
            static_cast<unsigned long>(GetCurrentThreadId()));
    }
    return result;
}

bool InstallRngSeedApplyHook()
{
    if (!netplay::mod_settings::IsRngCallTraceEnabled() || g_rngSeedHookFailed)
    {
        return false;
    }
    const netplay::bridge::takeover::RevivalAddressProfile* profile =
        netplay::bridge::takeover::g_activeRevival;
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (profile == nullptr || revival == nullptr
        || profile->rngSeedApplyRva == 0
        || profile->rngEngineStateOffset == 0)
    {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    g_rngSeedApplyAddr = base + profile->rngSeedApplyRva;
    if (g_rngEngineStateAddr == 0)
    {
        g_rngEngineStateAddr = base + profile->rngEngineStateOffset;
    }
    if (g_rngSeedHookInstalled)
    {
        const MH_STATUS e =
            MH_EnableHook(reinterpret_cast<void*>(g_rngSeedApplyAddr));
        return e == MH_OK || e == MH_ERROR_ENABLED;
    }
    // MSVC e-i share prologue 55 8B EC 8B 4D 08 (push ebp; mov ebp,esp;
    // mov ecx,[ebp+8]).
    static const uint8_t kSeedBytes[] = {0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08};
    if (!TraceBytesMatch(g_rngSeedApplyAddr, kSeedBytes, sizeof(kSeedBytes)))
    {
        g_rngSeedHookFailed = true;
        mod::Log(
            "RNG_TRACE: seed-apply signature mismatch @0x%08lX; seed trace "
            "disabled fail-closed",
            static_cast<unsigned long>(g_rngSeedApplyAddr));
        return false;
    }
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        g_rngSeedHookFailed = true;
        return false;
    }
    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<void*>(g_rngSeedApplyAddr),
        reinterpret_cast<void*>(&HookRngSeedApply),
        reinterpret_cast<void**>(&g_origRngSeedApply));
    if (create != MH_OK)
    {
        g_rngSeedHookFailed = true;
        mod::Log("RNG_TRACE: seed-apply MH_CreateHook failed status=%d", create);
        return false;
    }
    g_rngSeedOwnedHookMask = 0x01u;
    if (MH_EnableHook(reinterpret_cast<void*>(g_rngSeedApplyAddr)) != MH_OK)
    {
        g_rngSeedHookFailed = true;
        return false;
    }
    g_rngSeedHookInstalled = true;
    mod::Log(
        "RNG_TRACE: seed-apply tracer installed @0x%08lX revival=%s",
        static_cast<unsigned long>(g_rngSeedApplyAddr),
        profile->versionTag != nullptr ? profile->versionTag : "unknown");
    return true;
}

bool DisableRngSeedApplyHookForIdle()
{
    if (!g_rngSeedHookInstalled || (g_rngSeedOwnedHookMask & 0x01u) == 0)
    {
        return true;
    }
    const MH_STATUS s =
        MH_DisableHook(reinterpret_cast<void*>(g_rngSeedApplyAddr));
    return s == MH_OK || s == MH_ERROR_DISABLED;
}

bool RemoveRngSeedApplyHook()
{
    if (!g_rngSeedHookInstalled)
    {
        return true;
    }
    if (!DisableRngSeedApplyHookForIdle())
    {
        return false;
    }
    if ((g_rngSeedOwnedHookMask & 0x01u) != 0)
    {
        const MH_STATUS s =
            MH_RemoveHook(reinterpret_cast<void*>(g_rngSeedApplyAddr));
        if (s != MH_OK && s != MH_ERROR_NOT_CREATED)
        {
            return false;
        }
        g_rngSeedOwnedHookMask = 0;
    }
    g_rngSeedHookInstalled = false;
    g_origRngSeedApply = nullptr;
    return true;
}

bool InstallRngCallTracerHook()
{
    g_rngTraceAvailableForSession = false;
    if (!netplay::mod_settings::IsRngCallTraceEnabled())
    {
        return false;
    }
    if (g_rngHookFailed)
    {
        return false;
    }
    const netplay::bridge::takeover::RevivalAddressProfile* profile =
        netplay::bridge::takeover::g_activeRevival;
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (profile == nullptr || revival == nullptr
        || profile->rngReplacementRva == 0
        || profile->rngEngineStateOffset == 0)
    {
        mod::Log(
            "RNG_TRACE: call tracer unavailable (no byte-verified rand "
            "replacement RVA for revival=%s)",
            (profile != nullptr && profile->versionTag != nullptr)
                ? profile->versionTag : "unknown");
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    g_rngReplacementAddr = base + profile->rngReplacementRva;
    g_rngEngineStateAddr = base + profile->rngEngineStateOffset;

    if (g_rngHookInstalled)
    {
        const MH_STATUS e =
            MH_EnableHook(reinterpret_cast<void*>(g_rngReplacementAddr));
        const bool ok = (e == MH_OK || e == MH_ERROR_ENABLED);
        g_rngTraceAvailableForSession = ok;
        return ok;
    }

    // 1.02h prologue byte-verified: 55 8B EC 83 EC 08.
    static const uint8_t kRngBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};
    if (!TraceBytesMatch(g_rngReplacementAddr, kRngBytes, sizeof(kRngBytes)))
    {
        g_rngHookFailed = true;
        mod::Log(
            "RNG_TRACE: rand-replacement signature mismatch @0x%08lX; "
            "tracer disabled fail-closed",
            static_cast<unsigned long>(g_rngReplacementAddr));
        return false;
    }
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        g_rngHookFailed = true;
        mod::Log("RNG_TRACE: MH_Initialize failed status=%d", initStatus);
        return false;
    }
    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<void*>(g_rngReplacementAddr),
        reinterpret_cast<void*>(&HookRngReplacement),
        reinterpret_cast<void**>(&g_origRngReplacement));
    if (create != MH_OK)
    {
        g_rngHookFailed = true;
        mod::Log("RNG_TRACE: MH_CreateHook failed status=%d", create);
        return false;
    }
    g_rngOwnedHookMask = 0x01u;
    const MH_STATUS enable =
        MH_EnableHook(reinterpret_cast<void*>(g_rngReplacementAddr));
    if (enable != MH_OK)
    {
        g_rngHookFailed = true;
        mod::Log("RNG_TRACE: MH_EnableHook failed status=%d", enable);
        return false;
    }
    g_rngHookInstalled = true;
    g_rngTraceAvailableForSession = true;
    mod::Log(
        "RNG_TRACE: rand-replacement call tracer installed @0x%08lX "
        "engine=0x%08lX revival=%s",
        static_cast<unsigned long>(g_rngReplacementAddr),
        static_cast<unsigned long>(g_rngEngineStateAddr),
        profile->versionTag != nullptr ? profile->versionTag : "unknown");
    return true;
}

bool DisableRngCallTracerHookForIdle()
{
    g_rngTraceAvailableForSession = false;
    if (!g_rngHookInstalled || (g_rngOwnedHookMask & 0x01u) == 0)
    {
        return true;
    }
    const MH_STATUS s =
        MH_DisableHook(reinterpret_cast<void*>(g_rngReplacementAddr));
    return s == MH_OK || s == MH_ERROR_DISABLED;
}

bool RemoveRngCallTracerHook()
{
    if (!g_rngHookInstalled)
    {
        return true;
    }
    if (!DisableRngCallTracerHookForIdle())
    {
        return false;
    }
    if ((g_rngOwnedHookMask & 0x01u) != 0)
    {
        const MH_STATUS s =
            MH_RemoveHook(reinterpret_cast<void*>(g_rngReplacementAddr));
        if (s != MH_OK && s != MH_ERROR_NOT_CREATED)
        {
            return false;
        }
        g_rngOwnedHookMask = 0;
    }
    g_rngHookInstalled = false;
    g_origRngReplacement = nullptr;
    return true;
}

bool InstallSnapshotBoundaryHooks()
{
    g_snapshotMarkersAvailableForSession = false;
    if (!netplay::mod_settings::IsSnapshotBoundaryMarkersEnabled())
    {
        mod::Log(
            "TYPE47_TRACE: snapshot boundary markers disabled by "
            "ExperimentalSnapshotBoundaryMarkers=0");
        return false;
    }
    if (g_snapshotHooksFailed)
    {
        return false;
    }

    const netplay::bridge::takeover::RevivalAddressProfile* profile =
        netplay::bridge::takeover::g_activeRevival;
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (profile == nullptr || revival == nullptr
        || profile->snapshotSaveRva == 0 || profile->snapshotLoadRva == 0
        || profile->globalStatePtrOffset == 0)
    {
        mod::Log(
            "TYPE47_TRACE: snapshot boundary markers unavailable "
            "(no byte-verified save/load RVAs for revival=%s)",
            (profile != nullptr && profile->versionTag != nullptr)
                ? profile->versionTag
                : "unknown");
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    g_traceGameSysPtrAddress = base + profile->globalStatePtrOffset;

    if (g_snapshotHooksInstalled)
    {
        if (g_snapshotOwnedHookMask != 0x03u)
        {
            g_snapshotHooksFailed = true;
            mod::Log(
                "TYPE47_TRACE: incomplete owned snapshot hook set mask=0x%02X; "
                "re-enable refused",
                static_cast<unsigned>(g_snapshotOwnedHookMask));
            return false;
        }
        const MH_STATUS e0 =
            MH_EnableHook(reinterpret_cast<void*>(g_snapshotSaveAddr));
        const MH_STATUS e1 =
            MH_EnableHook(reinterpret_cast<void*>(g_snapshotLoadAddr));
        const bool ok =
            (e0 == MH_OK || e0 == MH_ERROR_ENABLED)
            && (e1 == MH_OK || e1 == MH_ERROR_ENABLED);
        if (!ok)
        {
            mod::Log(
                "TYPE47_TRACE: snapshot re-enable failed save=%d load=%d",
                e0, e1);
        }
        g_snapshotMarkersAvailableForSession = ok;
        return ok;
    }

    g_snapshotSaveAddr = base + profile->snapshotSaveRva;
    g_snapshotLoadAddr = base + profile->snapshotLoadRva;

    // 1.02h prologues, byte-verified against the archived binary.  Both use
    // MSVC SEH frames whose pushed handler address is relocation-dependent,
    // so the signatures skip those imm32 bytes: save checks bytes [0..6) and
    // [10..16), load checks the relocation-invariant first 9 bytes.
    static const uint8_t kSaveBytesA[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68};
    static const uint8_t kSaveBytesB[] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t kLoadBytes[] = {
        0x55, 0x8B, 0xEC, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00};
    if (!TraceBytesMatch(g_snapshotSaveAddr, kSaveBytesA, sizeof(kSaveBytesA))
        || !TraceBytesMatch(
            g_snapshotSaveAddr + 10u, kSaveBytesB, sizeof(kSaveBytesB))
        || !TraceBytesMatch(g_snapshotLoadAddr, kLoadBytes, sizeof(kLoadBytes)))
    {
        g_snapshotHooksFailed = true;
        mod::Log(
            "TYPE47_TRACE: snapshot hook signature mismatch; boundary "
            "markers disabled fail-closed (save=0x%08lX load=0x%08lX)",
            static_cast<unsigned long>(g_snapshotSaveAddr),
            static_cast<unsigned long>(g_snapshotLoadAddr));
        return false;
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        g_snapshotHooksFailed = true;
        mod::Log(
            "TYPE47_TRACE: MH_Initialize failed for snapshot hooks status=%d",
            initStatus);
        return false;
    }

    const MH_STATUS createSave = MH_CreateHook(
        reinterpret_cast<void*>(g_snapshotSaveAddr),
        reinterpret_cast<void*>(&HookSnapshotSave),
        reinterpret_cast<void**>(&g_origSnapshotSave));
    const MH_STATUS createLoad = MH_CreateHook(
        reinterpret_cast<void*>(g_snapshotLoadAddr),
        reinterpret_cast<void*>(&HookSnapshotLoad),
        reinterpret_cast<void**>(&g_origSnapshotLoad));
    g_snapshotOwnedHookMask = 0;
    if (createSave == MH_OK)
    {
        g_snapshotOwnedHookMask |= 0x01u;
    }
    if (createLoad == MH_OK)
    {
        g_snapshotOwnedHookMask |= 0x02u;
    }
    if (createSave != MH_OK || createLoad != MH_OK)
    {
        if ((g_snapshotOwnedHookMask & 0x01u) != 0)
        {
            (void)MH_RemoveHook(reinterpret_cast<void*>(g_snapshotSaveAddr));
        }
        if ((g_snapshotOwnedHookMask & 0x02u) != 0)
        {
            (void)MH_RemoveHook(reinterpret_cast<void*>(g_snapshotLoadAddr));
        }
        g_snapshotOwnedHookMask = 0;
        g_snapshotHooksFailed = true;
        mod::Log(
            "TYPE47_TRACE: snapshot hook create failed save=%d load=%d",
            createSave, createLoad);
        return false;
    }
    const MH_STATUS enableSave =
        MH_EnableHook(reinterpret_cast<void*>(g_snapshotSaveAddr));
    const MH_STATUS enableLoad =
        MH_EnableHook(reinterpret_cast<void*>(g_snapshotLoadAddr));
    if (enableSave != MH_OK || enableLoad != MH_OK)
    {
        g_snapshotHooksFailed = true;
        mod::Log(
            "TYPE47_TRACE: snapshot hook enable failed save=%d load=%d",
            enableSave, enableLoad);
        return false;
    }
    g_snapshotHooksInstalled = true;
    g_snapshotMarkersAvailableForSession = true;
    mod::Log(
        "TYPE47_TRACE: snapshot SAVE/LOAD boundary hooks installed "
        "(save=0x%08lX load=0x%08lX revival=%s)",
        static_cast<unsigned long>(g_snapshotSaveAddr),
        static_cast<unsigned long>(g_snapshotLoadAddr),
        profile->versionTag != nullptr ? profile->versionTag : "unknown");
    return true;
}

bool InstallType47TraceHooks()
{
    // A failed install may still own one or more MinHook records when rollback
    // could not complete. Never try to re-enable that partial set; final
    // shutdown retains ownership and will retry removal.
    if (g_type47HooksFailed)
    {
        return false;
    }
    if (g_type47HooksInstalled)
    {
        if (g_type47OwnedHookMask != 0x0Fu)
        {
            g_type47HooksFailed = true;
            mod::Log(
                "TYPE47_TRACE: incomplete owned hook set mask=0x%02X; "
                "re-enable refused",
                static_cast<unsigned>(g_type47OwnedHookMask));
            return false;
        }
        const MH_STATUS e0 = MH_EnableHook(reinterpret_cast<void*>(kInitializeEffectAddr));
        const MH_STATUS e1 = MH_EnableHook(reinterpret_cast<void*>(kClearEffectAddr));
        const MH_STATUS e2 = MH_EnableHook(reinterpret_cast<void*>(kProcessEffectsAddr));
        const MH_STATUS e3 = MH_EnableHook(reinterpret_cast<void*>(kProcessEffectAddr));
        const bool ok =
            (e0 == MH_OK || e0 == MH_ERROR_ENABLED)
            && (e1 == MH_OK || e1 == MH_ERROR_ENABLED)
            && (e2 == MH_OK || e2 == MH_ERROR_ENABLED)
            && (e3 == MH_OK || e3 == MH_ERROR_ENABLED);
        if (!ok)
        {
            mod::Log(
                "TYPE47_TRACE: re-enable failed init=%d clear=%d pass=%d one=%d",
                e0, e1, e2, e3);
        }
        return ok;
    }
    static const uint8_t kInitBytes[] = {0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC};
    static const uint8_t kClearBytes[] = {0x55, 0x8B, 0xEC, 0x51, 0x89, 0x4D, 0xFC};
    static const uint8_t kProcessBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};
    static const uint8_t kProcessOneBytes[] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xDC, 0x02, 0x00, 0x00};
    if (!TraceBytesMatch(kProcessEffectAddr, kProcessOneBytes, sizeof(kProcessOneBytes))
        || !TraceBytesMatch(kInitializeEffectAddr, kInitBytes, sizeof(kInitBytes))
        || !TraceBytesMatch(kClearEffectAddr, kClearBytes, sizeof(kClearBytes))
        || !TraceBytesMatch(kProcessEffectsAddr, kProcessBytes, sizeof(kProcessBytes)))
    {
        g_type47HooksFailed = true;
        mod::Log(
            "TYPE47_TRACE: EFZ hook signature mismatch; tracer disabled fail-closed");
        return false;
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        g_type47HooksFailed = true;
        mod::Log("TYPE47_TRACE: MH_Initialize failed status=%d", initStatus);
        return false;
    }

    const MH_STATUS createInit = MH_CreateHook(
        reinterpret_cast<void*>(kInitializeEffectAddr),
        reinterpret_cast<void*>(&HookInitializeEffect),
        reinterpret_cast<void**>(&g_origInitializeEffect));
    const MH_STATUS createClear = MH_CreateHook(
        reinterpret_cast<void*>(kClearEffectAddr),
        reinterpret_cast<void*>(&HookClearEffect),
        reinterpret_cast<void**>(&g_origClearEffect));
    const MH_STATUS createPass = MH_CreateHook(
        reinterpret_cast<void*>(kProcessEffectsAddr),
        reinterpret_cast<void*>(&HookProcessEffects),
        reinterpret_cast<void**>(&g_origProcessEffects));
    const MH_STATUS createOne = MH_CreateHook(
        reinterpret_cast<void*>(kProcessEffectAddr),
        reinterpret_cast<void*>(&HookProcessEffect),
        reinterpret_cast<void**>(&g_origProcessEffect));
    const uintptr_t targets[] = {
        kInitializeEffectAddr,
        kClearEffectAddr,
        kProcessEffectsAddr,
        kProcessEffectAddr,
    };
    const MH_STATUS creates[] = {
        createInit,
        createClear,
        createPass,
        createOne,
    };
    g_type47OwnedHookMask = 0;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
    {
        if (creates[i] == MH_OK)
        {
            g_type47OwnedHookMask |= static_cast<uint8_t>(1u << i);
        }
    }
    if (createInit != MH_OK || createClear != MH_OK
        || createPass != MH_OK || createOne != MH_OK)
    {
        // Remove only targets created by this call. Never remove another
        // feature's pre-existing MinHook record.
        for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
        {
            const uint8_t bit = static_cast<uint8_t>(1u << i);
            if ((g_type47OwnedHookMask & bit) == 0)
            {
                continue;
            }
            const MH_STATUS status =
                MH_RemoveHook(reinterpret_cast<void*>(targets[i]));
            if (status != MH_OK && status != MH_ERROR_NOT_CREATED)
            {
                mod::Log(
                    "TYPE47_TRACE: create rollback remove failed "
                    "target=0x%08lX status=%d",
                    static_cast<unsigned long>(targets[i]), status);
            }
            else
            {
                g_type47OwnedHookMask &= static_cast<uint8_t>(~bit);
            }
        }
        const bool rollbackOk = g_type47OwnedHookMask == 0;
        g_type47HooksInstalled = !rollbackOk;
        if (rollbackOk)
        {
            g_origInitializeEffect = nullptr;
            g_origClearEffect = nullptr;
            g_origProcessEffects = nullptr;
            g_origProcessEffect = nullptr;
        }
        g_type47HooksFailed = true;
        mod::Log(
            "TYPE47_TRACE: hook creation failed init=%d clear=%d pass=%d "
            "one=%d rollback=%s",
            createInit, createClear, createPass, createOne,
            rollbackOk ? "complete" : "retained-for-shutdown");
        return false;
    }

    const MH_STATUS enableInit =
        MH_EnableHook(reinterpret_cast<void*>(kInitializeEffectAddr));
    const MH_STATUS enableClear =
        MH_EnableHook(reinterpret_cast<void*>(kClearEffectAddr));
    const MH_STATUS enablePass =
        MH_EnableHook(reinterpret_cast<void*>(kProcessEffectsAddr));
    const MH_STATUS enableOne =
        MH_EnableHook(reinterpret_cast<void*>(kProcessEffectAddr));
    if (enableInit != MH_OK || enableClear != MH_OK
        || enablePass != MH_OK || enableOne != MH_OK)
    {
        bool rollbackDisabled = true;
        for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
        {
            const uint8_t bit = static_cast<uint8_t>(1u << i);
            if ((g_type47OwnedHookMask & bit) == 0)
            {
                continue;
            }
            const MH_STATUS status =
                MH_DisableHook(reinterpret_cast<void*>(targets[i]));
            if (status != MH_OK && status != MH_ERROR_DISABLED
                && status != MH_ERROR_NOT_CREATED)
            {
                rollbackDisabled = false;
                mod::Log(
                    "TYPE47_TRACE: enable rollback disable failed "
                    "target=0x%08lX status=%d",
                    static_cast<unsigned long>(targets[i]), status);
            }
        }

        bool rollbackRemoved = rollbackDisabled;
        if (rollbackDisabled)
        {
            const DWORD drainStart = GetTickCount();
            while (InterlockedCompareExchange(
                       &g_type47DetoursActive, 0, 0) != 0
                && GetTickCount() - drainStart < 1000u)
            {
                Sleep(1);
            }
            if (InterlockedCompareExchange(
                    &g_type47DetoursActive, 0, 0) != 0)
            {
                rollbackRemoved = false;
            }
        }
        if (rollbackRemoved)
        {
            for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
            {
                const uint8_t bit = static_cast<uint8_t>(1u << i);
                if ((g_type47OwnedHookMask & bit) == 0)
                {
                    continue;
                }
                const MH_STATUS status =
                    MH_RemoveHook(reinterpret_cast<void*>(targets[i]));
                if (status != MH_OK && status != MH_ERROR_NOT_CREATED)
                {
                    rollbackRemoved = false;
                    mod::Log(
                        "TYPE47_TRACE: enable rollback remove failed "
                        "target=0x%08lX status=%d",
                        static_cast<unsigned long>(targets[i]), status);
                }
                else
                {
                    g_type47OwnedHookMask &= static_cast<uint8_t>(~bit);
                }
            }
        }
        const bool rollbackOk = rollbackDisabled && rollbackRemoved
            && g_type47OwnedHookMask == 0;
        g_type47HooksInstalled = !rollbackOk;
        if (rollbackOk)
        {
            g_origInitializeEffect = nullptr;
            g_origClearEffect = nullptr;
            g_origProcessEffects = nullptr;
            g_origProcessEffect = nullptr;
        }
        g_type47HooksFailed = true;
        mod::Log(
            "TYPE47_TRACE: hook enable failed init=%d clear=%d pass=%d "
            "one=%d rollback=%s",
            enableInit, enableClear, enablePass, enableOne,
            rollbackOk ? "complete" : "retained-for-shutdown");
        return false;
    }

    g_type47HooksInstalled = true;
    g_type47OwnedHookMask = 0x0Fu;
    mod::Log(
        "TYPE47_TRACE: fixed-address create/clear/pass/update hooks installed");
    return true;
}

bool DisableSnapshotBoundaryHooksForIdle();
bool RemoveSnapshotBoundaryHooks();

bool DisableAndRemoveType47TraceHooks()
{
    InterlockedExchange(&g_type47TraceSealed, 1);
    InvalidateType47TraceProducers();
    g_type47TraceAvailableForSession = false;
    // Snapshot boundary detours share the disable+drain discipline; their
    // removal below happens after the shared callback drain.
    (void)DisableSnapshotBoundaryHooksForIdle();
    (void)DisableRngCallTracerHookForIdle();
    (void)DisableRngSeedApplyHookForIdle();
    if (!g_type47HooksInstalled)
    {
        (void)RemoveSnapshotBoundaryHooks();
        (void)RemoveRngCallTracerHook();
        (void)RemoveRngSeedApplyHook();
        return true;
    }

    const uintptr_t targets[] = {
        kInitializeEffectAddr,
        kClearEffectAddr,
        kProcessEffectsAddr,
        kProcessEffectAddr,
    };
    bool disabled = true;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
    {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if ((g_type47OwnedHookMask & bit) == 0)
        {
            continue;
        }
        const MH_STATUS status =
            MH_DisableHook(reinterpret_cast<void*>(targets[i]));
        if (status != MH_OK && status != MH_ERROR_DISABLED
            && status != MH_ERROR_NOT_CREATED)
        {
            disabled = false;
            mod::Log(
                "TYPE47_TRACE: disable failed target=0x%08lX status=%d",
                static_cast<unsigned long>(targets[i]), status);
        }
    }

    if (!disabled)
    {
        mod::Log(
            "TYPE47_TRACE: removal skipped because one or more detours "
            "could not be disabled");
        return false;
    }

    bool removed = true;
    // Per-session transitions only disable these hooks; final removal occurs
    // during module shutdown. At that point no new detour can enter after the
    // successful MH_DisableHook calls. Wait for any already-entered callback
    // to return through its still-valid trampoline before freeing it.
    const DWORD drainStart = GetTickCount();
    while (InterlockedCompareExchange(&g_type47DetoursActive, 0, 0) != 0
        && GetTickCount() - drainStart < 1000u)
    {
        Sleep(1);
    }
    if (InterlockedCompareExchange(&g_type47DetoursActive, 0, 0) != 0)
    {
        mod::Log(
            "TYPE47_TRACE: removal deferred; %ld detour callbacks still active",
            static_cast<long>(InterlockedCompareExchange(
                &g_type47DetoursActive, 0, 0)));
        return false;
    }
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
    {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if ((g_type47OwnedHookMask & bit) == 0)
        {
            continue;
        }
        const MH_STATUS status =
            MH_RemoveHook(reinterpret_cast<void*>(targets[i]));
        if (status != MH_OK && status != MH_ERROR_NOT_CREATED)
        {
            removed = false;
            mod::Log(
                "TYPE47_TRACE: remove failed target=0x%08lX status=%d",
                static_cast<unsigned long>(targets[i]), status);
        }
        else
        {
            g_type47OwnedHookMask &= static_cast<uint8_t>(~bit);
        }
    }

    removed = removed && g_type47OwnedHookMask == 0;
    if (removed)
    {
        g_type47HooksInstalled = false;
        g_origInitializeEffect = nullptr;
        g_origClearEffect = nullptr;
        g_origProcessEffects = nullptr;
        g_origProcessEffect = nullptr;
    }
    // The shared callback drain above also covered the snapshot detours
    // (they use the same Type47DetourActivity counter).
    const bool snapshotRemoved = RemoveSnapshotBoundaryHooks();
    const bool rngRemoved = RemoveRngCallTracerHook();
    const bool rngSeedRemoved = RemoveRngSeedApplyHook();
    mod::Log(
        "TYPE47_TRACE: hooks quiesced disabled=%d removed=%d snapshotRemoved=%d "
        "rngRemoved=%d rngSeedRemoved=%d",
        disabled ? 1 : 0,
        removed ? 1 : 0,
        snapshotRemoved ? 1 : 0,
        rngRemoved ? 1 : 0,
        rngSeedRemoved ? 1 : 0);
    return disabled && removed && snapshotRemoved && rngRemoved && rngSeedRemoved;
}

// Disable (keep created) the snapshot boundary detours between sessions,
// mirroring the per-session idle policy of the EFZ type-47 hooks.
bool DisableSnapshotBoundaryHooksForIdle()
{
    g_snapshotMarkersAvailableForSession = false;
    (void)DisableRngCallTracerHookForIdle();
    (void)DisableRngSeedApplyHookForIdle();
    if (!g_snapshotHooksInstalled)
    {
        return true;
    }
    const uintptr_t targets[] = {g_snapshotSaveAddr, g_snapshotLoadAddr};
    bool ok = true;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
    {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if ((g_snapshotOwnedHookMask & bit) == 0 || targets[i] == 0)
        {
            continue;
        }
        const MH_STATUS status =
            MH_DisableHook(reinterpret_cast<void*>(targets[i]));
        if (status != MH_OK && status != MH_ERROR_DISABLED)
        {
            ok = false;
            mod::Log(
                "TYPE47_TRACE: snapshot idle disable failed target=0x%08lX status=%d",
                static_cast<unsigned long>(targets[i]), status);
        }
    }
    return ok;
}

// Final removal of the snapshot boundary detours at module shutdown, after
// the same disable + callback-drain discipline as the EFZ hooks.
bool RemoveSnapshotBoundaryHooks()
{
    if (!g_snapshotHooksInstalled)
    {
        return true;
    }
    if (!DisableSnapshotBoundaryHooksForIdle())
    {
        return false;
    }
    const uintptr_t targets[] = {g_snapshotSaveAddr, g_snapshotLoadAddr};
    bool removed = true;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
    {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if ((g_snapshotOwnedHookMask & bit) == 0 || targets[i] == 0)
        {
            continue;
        }
        const MH_STATUS status =
            MH_RemoveHook(reinterpret_cast<void*>(targets[i]));
        if (status != MH_OK && status != MH_ERROR_NOT_CREATED)
        {
            removed = false;
            mod::Log(
                "TYPE47_TRACE: snapshot remove failed target=0x%08lX status=%d",
                static_cast<unsigned long>(targets[i]), status);
        }
        else
        {
            g_snapshotOwnedHookMask &= static_cast<uint8_t>(~bit);
        }
    }
    removed = removed && g_snapshotOwnedHookMask == 0;
    if (removed)
    {
        g_snapshotHooksInstalled = false;
        g_origSnapshotSave = nullptr;
        g_origSnapshotLoad = nullptr;
    }
    return removed;
}

bool DisableType47TraceHooksForIdle()
{
    InterlockedExchange(&g_type47TraceSealed, 1);
    InvalidateType47TraceProducers();
    g_type47TraceAvailableForSession = false;
    (void)DisableSnapshotBoundaryHooksForIdle();
    if (!g_type47HooksInstalled)
    {
        return true;
    }
    const uintptr_t targets[] = {
        kInitializeEffectAddr,
        kClearEffectAddr,
        kProcessEffectsAddr,
        kProcessEffectAddr,
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); ++i)
    {
        const uint8_t bit = static_cast<uint8_t>(1u << i);
        if ((g_type47OwnedHookMask & bit) == 0)
        {
            continue;
        }
        const MH_STATUS status =
            MH_DisableHook(reinterpret_cast<void*>(targets[i]));
        if (status != MH_OK && status != MH_ERROR_DISABLED)
        {
            ok = false;
            mod::Log(
                "TYPE47_TRACE: idle disable failed target=0x%08lX status=%d",
                static_cast<unsigned long>(targets[i]), status);
        }
    }
    return ok;
}

void ConfigureType47TraceContext()
{
    g_traceSessionPtrAddress = 0;
    g_traceRngStateAddress = 0;
    g_traceCurrentFrameOffset = 0;
    g_traceGameModeSnapshotOffset = 0;

    const netplay::bridge::takeover::RevivalAddressProfile* profile =
        netplay::bridge::takeover::g_activeRevival;
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (profile == nullptr || revival == nullptr
        || profile->sessionPtrOffsetCount == 0
        || profile->rngEngineStateOffset == 0)
    {
        return;
    }
    const uintptr_t revivalBase = reinterpret_cast<uintptr_t>(revival);
    g_traceSessionPtrAddress = revivalBase + profile->sessionPtrOffsets[0];
    g_traceRngStateAddress = revivalBase + profile->rngEngineStateOffset;
    g_traceCurrentFrameOffset = profile->sessionOffsetCurrentFrame;
    g_traceGameModeSnapshotOffset = profile->sessionOffsetGameModeSnapshot;
}

void ResetType47TraceForSession(bool hooksReady)
{
    InvalidateType47TraceProducers();
    InterlockedExchange(&g_type47EvidenceTriggerSequence, 0);
    InterlockedExchange(&g_type47PassSerial, 0);
    // Re-arm per-session diagnostic one-shots so the 2nd/3rd-session desync
    // dumps carry the char-context/save-object identity lines (they otherwise
    // print only for the process's first session), and reset the pass-serial
    // dedup companion that the serial reset above would otherwise leave stale.
    InterlockedExchange(&g_charContextLastPass, -1);
    InterlockedExchange(&g_charContextDiagOnce, 0);
    InterlockedExchange(&g_saveObjectDiagOnce, 0);
    ConfigureType47TraceContext();
    const LONG start =
        InterlockedCompareExchange(&g_type47TraceWriteSequence, 0, 0) + 1;
    InterlockedExchange(&g_type47SessionStartSequence, start);
    InterlockedExchange(&g_type47SessionEndSequence, 0);
    const LONG generation = InterlockedIncrement(&g_type47TraceEpoch);
    InterlockedExchange(&g_type47SessionGeneration, generation);
    g_type47TraceAvailableForSession =
        hooksReady
        && g_traceSessionPtrAddress != 0
        && g_traceRngStateAddress != 0;
    InterlockedExchange(&g_type47TraceSealed, 0);
    if (!g_type47TraceAvailableForSession)
    {
        mod::Log(
            "TYPE47_TRACE: detailed trace unavailable hooks=%d sessionAddr=%p rngAddr=%p",
            hooksReady ? 1 : 0,
            reinterpret_cast<void*>(g_traceSessionPtrAddress),
            reinterpret_cast<void*>(g_traceRngStateAddress));
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint32_t Fnv1a(uint32_t hash, const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

uint32_t WireSchemaIdentity()
{
    // Require the same layout and exact diagnostic build. The value is a
    // capability identity, not game state.
    static uint32_t identity = 0;
    if (identity == 0)
    {
        uint32_t hash = Fnv1a(
            2166136261u, &kSchemaLayoutId, sizeof(kSchemaLayoutId));
        const uint32_t packetSize = static_cast<uint32_t>(sizeof(WirePacket));
        hash = Fnv1a(hash, &packetSize, sizeof(packetSize));
        hash = Fnv1a(
            hash,
            netplay::build_info::kVersion,
            std::strlen(netplay::build_info::kVersion));
        hash = Fnv1a(
            hash,
            netplay::build_info::kBuildTimestamp,
            std::strlen(netplay::build_info::kBuildTimestamp));
        identity = hash != 0 ? hash : 1;
    }
    return identity;
}

// Hash a reviewed projection of every active effect slot plus the ring
// cursors. The first cross-peer difference can identify an effect-layer lead,
// but equality does not prove the unselected bytes are equal. Returns 0 when
// the ring is unreadable so "no data" never fakes a mismatch.
uint32_t ComputeEffectRingHash(uintptr_t gameSys, FrameRecord* frameRecord)
{
    if (frameRecord != nullptr)
    {
        frameRecord->effectAllocCursor = 0;
        frameRecord->effectProcCursor = 0;
        frameRecord->effectTraceCount = 0;
        frameRecord->effectTraceOverflow = 0;
    }
    if (gameSys == 0)
    {
        return 0;
    }
    static uint32_t activeFlags[kEffectRingSlots];
    static uint32_t statusFlags[kEffectRingSlots];
    if (g_validatedEffectGameSys != gameSys)
    {
        g_validatedEffectGameSys = gameSys;
        g_effectArenaReadable =
            IsReadableRange(
                reinterpret_cast<const void*>(gameSys + kEffectAllocCursorOffset),
                sizeof(uint16_t) * 2u)
            && IsReadableRange(
                reinterpret_cast<const void*>(gameSys + kEffectActiveFlagBase),
                sizeof(activeFlags))
            && IsReadableRange(
                reinterpret_cast<const void*>(gameSys + kEffectStatusBase),
                sizeof(statusFlags))
            && IsReadableRange(
                reinterpret_cast<const void*>(gameSys + kEffectRecordBase),
                kEffectRecordArenaBytes);
    }
    if (!g_effectArenaReadable)
    {
        return 0;
    }

    uint32_t hash = 2166136261u;
    uint16_t cursors[2] = {0, 0};
    __try
    {
        std::memcpy(
            activeFlags,
            reinterpret_cast<const void*>(gameSys + kEffectActiveFlagBase),
            sizeof(activeFlags));
        std::memcpy(
            statusFlags,
            reinterpret_cast<const void*>(gameSys + kEffectStatusBase),
            sizeof(statusFlags));
        std::memcpy(
            cursors,
            reinterpret_cast<const void*>(gameSys + kEffectAllocCursorOffset),
            sizeof(cursors));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_effectArenaReadable = false;
        return 0;
    }
    hash = Fnv1a(hash, cursors, sizeof(cursors));
    if (frameRecord != nullptr)
    {
        frameRecord->effectAllocCursor = cursors[0];
        frameRecord->effectProcCursor = cursors[1];
    }

    uint32_t activeCount = 0;
    for (uint32_t i = 0; i < kEffectRingSlots; ++i)
    {
        if (activeFlags[i] == 0)
        {
            continue;
        }
        ++activeCount;
        const uintptr_t record =
            gameSys + kEffectRecordBase + kEffectRecordStride * i;
        EffectProjectionRecord fields = {};
        fields.slot = static_cast<uint16_t>(i);
        fields.activeFlag = activeFlags[i];
        fields.status = statusFlags[i];
        __try
        {
            std::memcpy(&fields.behaviorId,
                        reinterpret_cast<const void*>(record + kEffectFieldBehaviorId), 2);
            std::memcpy(&fields.animFrame,
                        reinterpret_cast<const void*>(record + kEffectFieldAnimFrame), 2);
            std::memcpy(&fields.animTick,
                        reinterpret_cast<const void*>(record + kEffectFieldAnimTick), 2);
            std::memcpy(&fields.x,
                        reinterpret_cast<const void*>(record + kEffectFieldPosX), 8);
            std::memcpy(&fields.y,
                        reinterpret_cast<const void*>(record + kEffectFieldPosY), 8);
            std::memcpy(&fields.vx,
                        reinterpret_cast<const void*>(record + kEffectFieldVelX), 8);
            std::memcpy(&fields.vy,
                        reinterpret_cast<const void*>(record + kEffectFieldVelY), 8);
            std::memcpy(&fields.parameter,
                        reinterpret_cast<const void*>(record + kEffectFieldParameter), 4);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_effectArenaReadable = false;
            return 0;
        }
        hash = Fnv1a(hash, &fields.slot, sizeof(fields.slot));
        hash = Fnv1a(hash, &fields.activeFlag, sizeof(fields.activeFlag));
        hash = Fnv1a(hash, &fields.status, sizeof(fields.status));
        hash = Fnv1a(hash, &fields.behaviorId, sizeof(fields.behaviorId));
        hash = Fnv1a(hash, &fields.animFrame, sizeof(fields.animFrame));
        hash = Fnv1a(hash, &fields.animTick, sizeof(fields.animTick));
        hash = Fnv1a(hash, &fields.parameter, sizeof(fields.parameter));
        hash = Fnv1a(hash, &fields.x, sizeof(fields.x));
        hash = Fnv1a(hash, &fields.y, sizeof(fields.y));
        hash = Fnv1a(hash, &fields.vx, sizeof(fields.vx));
        hash = Fnv1a(hash, &fields.vy, sizeof(fields.vy));
        if (frameRecord != nullptr)
        {
            if (frameRecord->effectTraceCount < kEffectTraceSlots)
            {
                frameRecord->effectTrace[frameRecord->effectTraceCount++] = fields;
            }
            else
            {
                ++frameRecord->effectTraceOverflow;
            }
        }
    }
    hash = Fnv1a(hash, &activeCount, sizeof(activeCount));
    return hash != 0 ? hash : 1;
}

// Read the live Revival minstd_rand engine state (the value Revival logs as
// "Rng" and stores in Sync records at +56).  -1 when unavailable.
int32_t ReadRevivalRngState()
{
    // ResetType47TraceForSession resolves this once, before frame capture.
    // Avoid taking the loader path through GetModuleHandleA on every tick.
    const uintptr_t address = g_traceRngStateAddress;
    if (address == 0)
    {
        return -1;
    }
    if (g_validatedRngAddress != address)
    {
        g_validatedRngAddress = address;
        g_rngAddressReadable =
            IsReadableRange(reinterpret_cast<const void*>(address), sizeof(int32_t));
    }
    if (!g_rngAddressReadable)
    {
        return -1;
    }
    int state = -1;
    __try
    {
        std::memcpy(&state, reinterpret_cast<const void*>(address), sizeof(state));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        g_rngAddressReadable = false;
        return -1;
    }
    return state;
}

bool GuardedCopy(const void* src, void* dst, size_t size)
{
    if (src == nullptr || dst == nullptr || size == 0)
    {
        return false;
    }

    bool ok = false;
    __try
    {
        std::memcpy(dst, src, size);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    return ok;
}

template <typename T>
bool GuardedRead(const void* src, T* out)
{
    if (out == nullptr)
    {
        return false;
    }

    T value = {};
    if (!GuardedCopy(src, &value, sizeof(value)))
    {
        return false;
    }
    *out = value;
    return true;
}

bool CopyRegion(uintptr_t src, void* dst, size_t size)
{
    if (src == 0
        || !GuardedCopy(reinterpret_cast<const void*>(src), dst, size))
    {
        if (dst != nullptr && size != 0)
        {
            std::memset(dst, 0, size);
        }
        return false;
    }
    return true;
}

bool ReadHistoryInput(
    uintptr_t sessionPtr,
    uintptr_t vecOffset,
    int frame,
    uint16_t* outInput)
{
    if (outInput == nullptr || sessionPtr == 0 || vecOffset == 0 || frame < 0)
    {
        return false;
    }
    *outInput = 0;
    uintptr_t begin = 0;
    uintptr_t end = 0;
    if (!GuardedRead(
            reinterpret_cast<const void*>(sessionPtr + vecOffset), &begin)
        || !GuardedRead(
            reinterpret_cast<const void*>(sessionPtr + vecOffset + 4), &end)
        || begin == 0
        || end <= begin)
    {
        return false;
    }
    const uintptr_t byteCount = end - begin;
    if ((byteCount & (sizeof(uint16_t) - 1u)) != 0)
    {
        return false;
    }
    const uintptr_t count = byteCount / sizeof(uint16_t);
    if (static_cast<uintptr_t>(frame) >= count)
    {
        return false;
    }
    const uintptr_t elem =
        begin + sizeof(uint16_t) * static_cast<uintptr_t>(frame);
    return GuardedRead(reinterpret_cast<const void*>(elem), outInput);
}

std::string ModuleDirectory()
{
    std::string path = netplay::bridge::takeover::ModulePath(
        netplay::bridge::takeover::SelfModule());
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos)
    {
        path.resize(slash);
    }
    return path;
}

// Length (in 2-byte entries) of a Revival input-history vector.
int ReadHistoryLength(uintptr_t sessionPtr, uintptr_t vecOffset)
{
    uintptr_t begin = 0;
    uintptr_t end = 0;
    if (sessionPtr == 0
        || vecOffset == 0
        || !GuardedRead(
            reinterpret_cast<const void*>(sessionPtr + vecOffset), &begin)
        || !GuardedRead(
            reinterpret_cast<const void*>(sessionPtr + vecOffset + 4), &end)
        || begin == 0
        || end < begin)
    {
        return -1;
    }
    const uintptr_t byteCount = end - begin;
    if ((byteCount & (sizeof(uint16_t) - 1u)) != 0)
    {
        return -1;
    }
    const uintptr_t count = byteCount / sizeof(uint16_t);
    return count <= 0x7FFFFFFFu ? static_cast<int>(count) : -1;
}

// Head/tail of a Revival named shared-memory wire (same 8-byte header the
// SYNC_DIAG ring probe reads).
bool ProbeWireHeadTail(const char* legacyName, DWORD* outHead, DWORD* outTail)
{
    *outHead = 0;
    *outTail = 0;
    const char* wireName = netplay::bridge::takeover::RevivalWireName(legacyName);
    HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, wireName);
    if (mapping == nullptr)
    {
        return false;
    }
    bool ok = false;
    const volatile DWORD* view = static_cast<const volatile DWORD*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 8));
    if (view != nullptr)
    {
        *outHead = view[0];
        *outTail = view[1];
        ok = true;
        UnmapViewOfFile(const_cast<DWORD*>(view));
    }
    CloseHandle(mapping);
    return ok;
}

// Decode the synced meta byte (high byte of the input pair): bit 0x20 is
// ESC, bits 0x01..0x10 are F4..F8.
void FormatInputPair(char* out, size_t outSize, uint16_t pair)
{
    const uint8_t pad = static_cast<uint8_t>(pair & 0xFF);
    const uint8_t meta = static_cast<uint8_t>(pair >> 8);
    char metaText[32] = {};
    int pos = 0;
    if ((meta & 0x20) != 0) pos += std::snprintf(metaText + pos, sizeof(metaText) - pos, "ESC ");
    for (int bit = 0; bit < 5; ++bit)
    {
        if ((meta & (1u << bit)) != 0)
        {
            pos += std::snprintf(metaText + pos, sizeof(metaText) - pos, "F%d ", 4 + bit);
        }
    }
    if (pos > 0)
    {
        metaText[pos - 1] = '\0';
    }
    std::snprintf(
        out,
        outSize,
        "pad=%02X meta=%02X%s%s%s",
        pad,
        meta,
        metaText[0] != '\0' ? " [" : "",
        metaText,
        metaText[0] != '\0' ? "]" : "");
}

// Session-object wchar_t[64] name field (raw array inside the config
// snapshot; may sit at an unaligned offset) converted to UTF-8.
void ReadSessionName(uintptr_t sessionPtr, uintptr_t nameOffset, char* out, size_t outSize)
{
    out[0] = '\0';
    if (sessionPtr == 0 || nameOffset == 0)
    {
        return;
    }
    wchar_t wide[64] = {};
    if (!IsReadableRange(
            reinterpret_cast<const void*>(sessionPtr + nameOffset), sizeof(wide)))
    {
        return;
    }
    std::memcpy(wide, reinterpret_cast<const void*>(sessionPtr + nameOffset), sizeof(wide));
    wide[63] = L'\0';
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, static_cast<int>(outSize), nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Side channel
// ---------------------------------------------------------------------------

bool ParseIpv4(const char* text, uint32_t* outAddr)
{
    // Accept "a.b.c.d" and "a.b.c.d:port"; IPv6 is not supported by the
    // side channel (detection stays passive).
    char host[64] = {};
    size_t n = 0;
    for (; text[n] != '\0' && text[n] != ':' && n < sizeof(host) - 1; ++n)
    {
        host[n] = text[n];
    }
    host[n] = '\0';
    const unsigned long parsed = inet_addr(host);
    if (parsed == INADDR_NONE || parsed == 0)
    {
        return false;
    }
    *outAddr = static_cast<uint32_t>(parsed);
    return true;
}

void CloseSideChannel()
{
    if (g_socket != INVALID_SOCKET)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
}

bool OpenSideChannel()
{
    if (!g_wsaStarted)
    {
        WSADATA wsaData = {};
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        {
            return false;
        }
        g_wsaStarted = true;
    }

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
    {
        return false;
    }

    sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;

    InterlockedExchange(&g_peerEndpointValid, 0);
    std::memset(&g_peerEndpoint, 0, sizeof(g_peerEndpoint));

    if (g_role == 0)
    {
        // Host: fixed listen port; the joiner initiates.
        local.sin_port = htons(static_cast<uint16_t>(g_hostPort + kSideChannelPortOffset));
    }
    else
    {
        // Joiner: ephemeral local port; the host replies to our source addr,
        // which also opens the NAT path (same shape as the game traffic).
        uint32_t hostAddr = 0;
        if (!ParseIpv4(g_peerAddress, &hostAddr))
        {
            mod::Log(
                "DESYNC_MONITOR: side channel disabled (address '%s' is not IPv4)",
                g_peerAddress);
            CloseSideChannel();
            return false;
        }
        g_peerEndpoint.sin_family = AF_INET;
        g_peerEndpoint.sin_addr.s_addr = hostAddr;
        g_peerEndpoint.sin_port =
            htons(static_cast<uint16_t>(g_hostPort + kSideChannelPortOffset));
        InterlockedExchange(&g_peerEndpointValid, 1);
        local.sin_port = 0;
    }

    if (bind(g_socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0)
    {
        mod::Log(
            "DESYNC_MONITOR: side channel bind failed role=%d port=%u err=%d",
            g_role,
            static_cast<unsigned>(g_hostPort + kSideChannelPortOffset),
            WSAGetLastError());
        CloseSideChannel();
        return false;
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(g_socket, FIONBIO, &nonBlocking) != 0)
    {
        mod::Log(
            "DESYNC_MONITOR: side channel nonblocking setup failed err=%d",
            WSAGetLastError());
        CloseSideChannel();
        return false;
    }

    mod::Log(
        "DESYNC_MONITOR: side channel open role=%d listenPort=%u peer='%s'",
        g_role,
        g_role == 0 ? static_cast<unsigned>(g_hostPort + kSideChannelPortOffset) : 0u,
        g_role == 0 ? "(waiting for joiner)" : g_peerAddress);
    return true;
}

bool SendPacket(const WirePacket& packet)
{
    if (g_socket == INVALID_SOCKET
        || InterlockedCompareExchange(&g_peerEndpointValid, 0, 0) == 0)
    {
        return false;
    }

    return sendto(
        g_socket,
        reinterpret_cast<const char*>(&packet),
        static_cast<int>(sizeof(packet)),
        0,
        reinterpret_cast<const sockaddr*>(&g_peerEndpoint),
        sizeof(g_peerEndpoint)) == static_cast<int>(sizeof(packet));
}

bool PacketSourceMatchesPeer(const sockaddr_in& from)
{
    return InterlockedCompareExchange(&g_peerEndpointValid, 0, 0) != 0
        && from.sin_family == AF_INET
        && from.sin_addr.s_addr == g_peerEndpoint.sin_addr.s_addr
        && from.sin_port == g_peerEndpoint.sin_port;
}

void InitializeWireHeader(WirePacket* packet, PacketKind kind)
{
    std::memset(packet, 0, sizeof(*packet));
    packet->magic = kPacketMagic;
    packet->version = kPacketVersion;
    packet->kind = static_cast<uint8_t>(kind);
    packet->role = static_cast<uint8_t>(g_role);
    packet->sessionNonce = g_sessionNonce;
    packet->schemaId = WireSchemaIdentity();
    packet->battleEpoch = static_cast<uint16_t>(
        InterlockedCompareExchange(&g_battleEpoch, 0, 0));
    packet->startFrame = static_cast<int32_t>(
        InterlockedCompareExchange(&g_captureStartFrame, -1, -1));
}

void SendControlPacket(PacketKind kind, int startFrame)
{
    WirePacket packet = {};
    InitializeWireHeader(&packet, kind);
    packet.startFrame = startFrame;
    (void)SendPacket(packet);
    // Throttle retries even if the nonblocking socket is temporarily full.
    g_lastControlSendTick = GetTickCount();
}

void SendEvidenceControlPacket(
    PacketKind kind,
    int evidenceFrame,
    EvidenceLayer layer)
{
    WirePacket packet = {};
    InitializeWireHeader(&packet, kind);
    packet.startFrame = evidenceFrame;
    packet.count = static_cast<uint8_t>(layer);
    const bool sent = SendPacket(packet);
    if (sent && kind != PacketKind::Trigger)
    {
        // ACK/NACK have no acknowledgement of their own. One best-effort
        // duplicate makes a single lost response much less likely to force
        // the peer into the bounded retry tail; receivers are idempotent.
        (void)SendPacket(packet);
    }
    if (sent && InterlockedCompareExchange(&g_transportDraining, 0, 0) != 0)
    {
        InterlockedExchange(
            &g_workerDrainQuietUntilTick,
            static_cast<LONG>(GetTickCount() + kTransportDrainQuietMs));
    }
    if (kind == PacketKind::Trigger)
    {
        // Keep evidence retries independent from handshake/Abort throttling.
        g_lastEvidenceTriggerSendTick = GetTickCount();
    }
}

void RetryEvidenceTriggerLocked(DWORD now)
{
    const bool shouldTransmit =
        g_role == 0 || (g_role == 1 && g_evidenceTriggerLocallyRaised);
    if (!g_evidenceTriggered
        || !shouldTransmit
        || g_evidenceTriggerAcknowledged
        || g_evidenceTriggerNacked
        || !IsEvidenceLayerCode(g_evidenceTriggerLayerCode)
        || (g_lastEvidenceTriggerSendTick != 0
            && now - g_lastEvidenceTriggerSendTick < 500u))
    {
        return;
    }
    SendEvidenceControlPacket(
        PacketKind::Trigger,
        g_evidenceTriggerFrame,
        static_cast<EvidenceLayer>(g_evidenceTriggerLayerCode));
}

bool IsTransportProcessing()
{
    return InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0
        || InterlockedCompareExchange(&g_transportDraining, 0, 0) != 0;
}

bool TickDeadlineReached(DWORD now, LONG deadlineTick)
{
    return static_cast<LONG>(now - static_cast<DWORD>(deadlineTick)) >= 0;
}

bool IsEvidenceDeliveryPendingLocked()
{
    const bool shouldTransmit =
        g_role == 0 || (g_role == 1 && g_evidenceTriggerLocallyRaised);
    return g_evidenceTriggered
        && shouldTransmit
        && !g_evidenceTriggerAcknowledged
        && !g_evidenceTriggerNacked
        && IsEvidenceLayerCode(g_evidenceTriggerLayerCode);
}

// Build one ascending packet from the fixed local ring. The worker calls this
// while the game thread only appends samples; no socket operation occurs on
// the game thread or while g_lock is held.
bool BuildNextSamplePacket(
    WirePacket* packet,
    unsigned* outFirstSequence,
    LONG* outEpoch)
{
    if (packet == nullptr || outFirstSequence == nullptr || outEpoch == nullptr)
    {
        return false;
    }

    EnterCriticalSection(&g_lock);
    // captureArmed stops new game-thread samples. It must not prevent the
    // worker from transmitting samples already queued when the battle or
    // session boundary was observed.
    if (!IsTransportProcessing())
    {
        LeaveCriticalSection(&g_lock);
        return false;
    }

    const unsigned total = g_sampleCount;
    const unsigned oldest =
        total > static_cast<unsigned>(kSampleRingDepth)
            ? total - static_cast<unsigned>(kSampleRingDepth)
            : 0;
    if (g_sentSampleCount < oldest)
    {
        mod::Log(
            "DESYNC_MONITOR: outgoing sample overflow dropped=%u epoch=%ld",
            oldest - g_sentSampleCount,
            static_cast<long>(InterlockedCompareExchange(&g_battleEpoch, 0, 0)));
        g_sentSampleCount = oldest;
    }
    if (g_sentSampleCount >= total)
    {
        LeaveCriticalSection(&g_lock);
        return false;
    }

    InitializeWireHeader(packet, PacketKind::Samples);
    const unsigned pending = total - g_sentSampleCount;
    const int maxCount = pending < static_cast<unsigned>(kSamplesPerPacket)
        ? static_cast<int>(pending)
        : kSamplesPerPacket;
    const unsigned firstSequence = g_sentSampleCount;
    int count = 0;
    for (int i = 0; i < maxCount; ++i)
    {
        const unsigned sequence = firstSequence + static_cast<unsigned>(i);
        const size_t distanceFromNext =
            static_cast<size_t>(total - sequence);
        const size_t index =
            (g_localRingNext + kSampleRingDepth - distanceFromNext)
            % kSampleRingDepth;
        if (g_localRing[index].frame < 0)
        {
            LeaveCriticalSection(&g_lock);
            return false;
        }
        if (i == 0)
        {
            packet->maskHash = g_localRing[index].maskHash;
            packet->maskByteCount = g_localRing[index].maskByteCount;
        }
        else if (g_localRing[index].maskHash != packet->maskHash
            || g_localRing[index].maskByteCount != packet->maskByteCount)
        {
            // The packet header has one mask identity. Never relabel queued
            // pre-calibration samples when the gameplay mask freezes while
            // the worker has a backlog.
            break;
        }
        packet->samples[i].frame = g_localRing[index].frame;
        packet->samples[i].checksum = g_localRing[index].checksum;
        packet->samples[i].effectHash = g_localRing[index].effectHash;
        packet->samples[i].rngState = g_localRing[index].rngState;
        ++count;
    }
    packet->count = static_cast<uint8_t>(count);
    *outFirstSequence = firstSequence;
    *outEpoch = InterlockedCompareExchange(&g_battleEpoch, 0, 0);
    LeaveCriticalSection(&g_lock);
    return true;
}

void PumpOutgoingSamples()
{
    for (int packetBudget = 0; packetBudget < 16; ++packetBudget)
    {
        WirePacket packet = {};
        unsigned firstSequence = 0;
        LONG epoch = 0;
        if (!BuildNextSamplePacket(&packet, &firstSequence, &epoch))
        {
            return;
        }
        if (!SendPacket(packet))
        {
            return;
        }
        if (InterlockedCompareExchange(&g_transportDraining, 0, 0) != 0)
        {
            InterlockedExchange(
                &g_workerDrainQuietUntilTick,
                static_cast<LONG>(GetTickCount() + kTransportDrainQuietMs));
        }
        // Samples are diagnostic UDP, not the game's transport. One
        // best-effort duplicate greatly reduces the chance that the exact
        // first effect/RNG split is skipped without adding an ACK wait or any
        // work to the simulation thread. The receiver drops duplicate frames.
        (void)SendPacket(packet);
        EnterCriticalSection(&g_lock);
        if (epoch == InterlockedCompareExchange(&g_battleEpoch, 0, 0)
            && g_sentSampleCount == firstSequence)
        {
            g_sentSampleCount += packet.count;
        }
        LeaveCriticalSection(&g_lock);
    }
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

const ChecksumSample* FindSample(
    const ChecksumSample* ring,
    int frame)
{
    for (size_t i = 0; i < kSampleRingDepth; ++i)
    {
        if (ring[i].frame == frame)
        {
            return &ring[i];
        }
    }
    return nullptr;
}

bool IsPreferredEvidenceTuple(
    int candidateFrame,
    EvidenceLayer candidateLayer,
    int currentFrame,
    EvidenceLayer currentLayer)
{
    return candidateFrame < currentFrame
        || (candidateFrame == currentFrame
            && static_cast<uint8_t>(candidateLayer)
                > static_cast<uint8_t>(currentLayer));
}

bool EvidenceAuthoritySatisfiesProposal(
    int authorityFrame,
    EvidenceLayer authorityLayer,
    int proposalFrame,
    EvidenceLayer proposalLayer)
{
    return authorityFrame < proposalFrame
        || (authorityFrame == proposalFrame
            && static_cast<uint8_t>(authorityLayer)
                >= static_cast<uint8_t>(proposalLayer));
}

bool HasFrameEvidenceLocked(int frame)
{
    for (size_t i = 0; i < kRegionRingDepth; ++i)
    {
        if (g_regionRing[i].frame == frame)
        {
            return true;
        }
    }
    for (size_t i = 0; i < g_postEvidenceFrameCount; ++i)
    {
        if (g_postEvidenceFrames[i].frame == frame)
        {
            return true;
        }
    }
    return false;
}

LONG AnchorType47TriggerSequenceToFrame(int targetFrame)
{
    const LONG sessionStart = InterlockedCompareExchange(
        &g_type47SessionStartSequence, 0, 0);
    const LONG current = InterlockedCompareExchange(
        &g_type47TraceWriteSequence, 0, 0);
    const LONG fallback = current >= sessionStart ? current : sessionStart;
    if (targetFrame < 0)
    {
        return fallback;
    }

    Type47FrameAnchor& anchor = g_type47FrameAnchors[
        static_cast<uint32_t>(targetFrame) & kType47FrameAnchorMask];
    const LONG anchorBefore = InterlockedCompareExchange(&anchor.stamp, 0, 0);
    if (anchorBefore < sessionStart || anchorBefore > current)
    {
        return fallback;
    }
    const uint32_t generation = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_type47SessionGeneration, 0, 0));
    const uint32_t anchorGeneration = anchor.sessionGeneration;
    const int32_t anchorFrame = anchor.frame;
    const LONG sequence = anchor.firstSequence;
    MemoryBarrier();
    const LONG anchorAfter = InterlockedCompareExchange(&anchor.stamp, 0, 0);
    if (anchorAfter != anchorBefore
        || sequence != anchorBefore
        || anchorGeneration != generation
        || anchorFrame != targetFrame)
    {
        return fallback;
    }

    Type47TraceEvent& event = g_type47Trace[
        (static_cast<uint32_t>(sequence) - 1u) & kType47TraceMask];
    const LONG eventBefore = InterlockedCompareExchange(&event.stamp, 0, 0);
    if (eventBefore != sequence)
    {
        return fallback;
    }
    const uint32_t eventGeneration = event.sessionGeneration;
    const int32_t eventFrame = event.frame;
    MemoryBarrier();
    const LONG eventAfter = InterlockedCompareExchange(&event.stamp, 0, 0);
    return eventAfter == eventBefore
        && eventGeneration == generation
        && eventFrame == targetFrame
        ? sequence
        : fallback;
}

void SnapshotEvidenceMaskLocked()
{
    if (!g_maskFrozen)
    {
        g_evidenceMaskByteCount = 0;
        g_evidenceMaskHash = 0;
        std::memset(g_evidenceMask, 0, sizeof(g_evidenceMask));
        return;
    }
    g_evidenceMaskByteCount = g_maskByteCount;
    g_evidenceMaskHash = g_maskHash;
    std::memcpy(g_evidenceMask, g_changeMask, sizeof(g_evidenceMask));
}

void SetEvidenceTupleLocked(
    int frame,
    EvidenceLayer layer,
    bool locallyRaised,
    const char* action)
{
    g_evidenceTriggerFrame = frame;
    g_evidenceBattleEpoch =
        InterlockedCompareExchange(&g_battleEpoch, 0, 0);
    g_evidenceTriggerLayerCode = static_cast<uint8_t>(layer);
    g_evidenceTriggerLocallyRaised = locallyRaised;
    // On the joiner a peer tuple is already the host's authority. On the host
    // every selected tuple remains pending until the joiner ACKs it exactly.
    g_evidenceTriggerAcknowledged = g_role == 1 && !locallyRaised;
    g_evidenceTriggerNacked = false;
    g_lastEvidenceTriggerSendTick = 0;
    InterlockedExchange(
        &g_type47EvidenceTriggerSequence,
        AnchorType47TriggerSequenceToFrame(frame));
    // Anchor the RNG-call ring's post-trigger preservation to the same
    // event, so the draws around the divergence survive later play.
    InterlockedExchange(
        &g_rngTraceEvidenceTrigger,
        InterlockedCompareExchange(&g_rngTraceWriteSequence, 0, 0));
    std::snprintf(
        g_evidenceTriggerLayer,
        sizeof(g_evidenceTriggerLayer),
        "%s",
        EvidenceLayerName(layer));
    mod::Log(
        "DESYNC_MONITOR: forensic onset %s frame=%d layer=%s source=%s "
        "(disk I/O deferred; post-window capture continues)",
        action != nullptr ? action : "selected",
        frame,
        g_evidenceTriggerLayer,
        locallyRaised ? "local" : "peer");
}

void TriggerEvidenceLocked(
    int frame,
    EvidenceLayer layer,
    bool locallyRaised)
{
    if (g_evidenceTriggered)
    {
        return;
    }
    g_evidenceTriggered = true;
    g_evidenceRegionNext = g_regionRingNext;
    g_evidenceCompareNext = g_compareLogNext;
    SnapshotEvidenceMaskLocked();
    SetEvidenceTupleLocked(frame, layer, locallyRaised, "frozen");
}

void ReviseEvidenceTupleLocked(
    int frame,
    EvidenceLayer layer,
    bool locallyRaised)
{
    if (!g_evidenceTriggered)
    {
        TriggerEvidenceLocked(frame, layer, locallyRaised);
        return;
    }
    SetEvidenceTupleLocked(frame, layer, locallyRaised, "canonicalized");
}

// Compare a remote sample against the local ring. Caller holds the lock.
// Returns true when a contiguous checksum-difference window is first
// captured. The selected gameplay checksum drives that window; the effect
// projection and RNG scalar are only attribution evidence and never mutate
// or terminate the session.
bool CompareRemoteSampleLocked(
    int frame,
    uint32_t remoteSum,
    uint32_t remoteEffectHash,
    int32_t remoteRngState,
    bool gameplayComparable)
{
    const ChecksumSample* local = FindSample(g_localRing, frame);
    if (local == nullptr)
    {
        return false;
    }

    // UDP packets can be duplicated, reordered, or skipped. Only a strictly
    // ascending exact-frame sequence can extend a diagnostic run.
    if (frame <= g_lastComparedFrame)
    {
        return false;
    }
    if (g_lastComparedFrame >= 0 && frame != g_lastComparedFrame + 1)
    {
        if (g_mismatchStreak != 0 || g_effectLayerRun != 0 || g_rngLayerRun != 0)
        {
            mod::Log(
                "DESYNC_MONITOR: comparison gap %d -> %d; all contiguous runs reset",
                g_lastComparedFrame,
                frame);
        }
        g_mismatchStreak = 0;
        g_firstMismatchFrame = -1;
        g_lastMismatchFrame = -1;
        if (!g_gameplayMismatchRunFrozen)
        {
            g_gameplayMismatchRunCount = 0;
        }
        g_effectLayerRun = 0;
        g_effectLayerFirstFrame = -1;
        g_rngLayerRun = 0;
        g_rngLayerFirstFrame = -1;
    }
    g_lastComparedFrame = frame;

    const bool match =
        !gameplayComparable || (local->checksum == remoteSum);

    CompareEntry scratch = {};
    CompareEntry* entryPtr = &scratch;
    if (!g_evidenceTriggered)
    {
        entryPtr = &g_compareLog[g_compareLogNext];
        g_compareLogNext = (g_compareLogNext + 1) % kCompareLogDepth;
    }
    else if (g_postEvidenceCompareCount < kPostEvidenceDepth)
    {
        entryPtr = &g_postEvidenceCompare[g_postEvidenceCompareCount++];
    }
    CompareEntry& entry = *entryPtr;
    entry.frame = frame;
    entry.localSum = local->checksum;
    entry.remoteSum = remoteSum;
    entry.match = match;
    entry.localEffect = local->effectHash;
    entry.remoteEffect = remoteEffectHash;
    entry.localRng = local->rngState;
    entry.remoteRng = remoteRngState;

    // Layer attribution.  Only compare when both sides produced data (a 0
    // effect hash or -1 RNG state means "unavailable", not "different").
    const bool effectComparable =
        local->effectHash != 0 && remoteEffectHash != 0;
    const bool effectMatch =
        !effectComparable || local->effectHash == remoteEffectHash;
    const bool rngComparable =
        local->rngState != -1 && remoteRngState != -1;
    const bool rngMatch =
        !rngComparable || local->rngState == remoteRngState;

    if (effectComparable)
    {
        if (!effectMatch && g_effectLayerRun == 0)
        {
            g_effectLayerFirstFrame = frame;
            mod::Log(
                "DESYNC_MONITOR_LAYER: effect projection differs at frame %d "
                "(gameplay=%s rng=%s) localEff=0x%08lX remoteEff=0x%08lX",
                frame,
                gameplayComparable ? (match ? "match" : "MISMATCH") : "not-comparable",
                rngMatch ? "match" : "MISMATCH",
                static_cast<unsigned long>(local->effectHash),
                static_cast<unsigned long>(remoteEffectHash));
        }
        if (!effectMatch)
        {
            ++g_effectLayerRun;
        }
        else if (g_effectLayerRun != 0)
        {
            mod::Log(
                "DESYNC_MONITOR_LAYER: effect projection matches again at frame %d "
                "(run of %d from frame %d)",
                frame,
                g_effectLayerRun,
                g_effectLayerFirstFrame);
            g_effectLayerRun = 0;
            g_effectLayerFirstFrame = -1;
        }
    }
    if (rngComparable)
    {
        if (!rngMatch && g_rngLayerRun == 0)
        {
            g_rngLayerFirstFrame = frame;
            mod::Log(
                "DESYNC_MONITOR_LAYER: RNG scalar differs at frame %d "
                "(gameplay=%s effects=%s) localRng=%ld remoteRng=%ld",
                frame,
                gameplayComparable ? (match ? "match" : "MISMATCH") : "not-comparable",
                effectMatch ? "match" : "MISMATCH",
                static_cast<long>(local->rngState),
                static_cast<long>(remoteRngState));
        }
        if (!rngMatch)
        {
            ++g_rngLayerRun;
        }
        else if (g_rngLayerRun != 0)
        {
            mod::Log(
                "DESYNC_MONITOR_LAYER: RNG scalar matches again at frame %d "
                "(run of %d from frame %d)",
                frame,
                g_rngLayerRun,
                g_rngLayerFirstFrame);
            g_rngLayerRun = 0;
            g_rngLayerFirstFrame = -1;
        }
    }


    const bool effectDiff = effectComparable && !effectMatch;
    const bool rngDiff = rngComparable && !rngMatch;
    const bool gameplayDiff = gameplayComparable && !match;
    if (effectDiff || rngDiff || gameplayDiff)
    {
        const EvidenceLayer layer =
            (effectDiff && rngDiff) ? EvidenceLayer::EffectAndRng
            : effectDiff ? EvidenceLayer::EffectProjection
            : rngDiff ? EvidenceLayer::RngScalar
            : EvidenceLayer::SelectedGameplay;
        TriggerEvidenceLocked(frame, layer, true);
    }

    if (!gameplayComparable)
    {
        return false;
    }

    if (match)
    {
        if (g_mismatchStreak != 0 && frame >= g_firstMismatchFrame)
        {
            mod::Log(
                "DESYNC_MONITOR: checksum-difference run ended at matching frame %d "
                "(run was %d from frame %d; hidden state may still differ)",
                frame,
                g_mismatchStreak,
                g_firstMismatchFrame);
            g_mismatchStreak = 0;
            g_firstMismatchFrame = -1;
            g_lastMismatchFrame = -1;
            if (!g_gameplayMismatchRunFrozen)
            {
                g_gameplayMismatchRunCount = 0;
            }
        }
        return false;
    }

    if (g_mismatchStreak == 0)
    {
        g_firstMismatchFrame = frame;
        g_lastMismatchFrame = frame;
        g_mismatchStreak = 1;
        if (!g_gameplayMismatchRunFrozen)
        {
            g_gameplayMismatchRunCount = 0;
            g_gameplayMismatchRun[g_gameplayMismatchRunCount++] = entry;
        }
        mod::Log(
            "DESYNC_MONITOR: checksum mismatch at frame %d local=0x%08lX remote=0x%08lX",
            frame,
            static_cast<unsigned long>(local->checksum),
            static_cast<unsigned long>(remoteSum));
        return false;
    }

    g_lastMismatchFrame = frame;
    ++g_mismatchStreak;
    if (!g_gameplayMismatchRunFrozen
        && g_gameplayMismatchRunCount < kGameplayRunDepth)
    {
        g_gameplayMismatchRun[g_gameplayMismatchRunCount++] = entry;
    }

    if (g_mismatchStreak >= kConfirmMismatchFrames
        && InterlockedExchange(&g_desyncConfirmed, 1) == 0)
    {
        g_confirmedFrame = g_gameplayMismatchRunCount > 0
            ? g_gameplayMismatchRun[0].frame
            : g_firstMismatchFrame;
        g_gameplayMismatchRunFrozen = true;
        return true;
    }
    return false;
}

int FindLowestCommonFrameAfterLocked(int frameExclusive)
{
    int lowest = -1;
    for (size_t i = 0; i < kSampleRingDepth; ++i)
    {
        const int frame = g_localRing[i].frame;
        if (frame <= frameExclusive || (lowest >= 0 && frame >= lowest))
        {
            continue;
        }
        if (FindSample(g_remoteRing, frame) != nullptr)
        {
            lowest = frame;
        }
    }
    return lowest;
}

bool DrainComparableSamplesLocked(bool flushFinal = false)
{
    bool captured = false;
    const int frontier =
        g_highestLocalFrame < g_highestRemoteFrame
            ? g_highestLocalFrame
            : g_highestRemoteFrame;
    if (frontier < 0)
    {
        return false;
    }

    if (g_nextCompareFrame < 0)
    {
        const int first = FindLowestCommonFrameAfterLocked(-1);
        if (first < 0
            || (!flushFinal && frontier - first < kReorderWaitFrames))
        {
            return false;
        }
        g_nextCompareFrame = first;
    }

    const int compareBudget = flushFinal
        ? static_cast<int>(kSampleRingDepth)
        : 64;
    for (int budget = 0; budget < compareBudget; ++budget)
    {
        const ChecksumSample* local = FindSample(g_localRing, g_nextCompareFrame);
        const ChecksumSample* remote = FindSample(g_remoteRing, g_nextCompareFrame);
        if (local == nullptr || remote == nullptr)
        {
            if (!flushFinal
                && frontier - g_nextCompareFrame < kReorderWaitFrames)
            {
                break;
            }
            const int nextAvailable =
                FindLowestCommonFrameAfterLocked(g_nextCompareFrame);
            if (nextAvailable < 0 || nextAvailable > frontier)
            {
                break;
            }
            g_nextCompareFrame = nextAvailable;
            local = FindSample(g_localRing, g_nextCompareFrame);
            remote = FindSample(g_remoteRing, g_nextCompareFrame);
            if (local == nullptr || remote == nullptr)
            {
                break;
            }
        }

        const bool gameplayComparable =
            local->maskHash != 0
            && remote->maskHash != 0
            && remote->maskHash == local->maskHash
            && remote->maskByteCount == local->maskByteCount;
        const bool masksDiffer =
            local->maskHash != 0
            && remote->maskHash != 0
            && (remote->maskHash != local->maskHash
                || remote->maskByteCount != local->maskByteCount);
        if (masksDiffer && !g_maskMismatchLogged)
        {
            g_maskMismatchLogged = true;
            mod::Log(
                "DESYNC_MONITOR: selected-gameplay masks differ; gameplay-window "
                "trigger disabled localMask=0x%08lX/%u remoteMask=0x%08lX/%u "
                "(effect/RNG attribution continues)",
                static_cast<unsigned long>(local->maskHash),
                static_cast<unsigned>(local->maskByteCount),
                static_cast<unsigned long>(remote->maskHash),
                static_cast<unsigned>(remote->maskByteCount));
        }
        captured |= CompareRemoteSampleLocked(
            g_nextCompareFrame,
            remote->checksum,
            remote->effectHash,
            remote->rngState,
            gameplayComparable);
        ++g_nextCompareFrame;
    }
    return captured;
}

// ---------------------------------------------------------------------------
// Dump
// ---------------------------------------------------------------------------

bool g_dumpWriteOk = true;

void AppendText(HANDLE file, const char* text)
{
    DWORD written = 0;
    const DWORD expected = static_cast<DWORD>(std::strlen(text));
    if (!WriteFile(file, text, expected, &written, nullptr)
        || written != expected)
    {
        g_dumpWriteOk = false;
    }
}

void AppendFormat(HANDLE file, const char* format, ...)
{
    char buffer[512] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    AppendText(file, buffer);
}

void AppendHexRegion(
    HANDLE file,
    const char* label,
    uint32_t srcAddr,
    const uint8_t* bytes,
    size_t size,
    bool valid)
{
    AppendFormat(
        file,
        "  %s src=0x%08lX size=%u valid=%d\r\n",
        label,
        static_cast<unsigned long>(srcAddr),
        static_cast<unsigned>(size),
        valid ? 1 : 0);
    if (!valid)
    {
        return;
    }
    char line[128];
    for (size_t offset = 0; offset < size; offset += 16)
    {
        int pos = std::snprintf(line, sizeof(line), "    +%04X ", static_cast<unsigned>(offset));
        for (size_t i = 0; i < 16 && offset + i < size; ++i)
        {
            pos += std::snprintf(line + pos, sizeof(line) - pos, "%02X ", bytes[offset + i]);
        }
        pos += std::snprintf(line + pos, sizeof(line) - pos, "\r\n");
        AppendText(file, line);
    }
}

void CopyLogInto(const std::string& dumpDir, const std::string& sourcePath, const char* destName)
{
    const std::string dest = dumpDir + "\\" + destName;
    const BOOL ok = CopyFileA(sourcePath.c_str(), dest.c_str(), FALSE);
    mod::Log(
        "DESYNC_MONITOR: copy '%s' -> '%s' result=%d err=%lu",
        sourcePath.c_str(),
        destName,
        ok ? 1 : 0,
        ok ? 0ul : static_cast<unsigned long>(GetLastError()));
}

const char* Type47PhaseName(uint8_t phase)
{
    switch (phase)
    {
    case kTracePassBegin: return "pass_begin";
    case kTraceBeforeUpdate: return "before_update";
    case kTraceAfterUpdate: return "after_update";
    case kTraceCreated: return "created";
    case kTraceCleared: return "cleared";
    case kTracePassEnd: return "pass_end";
    case kTraceSnapshotSave: return "snapshot_save";
    case kTraceSnapshotLoad: return "snapshot_load";
    case kTraceCharContext: return "char_context";
    default: return "unknown";
    }
}

int CountParkMillerAdvances(int32_t before, int32_t after, int maxAdvances)
{
    if (before <= 0 || after <= 0)
    {
        return -1;
    }
    int64_t state = before;
    if (before == after)
    {
        return 0;
    }
    for (int count = 1; count <= maxAdvances; ++count)
    {
        state = (state * 48271ll) % 2147483647ll;
        if (state == after)
        {
            return count;
        }
    }
    return -1;
}

bool WriteRngTraceCsv(const std::string& dumpDir)
{
    if (!netplay::mod_settings::IsRngCallTraceEnabled())
    {
        return true;
    }
    const std::string path = dumpDir + "\\rng_trace.csv";
    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        mod::Log("RNG_TRACE: could not create '%s' err=%lu",
                 path.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }
    AppendText(file,
        "sequence,session_generation,pass_serial,frame,commit,resim,"
        "effect_slot,effect_phase,return_addr,state_before,state_after,"
        "internal_advances,thread_id,eff_anim_frame,eff_ypos_bits,"
        "eff_yvel_bits\r\n");

    const LONG end =
        InterlockedCompareExchange(&g_rngTraceWriteSequence, 0, 0);
    const uint32_t sessionGeneration = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_type47SessionGeneration, 0, 0));
    const LONG retained = static_cast<LONG>(kRngTraceDepth);
    const LONG begin = end > retained ? end - retained + 1 : 1;
    unsigned written = 0;
    unsigned unstable = 0;
    for (LONG sequence = begin; sequence <= end; ++sequence)
    {
        RngCallEvent& sourceRef =
            g_rngTrace[(static_cast<uint32_t>(sequence) - 1u) & kRngTraceMask];
        const LONG before = InterlockedCompareExchange(&sourceRef.stamp, 0, 0);
        if (before != sequence)
        {
            ++unstable;
            continue;
        }
        RngCallEvent copy;
        std::memcpy(&copy, &sourceRef, sizeof(copy));
        MemoryBarrier();
        if (InterlockedCompareExchange(&sourceRef.stamp, 0, 0) != before)
        {
            ++unstable;
            continue;
        }
        if (copy.sessionGeneration != sessionGeneration)
        {
            continue;
        }
        AppendFormat(
            file,
            "%ld,%lu,%lu,%ld,%ld,%u,%u,%u,0x%08lX,%ld,%ld,%d,%lu,"
            "%u,%016llX,%016llX\r\n",
            static_cast<long>(sequence),
            static_cast<unsigned long>(copy.sessionGeneration),
            static_cast<unsigned long>(copy.passSerial),
            static_cast<long>(copy.frame),
            static_cast<long>(copy.commitFrame),
            static_cast<unsigned>(copy.resim),
            static_cast<unsigned>(copy.effectSlot),
            static_cast<unsigned>(copy.phaseContext),
            static_cast<unsigned long>(copy.returnAddr),
            static_cast<long>(copy.stateBefore),
            static_cast<long>(copy.stateAfter),
            copy.internalAdvances,
            static_cast<unsigned long>(copy.threadId),
            static_cast<unsigned>(copy.effectAnimFrame),
            static_cast<unsigned long long>(copy.effectYPosBits),
            static_cast<unsigned long long>(copy.effectYVelBits));
        ++written;
    }
    CloseHandle(file);
    mod::Log(
        "RNG_TRACE: wrote '%s' records=%u unstable=%u range=%ld..%ld",
        path.c_str(), written, unstable,
        static_cast<long>(begin), static_cast<long>(end));
    return true;
}

bool WriteType47TraceCsv(const std::string& dumpDir)
{
    const std::string path = dumpDir + "\\type47_trace.csv";
    HANDLE file = CreateFileA(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        mod::Log(
            "TYPE47_TRACE: failed to create '%s' err=%lu",
            path.c_str(), static_cast<unsigned long>(GetLastError()));
        return false;
    }

    g_dumpWriteOk = true;
    AppendText(file,
        "sequence,relative_to_trigger,session_generation,pass_serial,frame,commit,"
        "rng,phase,slot,active,alloc,proc,behavior,status,parameter,anim_frame,"
        "anim_tick,x_bits,y_bits,vx_bits,vy_bits,bounce_after_gravity,"
        "expected_source_rand_calls,rng_internal_advances,"
        "p1_move,p1_anim_frame,p1_anim_tick,p1_freeze,p1_contact,"
        "p2_move,p2_anim_frame,p2_anim_tick,p2_freeze,p2_contact,"
        "p1_sx_bits,p1_sy_bits,p2_sx_bits,p2_sy_bits,"
        "fpu_x87cw,fpu_mxcsr,p1_facing,p2_facing,"
        "p1_hitstun,p1_freeze2,p1_movetimer,p2_hitstun,p2_freeze2,p2_movetimer,"
        "coll_gate40,coll_gate44,coll_gate48,round_no,ready_mask,"
        "wordstream_mode,wordstream_ctr,"
        "p1_vx_bits,p1_vy_bits,p1_kbx_bits,p1_kby_bits,p1_momx_bits,"
        "p2_vx_bits,p2_vy_bits,p2_kbx_bits,p2_kby_bits,p2_momx_bits,"
        "push_flags,p1_anchor,p2_anchor,p1_boxfnv,p2_boxfnv,"
        "p1_reactflag,p2_reactflag,p1_throwctr,p2_throwctr,p1_meter,p2_meter,"
        "eff_dir,active_count,sim_ptrs,save_ptrs,load_from,resim\r\n");

    const LONG currentEnd =
        InterlockedCompareExchange(&g_type47TraceWriteSequence, 0, 0);
    const LONG sealedEnd =
        InterlockedCompareExchange(&g_type47SessionEndSequence, 0, 0);
    const LONG rawEnd = sealedEnd > 0 && sealedEnd < currentEnd
        ? sealedEnd
        : currentEnd;
    const LONG trigger =
        InterlockedCompareExchange(&g_type47EvidenceTriggerSequence, 0, 0);
    LONG end = rawEnd;
    if (trigger > 0)
    {
        const int64_t preservedEnd =
            static_cast<int64_t>(trigger)
            + static_cast<int64_t>(kType47PostTriggerEvents);
        if (static_cast<int64_t>(end) > preservedEnd)
        {
            end = static_cast<LONG>(preservedEnd);
            mod::Log(
                "TYPE47_TRACE: detailed storage preserved through sequence=%ld; "
                "%ld later callbacks used the bounded discard ring",
                static_cast<long>(end),
                static_cast<long>(rawEnd - end));
        }
    }
    const LONG sessionBegin =
        InterlockedCompareExchange(&g_type47SessionStartSequence, 0, 0);
    const uint32_t sessionGeneration = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_type47SessionGeneration, 0, 0));
    const LONG retained = static_cast<LONG>(kType47TraceDepth);
    const LONG retainedBegin = end > retained ? end - retained + 1 : 1;
    const LONG begin = sessionBegin > retainedBegin ? sessionBegin : retainedBegin;
    unsigned writtenRecords = 0;
    unsigned unstableRecords = 0;
    unsigned foreignGenerationRecords = 0;
    int32_t beforeRng[kEffectRingSlots] = {};
    uint32_t beforePass[kEffectRingSlots] = {};
    bool beforeValid[kEffectRingSlots] = {};
    for (LONG sequence = begin; sequence <= end; ++sequence)
    {
        Type47TraceEvent& source =
            g_type47Trace[(static_cast<uint32_t>(sequence) - 1u) & kType47TraceMask];
        const LONG before = InterlockedCompareExchange(&source.stamp, 0, 0);
        if (before != sequence)
        {
            ++unstableRecords;
            continue;
        }
        Type47TraceEvent copy;
        std::memcpy(&copy, &source, sizeof(copy));
        MemoryBarrier();
        const LONG after = InterlockedCompareExchange(&source.stamp, 0, 0);
        if (after != before)
        {
            ++unstableRecords;
            continue;
        }
        if (copy.sessionGeneration != sessionGeneration)
        {
            ++foreignGenerationRecords;
            continue;
        }

        int bounceAfterGravity = -1;
        int expectedSourceRandCalls = -1;
        int rngInternalAdvances = -1;
        if (copy.slot < kEffectRingSlots)
        {
            if (copy.phase == kTraceBeforeUpdate)
            {
                double y = 0.0;
                double vy = 0.0;
                std::memcpy(&y, &copy.yBits, sizeof(y));
                std::memcpy(&vy, &copy.vyBits, sizeof(vy));
                const bool firstUpdate =
                    copy.animFrame == 0 && copy.animTick == 0;
                if (!firstUpdate)
                {
                    // Type 47 applies +0.2 gravity before testing y+vy>=0.
                    bounceAfterGravity = (y + vy + 0.2 >= 0.0) ? 1 : 0;
                    expectedSourceRandCalls = 1 + bounceAfterGravity;
                }
                else
                {
                    // The first update consumes two rand() calls to replace
                    // vx/vy before gravity and the bounce test. Its predicate
                    // cannot be reconstructed from the pre-update vy field;
                    // -1 explicitly means unavailable, not "no bounce".
                    bounceAfterGravity = -1;
                    expectedSourceRandCalls = -1;
                }
                beforeRng[copy.slot] = copy.rngState;
                beforePass[copy.slot] = copy.passSerial;
                beforeValid[copy.slot] = true;
            }
            else if (copy.phase == kTraceAfterUpdate
                && beforeValid[copy.slot]
                && beforePass[copy.slot] == copy.passSerial)
            {
                // This is the number of minstd_rand recurrence advances, not
                // source-level rand() calls: Revival's adapter may reject and
                // consume multiple internal values per logical call.
                rngInternalAdvances = CountParkMillerAdvances(
                    beforeRng[copy.slot], copy.rngState, 64);
                beforeValid[copy.slot] = false;
            }
        }
        const int relativeToTrigger =
            trigger <= 0 ? 0 : (sequence < trigger ? -1 : (sequence > trigger ? 1 : 0));
        AppendFormat(
            file,
            "%ld,%d,%lu,%lu,%ld,%ld,%ld,%s,%u,%u,%u,%u,%u,%lu,%lu,"
            "%u,%u,%016llX,%016llX,%016llX,%016llX,%d,%d,%d,"
            "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
            "%016llX,%016llX,%016llX,%016llX,"
            "%u,%lu,%ld,%ld,"
            "%u,%u,%u,%u,%u,%u,"
            "%u,%u,%lu,%u,%lu,"
            "%u,%lu,"
            "%016llX,%016llX,%016llX,%016llX,%016llX,"
            "%016llX,%016llX,%016llX,%016llX,%016llX,"
            "%lu,%lu,%lu,%lu,%lu,"
            "%ld,%ld,%ld,%ld,%u,%u,"
            "%d,%u,%016llX,%016llX,%lu,%u\r\n",
            static_cast<long>(sequence),
            relativeToTrigger,
            static_cast<unsigned long>(copy.sessionGeneration),
            static_cast<unsigned long>(copy.passSerial),
            static_cast<long>(copy.frame),
            static_cast<long>(copy.commitFrame),
            static_cast<long>(copy.rngState),
            Type47PhaseName(copy.phase),
            static_cast<unsigned>(copy.slot),
            static_cast<unsigned>(copy.active),
            static_cast<unsigned>(copy.allocCursor),
            static_cast<unsigned>(copy.procCursor),
            static_cast<unsigned>(copy.behavior),
            static_cast<unsigned long>(copy.status),
            static_cast<unsigned long>(copy.parameter),
            static_cast<unsigned>(copy.animFrame),
            static_cast<unsigned>(copy.animTick),
            static_cast<unsigned long long>(copy.xBits),
            static_cast<unsigned long long>(copy.yBits),
            static_cast<unsigned long long>(copy.vxBits),
            static_cast<unsigned long long>(copy.vyBits),
            bounceAfterGravity,
            expectedSourceRandCalls,
            rngInternalAdvances,
            static_cast<unsigned>(copy.p1Move),
            static_cast<unsigned>(copy.p1AnimFrame),
            static_cast<unsigned>(copy.p1AnimTick),
            static_cast<unsigned>(copy.p1Freeze),
            static_cast<unsigned>(copy.p1Contact),
            static_cast<unsigned>(copy.p2Move),
            static_cast<unsigned>(copy.p2AnimFrame),
            static_cast<unsigned>(copy.p2AnimTick),
            static_cast<unsigned>(copy.p2Freeze),
            static_cast<unsigned>(copy.p2Contact),
            static_cast<unsigned long long>(copy.p1ScreenXBits),
            static_cast<unsigned long long>(copy.p1ScreenYBits),
            static_cast<unsigned long long>(copy.p2ScreenXBits),
            static_cast<unsigned long long>(copy.p2ScreenYBits),
            static_cast<unsigned>(copy.fpuX87Cw),
            static_cast<unsigned long>(copy.fpuMxcsr),
            static_cast<long>(copy.p1Facing),
            static_cast<long>(copy.p2Facing),
            static_cast<unsigned>(copy.p1Hitstun),
            static_cast<unsigned>(copy.p1Freeze2),
            static_cast<unsigned>(copy.p1MoveTimer),
            static_cast<unsigned>(copy.p2Hitstun),
            static_cast<unsigned>(copy.p2Freeze2),
            static_cast<unsigned>(copy.p2MoveTimer),
            static_cast<unsigned>(copy.collGate40),
            static_cast<unsigned>(copy.collGate44),
            static_cast<unsigned long>(copy.collGate48),
            static_cast<unsigned>(copy.roundNo),
            static_cast<unsigned long>(copy.readyMask),
            static_cast<unsigned>(copy.wordstreamMode),
            static_cast<unsigned long>(copy.wordstreamCtr),
            static_cast<unsigned long long>(copy.p1VxBits),
            static_cast<unsigned long long>(copy.p1VyBits),
            static_cast<unsigned long long>(copy.p1KbxBits),
            static_cast<unsigned long long>(copy.p1KbyBits),
            static_cast<unsigned long long>(copy.p1MomxBits),
            static_cast<unsigned long long>(copy.p2VxBits),
            static_cast<unsigned long long>(copy.p2VyBits),
            static_cast<unsigned long long>(copy.p2KbxBits),
            static_cast<unsigned long long>(copy.p2KbyBits),
            static_cast<unsigned long long>(copy.p2MomxBits),
            static_cast<unsigned long>(copy.pushFlags),
            static_cast<unsigned long>(copy.p1Anchor),
            static_cast<unsigned long>(copy.p2Anchor),
            static_cast<unsigned long>(copy.p1BoxFnv),
            static_cast<unsigned long>(copy.p2BoxFnv),
            static_cast<long>(copy.p1ReactFlag),
            static_cast<long>(copy.p2ReactFlag),
            static_cast<long>(copy.p1ThrowCtr),
            static_cast<long>(copy.p2ThrowCtr),
            static_cast<unsigned>(copy.p1Meter),
            static_cast<unsigned>(copy.p2Meter),
            static_cast<int>(copy.effDir),
            static_cast<unsigned>(copy.activeCount),
            static_cast<unsigned long long>(copy.simPtrs),
            static_cast<unsigned long long>(copy.savePtrs),
            static_cast<unsigned long>(copy.loadFrom),
            static_cast<unsigned>(copy.resim));
        ++writtenRecords;
    }
    CloseHandle(file);
    const bool ok = g_dumpWriteOk;
    mod::Log(
        "TYPE47_TRACE: wrote '%s' records=%u unstable_or_overwritten=%u "
        "foreign_generation=%u generation=%lu sequenceRange=%ld..%ld "
        "triggerSequence=%ld ok=%d",
        path.c_str(), writtenRecords, unstableRecords,
        foreignGenerationRecords,
        static_cast<unsigned long>(sessionGeneration),
        static_cast<long>(begin), static_cast<long>(end),
        static_cast<long>(trigger), ok ? 1 : 0);
    return ok;
}

void AppendCompareEntry(HANDLE report, const CompareEntry& entry)
{
    if (entry.frame < 0)
    {
        return;
    }
    AppendFormat(
        report,
        "  frame=%d local=0x%08lX remote=0x%08lX eff=0x%08lX/0x%08lX%s "
        "rng=%ld/%ld%s %s\r\n",
        entry.frame,
        static_cast<unsigned long>(entry.localSum),
        static_cast<unsigned long>(entry.remoteSum),
        static_cast<unsigned long>(entry.localEffect),
        static_cast<unsigned long>(entry.remoteEffect),
        (entry.localEffect != 0 && entry.remoteEffect != 0
         && entry.localEffect != entry.remoteEffect) ? "(DIFF)" : "",
        static_cast<long>(entry.localRng),
        static_cast<long>(entry.remoteRng),
        (entry.localRng != -1 && entry.remoteRng != -1
         && entry.localRng != entry.remoteRng) ? "(DIFF)" : "",
        entry.match ? "selected-match" : "selected-DIFF");
}

void AppendFrameRecord(HANDLE frames, const FrameRecord& record, int triggerFrame)
{
    char p1Text[64] = {};
    char p2Text[64] = {};
    FormatInputPair(p1Text, sizeof(p1Text), record.p1Input);
    FormatInputPair(p2Text, sizeof(p2Text), record.p2Input);
    AppendFormat(
        frames,
        "==== frame %d checksum=0x%08lX inputs p1=0x%04X (%s) "
        "p2=0x%04X (%s) %s\r\n",
        record.frame,
        static_cast<unsigned long>(record.checksum),
        static_cast<unsigned>(record.p1Input),
        p1Text,
        static_cast<unsigned>(record.p2Input),
        p2Text,
        record.frame == triggerFrame ? "[FORENSIC-TRIGGER]" : "");
    AppendFormat(
        frames,
        "     commit=%d syncFeed=%d localLen=%d remoteLen=%d ping=%d "
        "effectHash=0x%08lX rngState=%ld effectCursors=%u/%u "
        "effectRecords=%u overflow=%u\r\n",
        record.commitFrame,
        record.syncFeed,
        record.localLen,
        record.remoteLen,
        record.pingMs,
        static_cast<unsigned long>(record.effectHash),
        static_cast<long>(record.rngState),
        static_cast<unsigned>(record.effectAllocCursor),
        static_cast<unsigned>(record.effectProcCursor),
        static_cast<unsigned>(record.effectTraceCount),
        static_cast<unsigned>(record.effectTraceOverflow));
    for (uint16_t i = 0; i < record.effectTraceCount; ++i)
    {
        const EffectProjectionRecord& effect = record.effectTrace[i];
        uint64_t xBits = 0;
        uint64_t yBits = 0;
        uint64_t vxBits = 0;
        uint64_t vyBits = 0;
        std::memcpy(&xBits, &effect.x, sizeof(xBits));
        std::memcpy(&yBits, &effect.y, sizeof(yBits));
        std::memcpy(&vxBits, &effect.vx, sizeof(vxBits));
        std::memcpy(&vyBits, &effect.vy, sizeof(vyBits));
        AppendFormat(
            frames,
            "     effect slot=%u active=0x%08lX status=0x%08lX behavior=%u "
            "anim=%u/%u parameter=0x%08lX pos=%.9g,%.9g vel=%.9g,%.9g "
            "bits=x:%08lX%08lX y:%08lX%08lX vx:%08lX%08lX vy:%08lX%08lX\r\n",
            static_cast<unsigned>(effect.slot),
            static_cast<unsigned long>(effect.activeFlag),
            static_cast<unsigned long>(effect.status),
            static_cast<unsigned>(effect.behaviorId),
            static_cast<unsigned>(effect.animFrame),
            static_cast<unsigned>(effect.animTick),
            static_cast<unsigned long>(effect.parameter),
            effect.x,
            effect.y,
            effect.vx,
            effect.vy,
            static_cast<unsigned long>(xBits >> 32),
            static_cast<unsigned long>(xBits & 0xFFFFFFFFu),
            static_cast<unsigned long>(yBits >> 32),
            static_cast<unsigned long>(yBits & 0xFFFFFFFFu),
            static_cast<unsigned long>(vxBits >> 32),
            static_cast<unsigned long>(vxBits & 0xFFFFFFFFu),
            static_cast<unsigned long>(vyBits >> 32),
            static_cast<unsigned long>(vyBits & 0xFFFFFFFFu));
    }
    const uint8_t* cursor = record.bytes;
    AppendHexRegion(frames, "gameSystem", record.srcGameSys, cursor,
        kGameSysSliceSize, (record.validMask & 1) != 0);
    cursor += kGameSysSliceSize;
    AppendHexRegion(frames, "battleScreen", record.srcBattle, cursor,
        kBattleSliceSize, (record.validMask & 2) != 0);
    cursor += kBattleSliceSize;
    AppendHexRegion(frames, "characterP1", record.srcCharP1, cursor,
        kCharSliceSize, (record.validMask & 4) != 0);
    cursor += kCharSliceSize;
    AppendHexRegion(frames, "characterP2", record.srcCharP2, cursor,
        kCharSliceSize, (record.validMask & 8) != 0);
}

// Called only after the worker has stopped at session teardown. Snapshots the
// rings and writes the dump folder without perturbing an active rollback loop.
bool WriteDesyncDump()
{
    // Snapshot state under the lock; file I/O happens after release.
    static FrameRecord regionCopy[kRegionRingDepth];
    static FrameRecord postRegionCopy[kPostEvidenceDepth];
    static CompareEntry compareCopy[kCompareLogDepth];
    static CompareEntry postCompareCopy[kPostEvidenceDepth];
    static CompareEntry gameplayRunCopy[kGameplayRunDepth];
    int confirmedFrame = -1;
    int triggerFrame = -1;
    LONG triggerEpoch = 0;
    char triggerLayer[24] = {};
    unsigned sampleCount = 0;

    EnterCriticalSection(&g_lock);
    std::memcpy(regionCopy, g_regionRing, sizeof(g_regionRing));
    std::memcpy(postRegionCopy, g_postEvidenceFrames, sizeof(g_postEvidenceFrames));
    std::memcpy(compareCopy, g_compareLog, sizeof(g_compareLog));
    std::memcpy(postCompareCopy, g_postEvidenceCompare, sizeof(g_postEvidenceCompare));
    std::memcpy(
        gameplayRunCopy,
        g_gameplayMismatchRun,
        sizeof(g_gameplayMismatchRun));
    confirmedFrame = g_confirmedFrame;
    triggerFrame = g_evidenceTriggerFrame;
    triggerEpoch = g_evidenceBattleEpoch;
    std::memcpy(triggerLayer, g_evidenceTriggerLayer, sizeof(triggerLayer));
    const size_t regionNext = g_evidenceRegionNext;
    const size_t compareNext = g_evidenceCompareNext;
    const size_t postRegionCount = g_postEvidenceFrameCount;
    const size_t postCompareCount = g_postEvidenceCompareCount;
    const size_t gameplayRunCount = g_gameplayMismatchRunCount;
    sampleCount = g_sampleCount;
    const unsigned maskBytes = g_evidenceMaskByteCount;
    const uint32_t maskHash = g_evidenceMaskHash;
    uint8_t maskCopy[kRecordRegionBytes] = {};
    std::memcpy(maskCopy, g_evidenceMask, sizeof(maskCopy));
    const uint32_t sessionNonce = g_sessionNonce;
    const int captureStartFrame = static_cast<int>(
        InterlockedCompareExchange(&g_captureStartFrame, 0, 0));
    const int role = g_role;
    char peer[64];
    char nick[64];
    std::memcpy(peer, g_peerAddress, sizeof(peer));
    std::memcpy(nick, g_nickname, sizeof(nick));
    const uint16_t hostPort = g_hostPort;
    LeaveCriticalSection(&g_lock);

    const std::string dllDir = ModuleDirectory();
    const std::string gameDir = netplay::bridge::takeover::GameDirectory();

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char stamp[64] = {};
    std::snprintf(
        stamp,
        sizeof(stamp),
        "%04u%02u%02u_%02u%02u%02u_%03u_p%lu_r%d_n%08lX",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds,
        static_cast<unsigned long>(GetCurrentProcessId()),
        role,
        static_cast<unsigned long>(sessionNonce));

    const std::string logsDir = dllDir + "\\logs";
    (void)CreateDirectoryA(logsDir.c_str(), nullptr);
    const std::string dumpDir = logsDir + "\\desync_" + stamp;
    if (!CreateDirectoryA(dumpDir.c_str(), nullptr)
        && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        mod::Log(
            "DESYNC_MONITOR: dump directory creation failed '%s' err=%lu",
            dumpDir.c_str(),
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    mod::Log(
        "DESYNC_MONITOR: *** CHECKSUM WINDOW CAPTURED *** frame=%d dumping to '%s'",
        triggerFrame,
        dumpDir.c_str());
    bool requiredArtifactsOk = true;

    // ---- report.txt ------------------------------------------------------
    const std::string reportPath = dumpDir + "\\report.txt";
    HANDLE report = CreateFileA(
        reportPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    g_dumpWriteOk = true;
    if (report != INVALID_HANDLE_VALUE)
    {
        AppendFormat(report, "EFZ InGameNetplay checksum-divergence report %s\r\n", stamp);
        AppendFormat(report, "mod=%s build=%s\r\n",
            netplay::build_info::kVersion,
            netplay::build_info::kBuildTimestamp);
        AppendFormat(report, "revival=%s wine=%d\r\n",
            (netplay::bridge::takeover::g_activeRevival != nullptr
             && netplay::bridge::takeover::g_activeRevival->versionTag != nullptr)
                ? netplay::bridge::takeover::g_activeRevival->versionTag
                : "unknown",
            netplay::bridge::IsRunningUnderWine() ? 1 : 0);
        AppendFormat(report, "role=%d (0=host 1=join) nickname='%s' peer='%s' hostPort=%u\r\n",
            role, nick, peer, static_cast<unsigned>(hostPort));
        AppendFormat(report,
            "tracer: wire=%u schema=0x%08lX nonce=0x%08lX negotiatedStart=%d\r\n",
            static_cast<unsigned>(kPacketVersion),
            static_cast<unsigned long>(WireSchemaIdentity()),
            static_cast<unsigned long>(sessionNonce),
            captureStartFrame);
        AppendFormat(report,
            "type47: generation=%ld sessionSequenceStart=%ld "
            "sessionSequenceEnd=%ld triggerSequence=%ld "
            "snapshotBoundaryMarkers=%d\r\n",
            static_cast<long>(InterlockedCompareExchange(
                &g_type47SessionGeneration, 0, 0)),
            static_cast<long>(InterlockedCompareExchange(
                &g_type47SessionStartSequence, 0, 0)),
            static_cast<long>(InterlockedCompareExchange(
                &g_type47SessionEndSequence, 0, 0)),
            static_cast<long>(InterlockedCompareExchange(
                &g_type47EvidenceTriggerSequence, 0, 0)),
            g_snapshotMarkersAvailableForSession ? 1 : 0);
        if (g_snapshotMarkersAvailableForSession)
        {
            AppendText(report,
                "type47 snapshot markers: snapshot_save rows capture the state "
                "at Revival savestate-save ENTRY; snapshot_load rows capture "
                "the state after savestate-restore RETURNS. Passes between a "
                "save and the load that rewinds it show exactly what re-"
                "simulation changed. rng_internal_advances in type47_trace.csv "
                "remain Park-Miller recurrence steps, not logical rand() "
                "calls.\r\n");
        }
        else
        {
            AppendText(report,
                "type47 limitation: this session brackets EFZ effect updates but "
                "had no active Revival snapshot SAVE/LOAD markers (unsupported "
                "build, signature mismatch, or disabled setting). A first unequal "
                "pre-update tuple cannot by itself distinguish restore, fan-out, or "
                "iteration scheduling. rng_internal_advances in type47_trace.csv are "
                "Park-Miller recurrence steps, not logical rand() calls.\r\n");
        }
        AppendFormat(report,
            "forensic trigger: frame=%d layer=%s battleEpoch=%ld\r\n",
            triggerFrame,
            triggerLayer,
            static_cast<long>(triggerEpoch));
        AppendFormat(report,
            "selected-gameplay 96-frame window start: %d (-1 means not reached)\r\n",
            confirmedFrame);
        AppendFormat(report, "frames sampled this session: %u\r\n", sampleCount);
        AppendFormat(report,
            "checksum change-mask: identity=0x%08lX bytes=%u/%u\r\n"
            "Only identical hash+count peers are gameplay-checksum comparable. This is a\r\n"
            "selected-field projection; scalar equality does not prove full-state equality.\r\n\r\n",
            static_cast<unsigned long>(maskHash),
            maskBytes,
            static_cast<unsigned>(kRecordRegionBytes));

        // Do not call bridge/takeover status getters here: session teardown
        // owns those mutexes on normal cancellation paths. Per-frame context
        // already captured in FrameRecord remains lock-independent.

        // ---- Exported game state (chars / stage / round / wins) -----------
        {
            const EFZNetplayState* state =
                netplay::bridge::state_export::GetExportedState();
            if (state != nullptr)
            {
                AppendFormat(report,
                    "game: activity=%u p1Char=%u p2Char=%u stage=%u round=%u "
                    "wins=%d-%d matchCounter=%d local='%s' p1='%s' p2='%s'\r\n",
                    static_cast<unsigned>(state->activityPhase),
                    static_cast<unsigned>(state->p1CharId),
                    static_cast<unsigned>(state->p2CharId),
                    static_cast<unsigned>(state->stageId),
                    static_cast<unsigned>(state->roundIndex),
                    state->p1Wins,
                    state->p2Wins,
                    state->matchCounter,
                    state->localNickname,
                    state->p1Name,
                    state->p2Name);
            }
        }

        // ---- Revival session object snapshot -------------------------------
        {
            const uintptr_t session = netplay::bridge::takeover::g_lastValidatedSessionPtr;
            const netplay::bridge::takeover::RevivalAddressProfile* profile =
                netplay::bridge::takeover::g_activeRevival;
            if (session != 0 && profile != nullptr)
            {
                int curFrame = -1;
                int commit = -1;
                int syncFeed = -1;
                int matchId = -1;
                int prevMode = -1;
                int curMode = -1;
                int matchStart = -1;
                int advCtr = -1;
                int inputDelay = -1;
                int pingMs = -1;
                int activePlayer = -1;
                int queuePlayer = -1;
                int p1Wins = -1;
                int p2Wins = -1;
                const uintptr_t gmBase = session + profile->sessionOffsetGameModeSnapshot;
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetCurrentFrame), &curFrame);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 16), &commit);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 20), &syncFeed);
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetMatchId), &matchId);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase), &prevMode);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 4), &curMode);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 8), &matchStart);
                (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 12), &advCtr);
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetInputDelay), &inputDelay);
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetPingMs), &pingMs);
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetActivePlayer), &activePlayer);
                (void)SafeReadInt(reinterpret_cast<const void*>(
                    session + profile->sessionOffsetQueuePlayer), &queuePlayer);
                if (profile->sessionOffsetP1Wins != 0)
                {
                    (void)SafeReadInt(reinterpret_cast<const void*>(
                        session + profile->sessionOffsetP1Wins), &p1Wins);
                    (void)SafeReadInt(reinterpret_cast<const void*>(
                        session + profile->sessionOffsetP2Wins), &p2Wins);
                }
                char p1SessionName[128] = {};
                char p2SessionName[128] = {};
                ReadSessionName(session, profile->sessionOffsetP1Name,
                    p1SessionName, sizeof(p1SessionName));
                ReadSessionName(session, profile->sessionOffsetP2Name,
                    p2SessionName, sizeof(p2SessionName));
                AppendFormat(report,
                    "session: ptr=0x%08lX matchId=%d frame=%d commit=%d syncFeed=%d "
                    "mode=%d->%d matchStart=%d advCtr=%d\r\n",
                    static_cast<unsigned long>(session),
                    matchId, curFrame, commit, syncFeed,
                    prevMode, curMode, matchStart, advCtr);
                AppendFormat(report,
                    "session: delay=%d ping=%d active=%d queue=%d wins=%d-%d "
                    "localLen=%d remoteLen=%d p1='%s' p2='%s'\r\n",
                    inputDelay, pingMs, activePlayer, queuePlayer, p1Wins, p2Wins,
                    ReadHistoryLength(session, profile->sessionOffsetHistoryPrimaryVec),
                    ReadHistoryLength(session, profile->sessionOffsetHistorySecondaryVec),
                    p1SessionName,
                    p2SessionName);
            }
        }

        // ---- Wire rings -----------------------------------------------------
        {
            static const char* const kWires[] = {
                "InputP1", "InputP2", "Sync", "Net", "Quit", "LoadMatch", "Init"};
            AppendText(report, "wires:");
            for (size_t i = 0; i < sizeof(kWires) / sizeof(kWires[0]); ++i)
            {
                DWORD head = 0;
                DWORD tail = 0;
                const bool ok = ProbeWireHeadTail(kWires[i], &head, &tail);
                AppendFormat(report, " %s(%s h=%lu t=%lu)",
                    kWires[i],
                    ok ? "ok" : "NO",
                    static_cast<unsigned long>(head),
                    static_cast<unsigned long>(tail));
            }
            AppendText(report, "\r\n");
        }

        // ---- EfzRevival.ini highlights (config mismatches cause desyncs) ---
        {
            const std::string iniPath = gameDir + "\\EfzRevival.ini";
            AppendFormat(report,
                "ini: MaxRollback=%u Port=%u Debug=%u (full copy: EfzRevival.ini)\r\n\r\n",
                GetPrivateProfileIntA("Network", "MaxRollback", 0, iniPath.c_str()),
                GetPrivateProfileIntA("Network", "Port", 0, iniPath.c_str()),
                GetPrivateProfileIntA("Global", "Debug", 0, iniPath.c_str()));
        }
        AppendText(report,
            "Checksum comparisons around the first forensic trigger "
            "(oldest first):\r\n");
        for (size_t i = 0; i < kCompareLogDepth; ++i)
        {
            AppendCompareEntry(
                report,
                compareCopy[(compareNext + i) % kCompareLogDepth]);
        }
        for (size_t i = 0; i < postCompareCount; ++i)
        {
            AppendCompareEntry(report, postCompareCopy[i]);
        }
        AppendText(report,
            "\r\nSelected-gameplay contiguous confirmation run "
            "(separate from causal onset window):\r\n");
        if (gameplayRunCount < static_cast<size_t>(kConfirmMismatchFrames))
        {
            AppendFormat(report,
                "not confirmed; retained %u/%d contiguous mismatches\r\n",
                static_cast<unsigned>(gameplayRunCount),
                kConfirmMismatchFrames);
        }
        for (size_t i = 0; i < gameplayRunCount; ++i)
        {
            AppendCompareEntry(report, gameplayRunCopy[i]);
        }
        AppendText(report,
            "\r\nSee frames.txt for memory dumps of the recorded frames around the divergence.\r\n"
            "Compare frames.txt from both sides to find the first differing bytes.\r\n");
        CloseHandle(report);
        requiredArtifactsOk = requiredArtifactsOk && g_dumpWriteOk;
    }
    else
    {
        requiredArtifactsOk = false;
        mod::Log(
            "DESYNC_MONITOR: required report creation failed '%s' err=%lu",
            reportPath.c_str(),
            static_cast<unsigned long>(GetLastError()));
    }

    // Exact learned-mask identity. Equal byte counts alone are insufficient
    // to establish that two peers hashed the same selected-field function.
    const std::string maskPath = dumpDir + "\\change_mask.bin";
    HANDLE maskFile = CreateFileA(
        maskPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (maskFile != INVALID_HANDLE_VALUE)
    {
        DWORD written = 0;
        const BOOL wrote = WriteFile(
            maskFile,
            maskCopy,
            static_cast<DWORD>(sizeof(maskCopy)),
            &written,
            nullptr);
        CloseHandle(maskFile);
        if (!wrote || written != static_cast<DWORD>(sizeof(maskCopy)))
        {
            requiredArtifactsOk = false;
        }
    }
    else
    {
        requiredArtifactsOk = false;
    }

    // ---- frames.txt ------------------------------------------------------
    const std::string framesPath = dumpDir + "\\frames.txt";
    HANDLE frames = CreateFileA(
        framesPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    g_dumpWriteOk = true;
    if (frames != INVALID_HANDLE_VALUE)
    {
        // Oldest-to-newest frozen pre-window plus the bounded append-only
        // post-window. Continuing play cannot overwrite either onset side.
        for (size_t i = 0; i < kRegionRingDepth; ++i)
        {
            const FrameRecord& rec = regionCopy[(regionNext + i) % kRegionRingDepth];
            if (rec.frame < 0
                || rec.frame < triggerFrame - 32
                || rec.frame > triggerFrame + 32)
            {
                continue;
            }
            AppendFrameRecord(frames, rec, triggerFrame);
        }
        for (size_t i = 0; i < postRegionCount; ++i)
        {
            const FrameRecord& rec = postRegionCopy[i];
            if (rec.frame >= triggerFrame && rec.frame <= triggerFrame + 32)
            {
                AppendFrameRecord(frames, rec, triggerFrame);
            }
        }
        CloseHandle(frames);
        requiredArtifactsOk = requiredArtifactsOk && g_dumpWriteOk;
    }
    else
    {
        requiredArtifactsOk = false;
    }

    // ---- inputs.txt ------------------------------------------------------
    // Wide window of the committed input pairs straight from the session's
    // history vectors: diffing both sides' inputs.txt separates "inputs
    // diverged" (transport bug) from "same inputs, states diverged" (logic
    // desync).  The vectors persist for the whole match, so this can reach
    // much further back than the region ring.
    {
        const uintptr_t session = netplay::bridge::takeover::g_lastValidatedSessionPtr;
        const netplay::bridge::takeover::RevivalAddressProfile* profile =
            netplay::bridge::takeover::g_activeRevival;
        if (session != 0 && profile != nullptr)
        {
            const std::string inputsPath = dumpDir + "\\inputs.txt";
            HANDLE inputs = CreateFileA(
                inputsPath.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,
                nullptr,
                CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (inputs != INVALID_HANDLE_VALUE)
            {
                const int localLen =
                    ReadHistoryLength(session, profile->sessionOffsetHistoryPrimaryVec);
                const int remoteLen =
                    ReadHistoryLength(session, profile->sessionOffsetHistorySecondaryVec);
                const int maxLen = (localLen < remoteLen) ? localLen : remoteLen;
                int activePlayer = -1;
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(
                        session + profile->sessionOffsetActivePlayer),
                    &activePlayer);
                int windowBegin = triggerFrame - 192;
                int windowEnd = triggerFrame + 64;
                if (windowBegin < 0) windowBegin = 0;
                if (windowEnd > maxLen) windowEnd = maxLen;
                AppendFormat(inputs,
                    "committed input pairs, frames %d..%d "
                    "(primary/local=%d secondary/remote=%d activePlayer=%d)\r\n",
                    windowBegin, windowEnd - 1, localLen, remoteLen, activePlayer);
                for (int f = windowBegin; f < windowEnd; ++f)
                {
                    uint16_t primary = 0;
                    uint16_t secondary = 0;
                    const bool ok1 = ReadHistoryInput(
                        session,
                        profile->sessionOffsetHistoryPrimaryVec,
                        f,
                        &primary);
                    const bool ok2 = ReadHistoryInput(
                        session,
                        profile->sessionOffsetHistorySecondaryVec,
                        f,
                        &secondary);
                    const uint16_t p1 = activePlayer == 1 ? secondary : primary;
                    const uint16_t p2 = activePlayer == 1 ? primary : secondary;
                    AppendFormat(inputs,
                        "%6d primary=%04X secondary=%04X p1=%04X p2=%04X%s%s\r\n",
                        f,
                        static_cast<unsigned>(primary),
                        static_cast<unsigned>(secondary),
                        static_cast<unsigned>(p1),
                        static_cast<unsigned>(p2),
                        (!ok1 || !ok2) ? " [read-failed]" : "",
                        f == triggerFrame ? "  <== forensic trigger frame" : "");
                }
                CloseHandle(inputs);
            }
        }
    }

    // Detailed type-47 evidence is written only here, after the receive
    // worker has stopped and active simulation tracing has been quiesced.
    requiredArtifactsOk = WriteType47TraceCsv(dumpDir) && requiredArtifactsOk;
    requiredArtifactsOk = WriteRngTraceCsv(dumpDir) && requiredArtifactsOk;

    // ---- log copies ------------------------------------------------------
    CopyLogInto(dumpDir, gameDir + "\\EfzRevival.ini", "EfzRevival.ini");
    CopyLogInto(dumpDir, gameDir + "\\logEfz.txt", "logEfz.txt");
    CopyLogInto(dumpDir, gameDir + "\\logNet.txt", "logNet.txt");
    CopyLogInto(dumpDir, gameDir + "\\logDdraw.txt", "logDdraw.txt");
    CopyLogInto(dumpDir, dllDir + "\\logs\\native_host\\logEfz.txt", "native_host_logEfz.txt");
    CopyLogInto(dumpDir, dllDir + "\\logs\\native_host\\logNet.txt", "native_host_logNet.txt");
    CopyLogInto(dumpDir, dllDir + "\\logs\\efz_netplay_mod.log", "efz_netplay_mod.log");

    if (!requiredArtifactsOk)
    {
        mod::Log(
            "DESYNC_MONITOR: dump incomplete '%s'; required artifact write "
            "failed",
            dumpDir.c_str());
        return false;
    }
    mod::Log("DESYNC_MONITOR: dump complete '%s'", dumpDir.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// Worker thread: negotiate a symmetric future start, receive peer checksums,
// and compare them. Disk output is deferred until session teardown.
// ---------------------------------------------------------------------------

void LogTransportDrainOutcome(
    bool timedOut,
    unsigned unsentSamples,
    bool triggerPending)
{
    mod::Log(
        "DESYNC_MONITOR: transport drain %s unsentSamples=%u "
        "triggerPending=%d",
        timedOut ? "deadline reached" : "complete",
        unsentSamples,
        triggerPending ? 1 : 0);
}

DWORD WINAPI WorkerThreadProc(LPVOID)
{
    while (InterlockedCompareExchange(&g_workerStop, 0, 0) != 2)
    {
        PumpOutgoingSamples();
        bool pendingWindowCaptured = false;
        bool finishDrain = false;
        bool drainTimedOut = false;
        unsigned drainUnsentSamples = 0;
        bool drainTriggerPending = false;
        const DWORD loopNow = GetTickCount();
        EnterCriticalSection(&g_lock);
        if (IsTransportProcessing())
        {
            pendingWindowCaptured = DrainComparableSamplesLocked();
            // Run independently of select timeouts so sustained sample traffic
            // cannot starve a lost evidence control packet.
            RetryEvidenceTriggerLocked(loopNow);
        }
        if (InterlockedCompareExchange(&g_workerStop, 0, 0) == 1)
        {
            if (InterlockedCompareExchange(&g_transportDraining, 0, 0) == 0)
            {
                finishDrain = true;
            }
            else
            {
                drainUnsentSamples = g_sampleCount > g_sentSampleCount
                    ? g_sampleCount - g_sentSampleCount
                    : 0;
                drainTriggerPending = IsEvidenceDeliveryPendingLocked();
                const LONG quietUntil = InterlockedCompareExchange(
                    &g_workerDrainQuietUntilTick, 0, 0);
                const LONG deadline = InterlockedCompareExchange(
                    &g_workerDrainDeadlineTick, 0, 0);
                drainTimedOut = TickDeadlineReached(loopNow, deadline);
                finishDrain = drainTimedOut
                    || (TickDeadlineReached(loopNow, quietUntil)
                        && drainUnsentSamples == 0
                        && !drainTriggerPending);
            }
        }
        LeaveCriticalSection(&g_lock);
        if (pendingWindowCaptured)
        {
            mod::Log(
                "DESYNC_MONITOR: contiguous selected-gameplay window captured; "
                "disk dump deferred until session end");
        }
        if (drainTimedOut)
        {
            // The deadline is a hard bound. Quiet completion below polls the
            // receive queue once more; a deadline may deliberately abandon a
            // pathological/lossy tail rather than stall session teardown.
            bool finalWindowCaptured = false;
            EnterCriticalSection(&g_lock);
            finalWindowCaptured = DrainComparableSamplesLocked(true);
            RetryEvidenceTriggerLocked(GetTickCount());
            drainUnsentSamples = g_sampleCount > g_sentSampleCount
                ? g_sampleCount - g_sentSampleCount
                : 0;
            drainTriggerPending = IsEvidenceDeliveryPendingLocked();
            LeaveCriticalSection(&g_lock);
            if (finalWindowCaptured)
            {
                mod::Log(
                    "DESYNC_MONITOR: final reorder holdback produced a "
                    "gameplay mismatch window at the drain deadline");
            }
            LogTransportDrainOutcome(
                true,
                drainUnsentSamples,
                drainTriggerPending);
            break;
        }
        const SOCKET sock = g_socket;
        if (sock == INVALID_SOCKET)
        {
            break;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval timeout = {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000;
        const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
        if (selected == SOCKET_ERROR)
        {
            break;
        }
        if (selected == 0)
        {
            const DWORD now = GetTickCount();
            EnterCriticalSection(&g_lock);
            if (InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0
                && now - g_lastControlSendTick >= 500u)
            {
                const int startFrame = static_cast<int>(
                    InterlockedCompareExchange(&g_captureStartFrame, -1, -1));
                if (g_role == 1)
                {
                    if (g_handshakePhase == 0)
                        SendControlPacket(PacketKind::Hello, -1);
                    else if (g_handshakePhase == 1)
                        SendControlPacket(PacketKind::Ready, startFrame);
                    else if (g_handshakePhase == 2)
                        SendControlPacket(PacketKind::StartAck, startFrame);
                    else if (g_handshakePhase == 4)
                        SendControlPacket(PacketKind::Abort, startFrame);
                }
                else
                {
                    if (g_handshakePhase == 1)
                        SendControlPacket(PacketKind::HelloAck, -1);
                    else if (g_handshakePhase == 2)
                        SendControlPacket(PacketKind::Start, startFrame);
                    else if (g_handshakePhase == 4)
                        SendControlPacket(PacketKind::Abort, startFrame);
                }
            }
            if (InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0
                && !g_peerSeen
                && !g_peerAbsenceLogged
                && g_sessionStartTick != 0
                && now - g_sessionStartTick > 15000u)
            {
                g_peerAbsenceLogged = true;
                mod::Log(
                    "DESYNC_MONITOR: no compatible peer handshake after 15s; "
                    "recorder remained idle (normal gameplay path unchanged)");
            }
            LeaveCriticalSection(&g_lock);
            if (finishDrain)
            {
                // select just observed an empty receive queue. Exit only now,
                // rather than before polling, so a datagram queued exactly at
                // the quiet boundary still gets processed. With the queue
                // empty, any remaining <=8-frame reorder gap is permanent:
                // exhaust every retained common frame before deciding whether
                // the transport can close.
                bool finalWindowCaptured = false;
                EnterCriticalSection(&g_lock);
                finalWindowCaptured = DrainComparableSamplesLocked(true);
                RetryEvidenceTriggerLocked(GetTickCount());
                drainUnsentSamples = g_sampleCount > g_sentSampleCount
                    ? g_sampleCount - g_sentSampleCount
                    : 0;
                drainTriggerPending = IsEvidenceDeliveryPendingLocked();
                const bool closeAfterFinalFlush =
                    drainUnsentSamples == 0 && !drainTriggerPending;
                LeaveCriticalSection(&g_lock);
                if (finalWindowCaptured)
                {
                    mod::Log(
                        "DESYNC_MONITOR: final reorder holdback produced a "
                        "gameplay mismatch window; trigger tail reopened");
                }
                if (!closeAfterFinalFlush)
                {
                    continue;
                }
                LogTransportDrainOutcome(
                    false,
                    drainUnsentSamples,
                    drainTriggerPending);
                break;
            }
            continue;
        }

        WirePacket packet = {};
        sockaddr_in from = {};
        int fromLen = sizeof(from);
        const int received = recvfrom(
            sock,
            reinterpret_cast<char*>(&packet),
            static_cast<int>(sizeof(packet)),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen);
        if (received == SOCKET_ERROR)
        {
            if (InterlockedCompareExchange(&g_workerStop, 0, 0) == 2)
            {
                break;
            }
            continue;
        }
        if (received != static_cast<int>(sizeof(packet))
            || packet.magic != kPacketMagic
            || packet.version != kPacketVersion
            || packet.schemaId != WireSchemaIdentity()
            || packet.role > 1
            || packet.role == static_cast<uint8_t>(g_role))
        {
            continue;
        }

        bool confirmedNow = false;
        EnterCriticalSection(&g_lock);
        if (IsTransportProcessing())
        {
            const PacketKind kind = static_cast<PacketKind>(packet.kind);
            const bool activeSession =
                InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0;
            const bool drainPacket = kind == PacketKind::Samples
                || kind == PacketKind::Trigger
                || kind == PacketKind::TriggerAck
                || kind == PacketKind::TriggerNack;
            if (!activeSession && !drainPacket)
            {
                LeaveCriticalSection(&g_lock);
                continue;
            }

            // The first joiner HELLO creates the nonce/endpoint. Duplicate
            // control packets are response-only: no accepted transition is
            // ever rewound and an unrelated nonce cannot replace the peer.
            if (kind == PacketKind::Hello
                && g_role == 0
                && packet.role == 1
                && packet.sessionNonce != 0)
            {
                if (g_sessionNonce == 0 && g_handshakePhase == 0)
                {
                    g_sessionNonce = packet.sessionNonce;
                    g_peerEndpoint = from;
                    InterlockedExchange(&g_peerEndpointValid, 1);
                    g_peerSeen = true;
                    g_handshakePhase = 1;
                    mod::Log(
                        "DESYNC_MONITOR: compatible peer HELLO nonce=0x%08lX",
                        static_cast<unsigned long>(g_sessionNonce));
                }
                if (packet.sessionNonce == g_sessionNonce
                    && PacketSourceMatchesPeer(from))
                {
                    const int chosenStart = static_cast<int>(
                        InterlockedCompareExchange(&g_captureStartFrame, 0, 0));
                    if (g_handshakePhase == 1)
                        SendControlPacket(PacketKind::HelloAck, -1);
                    else if (g_handshakePhase == 2 || g_handshakePhase == 3)
                        SendControlPacket(PacketKind::Start, chosenStart);
                    else if (g_handshakePhase == 4)
                        SendControlPacket(PacketKind::Abort, chosenStart);
                }
            }
            else if (packet.sessionNonce == g_sessionNonce
                && g_sessionNonce != 0
                && PacketSourceMatchesPeer(from))
            {
                if (InterlockedCompareExchange(
                        &g_transportDraining, 0, 0) != 0)
                {
                    InterlockedExchange(
                        &g_workerDrainQuietUntilTick,
                        static_cast<LONG>(
                            GetTickCount() + kTransportDrainQuietMs));
                }
                if (!g_peerSeen)
                {
                    g_peerSeen = true;
                    mod::Log(
                        "DESYNC_MONITOR: compatible peer side-channel established role=%u",
                        static_cast<unsigned>(packet.role));
                }

                if (kind == PacketKind::Abort)
                {
                    if (g_handshakePhase != 4)
                    {
                        mod::Log(
                            "DESYNC_MONITOR: peer aborted tracer handshake; "
                            "gameplay continues without capture");
                    }
                    g_handshakePhase = 4;
                    InterlockedExchange(&g_captureArmed, 0);
                }
                else if (kind == PacketKind::HelloAck && g_role == 1
                    && g_handshakePhase == 0)
                {
                    const LONG latest =
                        InterlockedCompareExchange(&g_latestFrame, 0, 0);
                    const int startFrame =
                        (latest >= 0 ? static_cast<int>(latest) : 0)
                        + kHandshakeLeadFrames;
                    InterlockedExchange(&g_captureStartFrame, startFrame);
                    g_handshakePhase = 1;
                    SendControlPacket(PacketKind::Ready, startFrame);
                }
                else if (kind == PacketKind::HelloAck && g_role == 1
                    && g_handshakePhase == 1)
                {
                    SendControlPacket(
                        PacketKind::Ready,
                        static_cast<int>(InterlockedCompareExchange(
                            &g_captureStartFrame, 0, 0)));
                }
                else if (kind == PacketKind::Ready && g_role == 0
                    && g_handshakePhase == 1)
                {
                    const LONG latest =
                        InterlockedCompareExchange(&g_latestFrame, 0, 0);
                    const int minimumStart =
                        (latest >= 0 ? static_cast<int>(latest) : 0)
                        + kHandshakeLeadFrames;
                    const int startFrame =
                        packet.startFrame > minimumStart
                            ? packet.startFrame
                            : minimumStart;
                    InterlockedExchange(&g_captureStartFrame, startFrame);
                    InterlockedExchange(&g_captureArmed, 0);
                    g_handshakePhase = 2;
                    SendControlPacket(PacketKind::Start, startFrame);
                    mod::Log(
                        "DESYNC_MONITOR: symmetric capture scheduled at frame %d nonce=0x%08lX",
                        startFrame,
                        static_cast<unsigned long>(g_sessionNonce));
                }
                else if (kind == PacketKind::Ready && g_role == 0
                    && g_handshakePhase == 2)
                {
                    SendControlPacket(
                        PacketKind::Start,
                        static_cast<int>(InterlockedCompareExchange(
                            &g_captureStartFrame, 0, 0)));
                }
                else if (kind == PacketKind::Start && g_role == 1
                    && (g_handshakePhase == 1 || g_handshakePhase == 2)
                    && packet.startFrame >= 0)
                {
                    const int chosenStart = static_cast<int>(
                        InterlockedCompareExchange(&g_captureStartFrame, 0, 0));
                    if (g_handshakePhase == 2 && packet.startFrame == chosenStart)
                    {
                        // Idempotent retry after an ACK was lost. Do not apply
                        // the future-frame test again as the agreed frame
                        // naturally approaches while capture is armed.
                        SendControlPacket(PacketKind::StartAck, packet.startFrame);
                    }
                    else if (g_handshakePhase == 2)
                    {
                        g_handshakePhase = 4;
                        InterlockedExchange(&g_captureArmed, 0);
                        SendControlPacket(PacketKind::Abort, packet.startFrame);
                        mod::Log(
                            "DESYNC_MONITOR: tracer handshake aborted; peer changed "
                            "accepted start frame %d -> %d",
                            chosenStart,
                            packet.startFrame);
                    }
                    else
                    {
                    const LONG latest =
                        InterlockedCompareExchange(&g_latestFrame, 0, 0);
                    const int minimumSafe =
                        (latest >= 0 ? static_cast<int>(latest) : 0)
                        + kHandshakeSafetyFrames;
                    if (packet.startFrame < minimumSafe)
                    {
                        g_handshakePhase = 4;
                        InterlockedExchange(&g_captureArmed, 0);
                        SendControlPacket(PacketKind::Abort, packet.startFrame);
                        mod::Log(
                            "DESYNC_MONITOR: tracer handshake aborted; start frame %d "
                            "arrived too late (minimum safe %d)",
                            packet.startFrame,
                            minimumSafe);
                    }
                    else
                    {
                        InterlockedExchange(&g_captureStartFrame, packet.startFrame);
                        InterlockedExchange(&g_captureArmed, 1);
                        g_handshakePhase = 2;
                        SendControlPacket(PacketKind::StartAck, packet.startFrame);
                        mod::Log(
                            "DESYNC_MONITOR: symmetric capture accepted at frame %d nonce=0x%08lX",
                            packet.startFrame,
                            static_cast<unsigned long>(g_sessionNonce));
                    }
                    }
                }
                else if (kind == PacketKind::StartAck && g_role == 0
                    && g_handshakePhase == 2
                    && packet.startFrame ==
                        InterlockedCompareExchange(&g_captureStartFrame, 0, 0))
                {
                    const LONG latest =
                        InterlockedCompareExchange(&g_latestFrame, 0, 0);
                    const int minimumSafe =
                        (latest >= 0 ? static_cast<int>(latest) : 0)
                        + kHandshakeSafetyFrames;
                    if (packet.startFrame < minimumSafe)
                    {
                        g_handshakePhase = 4;
                        InterlockedExchange(&g_captureArmed, 0);
                        SendControlPacket(PacketKind::Abort, packet.startFrame);
                        mod::Log(
                            "DESYNC_MONITOR: tracer handshake aborted; ACK for frame %d "
                            "arrived too late (minimum safe %d)",
                            packet.startFrame,
                            minimumSafe);
                    }
                    else
                    {
                        InterlockedExchange(&g_captureArmed, 1);
                        g_handshakePhase = 3;
                        mod::Log("DESYNC_MONITOR: capture start acknowledged by peer");
                    }
                }
                else if ((kind == PacketKind::Trigger
                        || kind == PacketKind::TriggerAck
                        || kind == PacketKind::TriggerNack)
                    && PacketSourceMatchesPeer(from)
                    && packet.battleEpoch == static_cast<uint16_t>(
                        InterlockedCompareExchange(&g_battleEpoch, 0, 0))
                    && packet.startFrame >= 0
                    && InterlockedCompareExchange(
                        &g_captureStartFrame, -1, -1) >= 0
                    && packet.startFrame >= InterlockedCompareExchange(
                        &g_captureStartFrame, -1, -1)
                    && IsEvidenceLayerCode(packet.count))
                {
                    const EvidenceLayer layer =
                        static_cast<EvidenceLayer>(packet.count);
                    if (kind == PacketKind::Trigger)
                    {
                        if (g_role == 0)
                        {
                            // A join Trigger is only a proposal. The host owns
                            // the canonical tuple: earliest frame wins and a
                            // higher layer code wins ties at the same frame.
                            if (!HasFrameEvidenceLocked(packet.startFrame))
                            {
                                SendEvidenceControlPacket(
                                    PacketKind::TriggerNack,
                                    packet.startFrame,
                                    layer);
                                mod::Log(
                                    "DESYNC_MONITOR: join proposal unavailable "
                                    "frame=%d layer=%s; sent NACK",
                                    packet.startFrame,
                                    EvidenceLayerName(layer));
                            }
                            else
                            {
                                if (!g_evidenceTriggered)
                                {
                                    TriggerEvidenceLocked(
                                        packet.startFrame,
                                        layer,
                                        false);
                                }
                                else
                                {
                                    const EvidenceLayer currentLayer =
                                        static_cast<EvidenceLayer>(
                                            g_evidenceTriggerLayerCode);
                                    if (g_evidenceTriggerNacked)
                                    {
                                        // The joiner already proved that the
                                        // previous authority is unavailable.
                                        // A retained proposal from that same
                                        // peer is therefore the best common
                                        // tuple, even when it is later or a
                                        // weaker attribution layer.
                                        ReviseEvidenceTupleLocked(
                                            packet.startFrame,
                                            layer,
                                            false);
                                    }
                                    else if (IsPreferredEvidenceTuple(
                                            packet.startFrame,
                                            layer,
                                            g_evidenceTriggerFrame,
                                            currentLayer))
                                    {
                                        ReviseEvidenceTupleLocked(
                                            packet.startFrame,
                                            layer,
                                            false);
                                    }
                                }
                                SendEvidenceControlPacket(
                                    PacketKind::Trigger,
                                    g_evidenceTriggerFrame,
                                    static_cast<EvidenceLayer>(
                                        g_evidenceTriggerLayerCode));
                            }
                        }
                        else
                        {
                            // A host Trigger is authoritative only if this
                            // joiner still retains its target frame and it is
                            // not later/weaker than a locally raised proposal.
                            if (!HasFrameEvidenceLocked(packet.startFrame))
                            {
                                SendEvidenceControlPacket(
                                    PacketKind::TriggerNack,
                                    packet.startFrame,
                                    layer);
                                if (g_evidenceTriggered
                                    && g_evidenceTriggerLocallyRaised)
                                {
                                    SendEvidenceControlPacket(
                                        PacketKind::Trigger,
                                        g_evidenceTriggerFrame,
                                        static_cast<EvidenceLayer>(
                                            g_evidenceTriggerLayerCode));
                                }
                                mod::Log(
                                    "DESYNC_MONITOR: host authority unavailable "
                                    "frame=%d layer=%s; sent NACK",
                                    packet.startFrame,
                                    EvidenceLayerName(layer));
                            }
                            else if (!g_evidenceTriggered)
                            {
                                TriggerEvidenceLocked(
                                    packet.startFrame,
                                    layer,
                                    false);
                                SendEvidenceControlPacket(
                                    PacketKind::TriggerAck,
                                    packet.startFrame,
                                    layer);
                            }
                            else
                            {
                                const EvidenceLayer currentLayer =
                                    static_cast<EvidenceLayer>(
                                        g_evidenceTriggerLayerCode);
                                if (g_evidenceTriggerNacked)
                                {
                                    // The host could not retain our previous
                                    // proposal. This retained host tuple is a
                                    // proven common fallback; accepting it
                                    // prevents an unavailable proposal from
                                    // being echoed forever.
                                    ReviseEvidenceTupleLocked(
                                        packet.startFrame,
                                        layer,
                                        false);
                                    SendEvidenceControlPacket(
                                        PacketKind::TriggerAck,
                                        packet.startFrame,
                                        layer);
                                }
                                else if (EvidenceAuthoritySatisfiesProposal(
                                        packet.startFrame,
                                        layer,
                                        g_evidenceTriggerFrame,
                                        currentLayer))
                                {
                                    if (packet.startFrame
                                            != g_evidenceTriggerFrame
                                        || packet.count
                                            != g_evidenceTriggerLayerCode)
                                    {
                                        ReviseEvidenceTupleLocked(
                                            packet.startFrame,
                                            layer,
                                            false);
                                    }
                                    else
                                    {
                                        g_evidenceTriggerLocallyRaised = false;
                                        g_evidenceTriggerAcknowledged = true;
                                        g_evidenceTriggerNacked = false;
                                        g_lastEvidenceTriggerSendTick = 0;
                                    }
                                    // HasFrameEvidenceLocked above is the
                                    // mandatory precondition for every ACK.
                                    SendEvidenceControlPacket(
                                        PacketKind::TriggerAck,
                                        packet.startFrame,
                                        layer);
                                }
                                else
                                {
                                    // Preserve and resend the joiner's earlier
                                    // or stronger proposal; never falsely ACK a
                                    // host tuple that would discard it.
                                    g_evidenceTriggerLocallyRaised = true;
                                    g_evidenceTriggerAcknowledged = false;
                                    g_evidenceTriggerNacked = false;
                                    SendEvidenceControlPacket(
                                        PacketKind::Trigger,
                                        g_evidenceTriggerFrame,
                                        currentLayer);
                                }
                            }
                        }
                    }
                    else if (kind == PacketKind::TriggerAck
                        && g_role == 0
                        && g_evidenceTriggered
                        && !g_evidenceTriggerAcknowledged
                        && packet.startFrame == g_evidenceTriggerFrame
                        && packet.count == g_evidenceTriggerLayerCode
                        && packet.battleEpoch == static_cast<uint16_t>(
                            g_evidenceBattleEpoch))
                    {
                        g_evidenceTriggerAcknowledged = true;
                        g_evidenceTriggerNacked = false;
                        mod::Log(
                            "DESYNC_MONITOR: peer acknowledged forensic "
                            "onset frame=%d layer=%s",
                            g_evidenceTriggerFrame,
                            g_evidenceTriggerLayer);
                    }
                    else if (kind == PacketKind::TriggerNack
                        && g_evidenceTriggered
                        && packet.startFrame == g_evidenceTriggerFrame
                        && packet.count == g_evidenceTriggerLayerCode
                        && packet.battleEpoch == static_cast<uint16_t>(
                            g_evidenceBattleEpoch))
                    {
                        g_evidenceTriggerAcknowledged = false;
                        g_evidenceTriggerNacked = true;
                        mod::Log(
                            "DESYNC_MONITOR: peer lacks retained forensic "
                            "frame=%d layer=%s; trigger delivery stopped",
                            g_evidenceTriggerFrame,
                            g_evidenceTriggerLayer);
                    }
                }
                else if (kind == PacketKind::Samples
                    && packet.count > 0
                    && packet.count <= kSamplesPerPacket)
                {
                    const uint16_t localEpoch = static_cast<uint16_t>(
                        InterlockedCompareExchange(&g_battleEpoch, 0, 0));
                    if (packet.battleEpoch != localEpoch)
                    {
                        const int16_t epochDelta = static_cast<int16_t>(
                            static_cast<uint16_t>(packet.battleEpoch - localEpoch));
                        if (!g_epochMismatchLogged)
                        {
                            g_epochMismatchLogged = true;
                            mod::Log(
                                "DESYNC_MONITOR: capture-epoch mismatch local=%u "
                                "remote=%u relation=%s; no cross-battle samples "
                                "compared",
                                static_cast<unsigned>(localEpoch),
                                static_cast<unsigned>(packet.battleEpoch),
                                epochDelta < 0 ? "stale" : "future");
                        }
                        if (epochDelta > 0)
                        {
                            g_handshakePhase = 4;
                            InterlockedExchange(&g_captureArmed, 0);
                            // This is already the worker thread and the socket
                            // is nonblocking, so notify the peer immediately;
                            // continuous incoming samples cannot starve Abort.
                            SendControlPacket(
                                PacketKind::Abort,
                                static_cast<int>(InterlockedCompareExchange(
                                    &g_captureStartFrame, -1, -1)));
                        }
                    }
                    else
                    {
                        for (int i = 0; i < packet.count; ++i)
                        {
                            if (FindSample(
                                    g_remoteRing,
                                    packet.samples[i].frame) != nullptr)
                            {
                                continue;
                            }
                            ChecksumSample& slot = g_remoteRing[g_remoteRingNext];
                            g_remoteRingNext = (g_remoteRingNext + 1) % kSampleRingDepth;
                            slot.frame = packet.samples[i].frame;
                            slot.checksum = packet.samples[i].checksum;
                            slot.effectHash = packet.samples[i].effectHash;
                            slot.rngState = packet.samples[i].rngState;
                            slot.maskHash = packet.maskHash;
                            slot.maskByteCount = packet.maskByteCount;
                            if (slot.frame > g_highestRemoteFrame)
                            {
                                g_highestRemoteFrame = slot.frame;
                            }
                        }
                        confirmedNow |= DrainComparableSamplesLocked();
                    }
                }

                // A receive-side comparison may have raised the first
                // evidence trigger. Send its first control packet now rather
                // than waiting for another select cycle; normal teardown may
                // already be inside its bounded transport tail.
                RetryEvidenceTriggerLocked(GetTickCount());

                // A game-thread capture fault can move the tracer to Abort
                // while peer Samples keep the socket continuously readable.
                // Retry control here as well as in the select-timeout path so
                // incoming traffic cannot starve the peer notification.
                const DWORD controlNow = GetTickCount();
                if (g_handshakePhase == 4
                    && controlNow - g_lastControlSendTick >= 500u)
                {
                    SendControlPacket(
                        PacketKind::Abort,
                        static_cast<int>(InterlockedCompareExchange(
                            &g_captureStartFrame, -1, -1)));
                }
            }
        }
        LeaveCriticalSection(&g_lock);

        if (confirmedNow)
        {
            mod::Log(
                "DESYNC_MONITOR: contiguous gameplay mismatch window captured; "
                "disk dump deferred until session end");
        }
    }
    return 0;
}

bool StopWorkerAndChannel()
{
    if (g_workerThread == nullptr)
    {
        CloseSideChannel();
        InterlockedExchange(&g_transportDraining, 0);
        InterlockedExchange(&g_workerDrainQuietUntilTick, 0);
        InterlockedExchange(&g_workerDrainDeadlineTick, 0);
        InterlockedExchange(&g_workerStop, 0);
        return true;
    }

    const LONG priorStop = InterlockedCompareExchange(&g_workerStop, 0, 0);
    if (priorStop != 2)
    {
        const DWORD now = GetTickCount();
        InterlockedExchange(&g_transportDraining, 1);
        InterlockedExchange(
            &g_workerDrainQuietUntilTick,
            static_cast<LONG>(now + kTransportDrainQuietMs));
        InterlockedExchange(
            &g_workerDrainDeadlineTick,
            static_cast<LONG>(now + kTransportDrainDeadlineMs));
        // Publish the timing bounds before requesting graceful stop.
        InterlockedExchange(&g_workerStop, 1);
    }

    DWORD waitResult = WaitForSingleObject(
        g_workerThread,
        kTransportDrainDeadlineMs + 100u);
    if (waitResult != WAIT_OBJECT_0)
    {
        mod::Log(
            "DESYNC_MONITOR: graceful transport drain did not stop worker "
            "result=%lu; forcing worker stop",
            static_cast<unsigned long>(waitResult));
        InterlockedExchange(&g_workerStop, 2);
        // The socket remains worker-owned until the thread exits. Closing it
        // here would race the worker's plain SOCKET reads and could target a
        // recycled handle. select is bounded to 20 ms, so stop=2 is itself
        // the wake-up mechanism.
        waitResult = WaitForSingleObject(
            g_workerThread,
            kTransportForcedStopWaitMs);
        if (waitResult != WAIT_OBJECT_0)
        {
            mod::Log(
                "DESYNC_MONITOR: worker did not stop safely result=%lu; "
                "handle retained and restart refused",
                static_cast<unsigned long>(waitResult));
            return false;
        }
    }

    CloseSideChannel();
    CloseHandle(g_workerThread);
    g_workerThread = nullptr;
    InterlockedExchange(&g_transportDraining, 0);
    InterlockedExchange(&g_workerDrainQuietUntilTick, 0);
    InterlockedExchange(&g_workerDrainDeadlineTick, 0);
    InterlockedExchange(&g_workerStop, 0);
    return true;
}

void ResetMaskLocked(const char* reason)
{
    std::memset(g_changeMask, 0, sizeof(g_changeMask));
    g_prevRegionValid = false;
    g_maskSamples = 0;
    g_maskFrozen = false;
    g_maskByteCount = 0;
    g_maskHash = 0;
    g_maskMismatchLogged = false;
    if (reason != nullptr)
    {
        mod::Log("DESYNC_MONITOR: change-mask calibration restarted (%s)", reason);
    }
}

void ResetBattleCaptureLocked()
{
    for (size_t i = 0; i < kRegionRingDepth; ++i)
    {
        g_regionRing[i].frame = -1;
    }
    for (size_t i = 0; i < kSampleRingDepth; ++i)
    {
        g_localRing[i].frame = -1;
        g_remoteRing[i].frame = -1;
    }
    for (size_t i = 0; i < kCompareLogDepth; ++i)
    {
        g_compareLog[i].frame = -1;
    }
    for (size_t i = 0; i < kPostEvidenceDepth; ++i)
    {
        g_postEvidenceFrames[i].frame = -1;
        g_postEvidenceCompare[i].frame = -1;
    }
    for (size_t i = 0; i < kGameplayRunDepth; ++i)
    {
        g_gameplayMismatchRun[i].frame = -1;
    }
    g_regionRingNext = 0;
    g_localRingNext = 0;
    g_remoteRingNext = 0;
    g_compareLogNext = 0;
    g_postEvidenceFrameCount = 0;
    g_postEvidenceCompareCount = 0;
    g_gameplayMismatchRunCount = 0;
    g_gameplayMismatchRunFrozen = false;
    g_evidenceTriggered = false;
    g_evidenceTriggerFrame = -1;
    g_evidenceBattleEpoch = 0;
    g_evidenceTriggerLayer[0] = '\0';
    g_evidenceTriggerLayerCode = 0;
    g_evidenceTriggerLocallyRaised = false;
    g_evidenceTriggerAcknowledged = false;
    g_evidenceTriggerNacked = false;
    g_lastEvidenceTriggerSendTick = 0;
    g_evidenceRegionNext = 0;
    g_evidenceCompareNext = 0;
    g_evidenceMaskByteCount = 0;
    g_evidenceMaskHash = 0;
    std::memset(g_evidenceMask, 0, sizeof(g_evidenceMask));
    g_mismatchStreak = 0;
    g_firstMismatchFrame = -1;
    g_lastMismatchFrame = -1;
    g_lastComparedFrame = -1;
    g_nextCompareFrame = -1;
    g_highestLocalFrame = -1;
    g_highestRemoteFrame = -1;
    g_epochMismatchLogged = false;
    g_effectLayerRun = 0;
    g_effectLayerFirstFrame = -1;
    g_rngLayerRun = 0;
    g_rngLayerFirstFrame = -1;
    InterlockedExchange(&g_maxEventFrame, -1);
    InterlockedExchange(&g_snapshotLoadFromFrame, -1);
    InterlockedExchange(&g_rngTraceWriteSequence, 0);
    InterlockedExchange(&g_rngTraceEvidenceTrigger, 0);
    InterlockedExchange(&g_rngCurrentEffectSlot, 0xFFFF);
    InterlockedExchange(&g_rngCurrentEffectPhase, 0);
    InterlockedExchange(&g_lastAppliedSeed, 0);
    InterlockedExchange(&g_seedApplyLogCount, 0);
    g_confirmedFrame = -1;
    g_sampleCount = 0;
    g_sentSampleCount = 0;
    g_leftBattleSinceLastSample = false;
    g_lastSampledFrame = -1;
    g_validatedEffectGameSys = 0;
    g_effectArenaReadable = false;
    g_validatedRngAddress = 0;
    g_rngAddressReadable = false;
    ResetMaskLocked(nullptr);
    InterlockedExchange(&g_desyncConfirmed, 0);
}

void ResetRingsLocked()
{
    // This tracer deliberately supports one battle per negotiated session.
    // Assign its wire epoch before either peer can send a sample; lazily
    // incrementing it on the first local frame lets the faster peer's epoch 1
    // packet falsely abort the slower peer while it is still at epoch 0.
    InterlockedExchange(&g_battleEpoch, 1);
    ResetBattleCaptureLocked();
    g_peerSeen = false;
    g_peerAbsenceLogged = false;
    g_handshakePhase = 0;
    g_lastControlSendTick = 0;
    g_inBattle = false;
    InterlockedExchange(&g_latestFrame, -1);
    InterlockedExchange(&g_captureStartFrame, -1);
    InterlockedExchange(&g_captureArmed, 0);
    InterlockedExchange(&g_transportDraining, 0);
    InterlockedExchange(&g_workerDrainQuietUntilTick, 0);
    InterlockedExchange(&g_workerDrainDeadlineTick, 0);
}

// Freeze the worker before touching evidence. A timed-out stop keeps the
// handle, rings, and pending-dump latch intact so Shutdown or the next session
// start can retry instead of silently erasing the capture.
bool TryFinalizeEndedCapture()
{
    if (!StopWorkerAndChannel())
    {
        return false;
    }

    bool shouldDump = false;
    bool finalWindowCaptured = false;
    EnterCriticalSection(&g_lock);
    // Idempotent fail-safe for an already-dead worker (for example an
    // unexpected select error): the normal worker drain performs this before
    // closing its socket, but retained common tail frames must still be
    // compared and dumped locally when that path could not run.
    finalWindowCaptured = DrainComparableSamplesLocked(true);
    // The final peer samples can raise the first evidence trigger while the
    // worker is inside the bounded teardown drain, after NotifySessionEnded
    // took its initial pending snapshot. Latch it again after the join so the
    // next session cannot reset valid tail evidence without writing it.
    if (g_evidenceTriggered && !g_dumped)
    {
        g_dumpPending = true;
    }
    shouldDump = g_dumpPending && g_evidenceTriggered && !g_dumped;
    LeaveCriticalSection(&g_lock);
    if (finalWindowCaptured)
    {
        mod::Log(
            "DESYNC_MONITOR: post-join fail-safe released a retained "
            "gameplay mismatch window (peer trigger delivery unavailable)");
    }

    if (shouldDump)
    {
        if (!WriteDesyncDump())
        {
            mod::Log(
                "DESYNC_MONITOR: forensic dump failed; evidence remains "
                "latched for a later retry");
            return false;
        }
        EnterCriticalSection(&g_lock);
        g_dumped = true;
        g_dumpPending = false;
        LeaveCriticalSection(&g_lock);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void NotifySessionStarted(
    int netplayRole,
    const char* address,
    uint16_t port,
    const char* nickname)
{
    const bool tracingEnabled =
        netplay::mod_settings::IsDesyncDetectionEnabled();
    const bool comparableRole = netplayRole == 0 || netplayRole == 1;
    if ((!tracingEnabled || !comparableRole)
        && g_lockInited
        && InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0)
    {
        NotifySessionEnded("replaced_by_untraced_session");
    }
    if (!tracingEnabled)
    {
        return;
    }
    // Host (0) and Join (1) only; spectators (2) and spectate-redirect joins
    // (3) receive the state feed with their own frame numbering and cannot
    // be compared against the players.
    if (!comparableRole)
    {
        return;
    }

    EnsureLock();
    EnterCriticalSection(&g_lock);
    const bool replacedActiveSession =
        InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0;
    if (replacedActiveSession)
    {
        // Keep the worker eligible to flush already-recorded tail samples
        // after sessionActive is cleared.
        InterlockedExchange(&g_transportDraining, 1);
        InterlockedExchange(&g_sessionActive, 0);
        InterlockedExchange(&g_captureArmed, 0);
        if (g_evidenceTriggered && !g_dumped)
        {
            g_dumpPending = true;
        }
    }
    LeaveCriticalSection(&g_lock);
    // Close checksum admission under the same lock used by RecordFrameTick,
    // then seal type-47 producers. A tick that won the lock is represented in
    // both streams; a tick that lost it fails the append-time session check.
    SealType47TraceSession();
    if (replacedActiveSession)
    {
        mod::Log(
            "DESYNC_MONITOR: session start replaced an active capture; "
            "old evidence will be finalized before reset");
    }
    if (!TryFinalizeEndedCapture())
    {
        mod::Log(
            "DESYNC_MONITOR: session start skipped because the previous "
            "worker/evidence capture is not finalized");
        return;
    }

    // Keep owned trampolines allocated across sessions. Removing/recreating
    // them from this background start path can race EFZ's main thread after a
    // detour has entered but before it calls the original trampoline.
    const bool type47HooksReady = InstallType47TraceHooks();
    // Snapshot SAVE/LOAD boundary markers ride the same trace ring; they are
    // optional and never gate the type-47 trace itself.
    (void)InstallSnapshotBoundaryHooks();
    (void)InstallRngCallTracerHook();
    (void)InstallRngSeedApplyHook();
    ResetType47TraceForSession(type47HooksReady);

    EnterCriticalSection(&g_lock);
    ResetRingsLocked();
    InterlockedExchange(&g_sessionActive, 1);
    g_dumped = false;
    g_dumpPending = false;
    g_role = (netplayRole == 0) ? 0 : 1;
    g_hostPort = port;
    g_sessionStartTick = GetTickCount();
    if (g_role == 1)
    {
        g_sessionNonce =
            (static_cast<uint32_t>(GetCurrentProcessId()) * 2654435761u)
            ^ g_sessionStartTick
            ^ (static_cast<uint32_t>(port) << 16)
            ^ static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_sessionNonce));
        if (g_sessionNonce == 0)
            g_sessionNonce = 1;
    }
    else
    {
        g_sessionNonce = 0;
    }
    std::snprintf(g_peerAddress, sizeof(g_peerAddress), "%s",
        address != nullptr ? address : "");
    std::snprintf(g_nickname, sizeof(g_nickname), "%s",
        nickname != nullptr ? nickname : "");
    LeaveCriticalSection(&g_lock);

    InterlockedExchange(&g_workerStop, 0);
    if (OpenSideChannel())
    {
        g_workerThread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
        if (g_workerThread == nullptr)
        {
            mod::Log("DESYNC_MONITOR: worker thread creation failed");
            CloseSideChannel();
        }
    }
    mod::Log(
        "DESYNC_MONITOR: session started role=%d port=%u peer='%s' "
        "recorder=waiting_for_peer channel=%s",
        g_role,
        static_cast<unsigned>(g_hostPort),
        g_peerAddress,
        g_workerThread != nullptr ? "on" : "off");
}

void NotifySessionEnded(const char* reason)
{
    if (!g_lockInited)
    {
        SealType47TraceSession();
        (void)DisableType47TraceHooksForIdle();
        return;
    }
    bool wasActive = false;
    bool mustFinalize = false;
    EnterCriticalSection(&g_lock);
    wasActive = InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0;
    if (wasActive)
    {
        // sessionActive prevents new capture; transportDraining keeps the
        // worker alive for its bounded final-sample/trigger exchange.
        InterlockedExchange(&g_transportDraining, 1);
        InterlockedExchange(&g_sessionActive, 0);
    }
    InterlockedExchange(&g_captureArmed, 0);
    if (g_evidenceTriggered && !g_dumped)
    {
        g_dumpPending = true;
    }
    mustFinalize = wasActive || g_dumpPending || g_workerThread != nullptr;
    LeaveCriticalSection(&g_lock);
    // See NotifySessionStarted: checksum admission closes before the trace
    // epoch is sealed so the two evidence streams share one boundary.
    SealType47TraceSession();

    if (mustFinalize)
    {
        if (!TryFinalizeEndedCapture() && g_dumpPending)
        {
            mod::Log(
                "DESYNC_MONITOR: forensic dump pending; retry will occur at "
                "shutdown or before the next traced session");
        }
    }
    (void)DisableType47TraceHooksForIdle();
    if (wasActive)
    {
        mod::Log(
            "DESYNC_MONITOR: session ended reason='%s' samples=%u peerSeen=%d",
            reason != nullptr ? reason : "",
            g_sampleCount,
            g_peerSeen ? 1 : 0);
    }
}

void Shutdown()
{
    NotifySessionEnded("shutdown");
    (void)DisableAndRemoveType47TraceHooks();
}

void EmergencyEvidenceFlush(const char* reason)
{
    const char* reasonTag =
        (reason != nullptr && reason[0] != '\0') ? reason : "emergency";
    if (!g_lockInited)
    {
        return;
    }

    // Cheap idle probe: nothing to save and nothing running means no-op,
    // so terminal exit paths can call this unconditionally.
    const bool sessionActive =
        InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0;
    bool hasLatchedEvidence = false;
    EnterCriticalSection(&g_lock);
    hasLatchedEvidence = (g_evidenceTriggered && !g_dumped) || g_dumpPending;
    LeaveCriticalSection(&g_lock);
    if (!sessionActive && !hasLatchedEvidence)
    {
        return;
    }

    if (!netplay::mod_settings::IsEmergencyEvidenceFlushEnabled())
    {
        // The desync3 capture lost a peer-acknowledged forensic onset this
        // way; make the skip loud so a disabled flag is never mistaken for
        // "there was no evidence".
        mod::Log(
            "DESYNC_MONITOR: emergency evidence flush DISABLED by "
            "ExperimentalEmergencyEvidenceFlush=0 reason='%s' active=%d "
            "latchedEvidence=%d - frozen evidence will be lost at exit",
            reasonTag,
            sessionActive ? 1 : 0,
            hasLatchedEvidence ? 1 : 0);
        return;
    }

    mod::Log(
        "DESYNC_MONITOR: emergency evidence flush begin reason='%s' "
        "active=%d latchedEvidence=%d",
        reasonTag,
        sessionActive ? 1 : 0,
        hasLatchedEvidence ? 1 : 0);

    // The normal teardown already does everything needed, bounded: it closes
    // checksum admission, seals the type-47 epoch, gives the worker its
    // capped final-sample/trigger drain (so the peer can still receive the
    // trigger tuple and dump its own side), stops it, and writes the dump.
    // On failure the pending latch survives for a shutdown/next-start retry.
    NotifySessionEnded(reasonTag);

    bool dumped = false;
    bool stillPending = false;
    EnterCriticalSection(&g_lock);
    dumped = g_dumped;
    stillPending = g_dumpPending && !g_dumped;
    LeaveCriticalSection(&g_lock);
    mod::Log(
        "DESYNC_MONITOR: emergency evidence flush end reason='%s' dumped=%d "
        "pendingRetry=%d",
        reasonTag,
        dumped ? 1 : 0,
        stillPending ? 1 : 0);
}

bool IsSessionTracing()
{
    return g_lockInited
        && InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0;
}

bool IsCaptureArmed()
{
    return IsSessionTracing()
        && InterlockedCompareExchange(&g_captureArmed, 0, 0) != 0;
}

void ObserveFrame(int frame)
{
    if (IsSessionTracing() && frame >= 0)
    {
        InterlockedExchange(&g_latestFrame, frame);
        const LONG captureStart =
            InterlockedCompareExchange(&g_captureStartFrame, -1, -1);
        const bool traceWindowReady =
            g_type47TraceAvailableForSession
            && InterlockedCompareExchange(&g_type47TraceSealed, 0, 0) == 0
            && InterlockedCompareExchange(&g_captureArmed, 0, 0) != 0
            && captureStart >= 0
            && frame >= captureStart - 16;
        InterlockedExchange(
            &g_type47TraceEnabled,
            traceWindowReady ? 1 : 0);
    }
    else
    {
        InterlockedExchange(&g_type47TraceEnabled, 0);
    }
}

void RecordFrameTick(uintptr_t sessionPtr, int frame, int commitFrame)
{
    if (!IsCaptureArmed() || sessionPtr == 0 || frame < 0)
    {
        return;
    }
    InterlockedExchange(&g_latestFrame, frame);
    // Battle screen only - menus don't carry sync-relevant state. This
    // experimental capture is intentionally single-battle: re-entering the
    // battle screen requires a fresh peer handshake rather than letting two
    // locally observed transitions invent different wire identities.
    uint8_t screen = 0xFF;
    if (!GuardedRead(
            reinterpret_cast<const void*>(kScreenIndexAddr), &screen)
        || screen != 3)
    {
        if (g_inBattle)
        {
            EnterCriticalSection(&g_lock);
            g_leftBattleSinceLastSample = true;
            InterlockedExchange(&g_captureArmed, 0);
            InterlockedExchange(&g_type47TraceEnabled, 0);
            g_handshakePhase = 4;
            if (g_evidenceTriggered)
            {
                mod::Log(
                    "DESYNC_MONITOR: battle ended after forensic trigger; "
                    "capture sealed until session teardown");
            }
            else
            {
                mod::Log(
                    "DESYNC_MONITOR: battle ended before a comparable "
                    "difference; tracer closed for this session");
            }
            LeaveCriticalSection(&g_lock);
        }
        g_inBattle = false;
        return;
    }

    // Boundary handling above must run even if a transition reset the frame
    // or commit cursor. Only actual battle samples are gated by the negotiated
    // start and the bounded post-tick cursor relationship.
    const LONG captureStart =
        InterlockedCompareExchange(&g_captureStartFrame, -1, -1);
    if (InterlockedCompareExchange(&g_captureArmed, 0, 0) == 0
        || captureStart < 0
        || frame < captureStart)
    {
        return;
    }
    // gmBase+16 is a commit cursor, not uniformly an inclusive committed-
    // frame number: observed Revival builds normally report cursor==frame+1,
    // while transition/rollback phases may expose frame or frame-1. These
    // samples are attribution evidence, not proof of complete synchronized
    // state at the native snapshot boundary.
    const int commitCursorDelta = commitFrame - frame;
    if (commitFrame < 0 || commitCursorDelta < -1 || commitCursorDelta > 1)
    {
        return;
    }

    // One logical frame is sampled once even if the outer hook is called
    // repeatedly during a zero-simulation batch. A same-screen regression is
    // a rollback/resimulation boundary, not a peer-symmetric battle identity:
    // abort the optional tracer instead of advancing one peer's wire epoch.
    const bool newBattle = !g_inBattle;
    const bool frameRegression =
        g_inBattle && g_lastSampledFrame >= 0 && frame < g_lastSampledFrame;
    if (!newBattle && !frameRegression && frame == g_lastSampledFrame)
    {
        return;
    }
    g_inBattle = true;
    if (frameRegression)
    {
        EnterCriticalSection(&g_lock);
        if (InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0)
        {
            InterlockedExchange(&g_captureArmed, 0);
            InterlockedExchange(&g_type47TraceEnabled, 0);
            if (g_evidenceTriggered)
            {
                g_handshakePhase = 4;
                mod::Log(
                    "DESYNC_MONITOR: capture sealed at same-screen frame "
                    "regression %d -> %d to preserve forensic onset "
                    "epoch=%ld frame=%d",
                    g_lastSampledFrame,
                    frame,
                    static_cast<long>(g_evidenceBattleEpoch),
                    g_evidenceTriggerFrame);
            }
            else
            {
                g_handshakePhase = 4;
                mod::Log(
                    "DESYNC_MONITOR: tracer aborted at same-screen frame "
                    "regression %d -> %d; capture epoch and rings preserved",
                    g_lastSampledFrame,
                    frame);
            }
        }
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (newBattle)
    {
        bool abortCapture = false;
        EnterCriticalSection(&g_lock);
        if (InterlockedCompareExchange(&g_sessionActive, 0, 0) != 0)
        {
            if (g_leftBattleSinceLastSample)
            {
                abortCapture = true;
                InterlockedExchange(&g_captureArmed, 0);
                InterlockedExchange(&g_type47TraceEnabled, 0);
                if (g_evidenceTriggered)
                {
                    g_handshakePhase = 4;
                    mod::Log(
                        "DESYNC_MONITOR: capture sealed before battle re-entry "
                        "to preserve forensic onset epoch=%ld frame=%d",
                        static_cast<long>(g_evidenceBattleEpoch),
                        g_evidenceTriggerFrame);
                }
                else
                {
                    g_handshakePhase = 4;
                    mod::Log(
                        "DESYNC_MONITOR: tracer aborted on battle re-entry; "
                        "a fresh session handshake is required");
                }
            }
            else
            {
                // ResetRingsLocked already established epoch 1 and cleared
                // all capture state before the handshake. Do not clear a
                // faster peer's early samples when this side observes its
                // first battle frame.
                mod::Log(
                    "DESYNC_MONITOR: entered single-battle capture epoch=%ld",
                    static_cast<long>(InterlockedCompareExchange(
                        &g_battleEpoch, 0, 0)));
            }
        }
        LeaveCriticalSection(&g_lock);
        if (abortCapture)
            return;
    }

    const netplay::bridge::takeover::RevivalAddressProfile* profile =
        netplay::bridge::takeover::g_activeRevival;
    if (profile == nullptr)
    {
        return;
    }

    // Resolve region sources.
    uintptr_t battleScreen = 0;
    (void)GuardedRead(
        reinterpret_cast<const void*>(kScreenTableAddr + 4u * 3u), &battleScreen);
    uintptr_t gameSys = 0;
    uintptr_t charP1 = 0;
    uintptr_t charP2 = 0;
    if (battleScreen != 0)
    {
        (void)GuardedRead(
            reinterpret_cast<const void*>(battleScreen + kOffsetGameSystem), &gameSys);
        (void)GuardedRead(
            reinterpret_cast<const void*>(battleScreen + kOffsetBattleP1Char), &charP1);
        (void)GuardedRead(
            reinterpret_cast<const void*>(battleScreen + kOffsetBattleP2Char), &charP2);
    }

    // Copy regions into a stack-independent staging record (static: single
    // writer, the game thread).
    static FrameRecord staging;
    staging.frame = frame;
    staging.validMask = 0;
    staging.srcGameSys = static_cast<uint32_t>(gameSys != 0 ? gameSys + kGameSysSliceOffset : 0);
    staging.srcBattle = static_cast<uint32_t>(battleScreen != 0 ? battleScreen + kBattleSliceOffset : 0);
    staging.srcCharP1 = static_cast<uint32_t>(charP1);
    staging.srcCharP2 = static_cast<uint32_t>(charP2);

    uint8_t* cursor = staging.bytes;
    if (CopyRegion(staging.srcGameSys, cursor, kGameSysSliceSize))
    {
        staging.validMask |= 1;
    }
    cursor += kGameSysSliceSize;
    if (CopyRegion(staging.srcBattle, cursor, kBattleSliceSize))
    {
        staging.validMask |= 2;
    }
    cursor += kBattleSliceSize;
    if (CopyRegion(staging.srcCharP1, cursor, kCharSliceSize))
    {
        staging.validMask |= 4;
    }
    cursor += kCharSliceSize;
    if (CopyRegion(staging.srcCharP2, cursor, kCharSliceSize))
    {
        staging.validMask |= 8;
    }

    // The character objects are the sync-critical payload; without them the
    // checksum would compare mostly-static data and miss real divergence.
    if ((staging.validMask & 12) != 12)
    {
        return;
    }

    // Revival stores these vectors as local/remote, not as canonical P1/P2.
    // On the joiner activePlayer=1, so hashing primary then secondary would
    // reverse the same two inputs and manufacture a peer mismatch whenever
    // the players press different buttons.
    uint16_t primaryInput = 0;
    uint16_t secondaryInput = 0;
    const bool primaryInputOk = ReadHistoryInput(
        sessionPtr,
        profile->sessionOffsetHistoryPrimaryVec,
        frame,
        &primaryInput);
    const bool secondaryInputOk = ReadHistoryInput(
        sessionPtr,
        profile->sessionOffsetHistorySecondaryVec,
        frame,
        &secondaryInput);
    int activePlayer = -1;
    const bool activePlayerOk = GuardedRead(
        reinterpret_cast<const void*>(
            sessionPtr + profile->sessionOffsetActivePlayer),
        &activePlayer);
    if (!primaryInputOk || !secondaryInputOk || !activePlayerOk)
    {
        return;
    }
    if (activePlayer == 1)
    {
        staging.p1Input = secondaryInput;
        staging.p2Input = primaryInput;
    }
    else if (activePlayer == 0)
    {
        staging.p1Input = primaryInput;
        staging.p2Input = secondaryInput;
    }
    else
    {
        return;
    }

    staging.commitFrame = commitFrame;
    staging.syncFeed = -1;
    (void)GuardedRead(
        reinterpret_cast<const void*>(
            sessionPtr + profile->sessionOffsetGameModeSnapshot + 20),
        &staging.syncFeed);
    staging.localLen =
        ReadHistoryLength(sessionPtr, profile->sessionOffsetHistoryPrimaryVec);
    staging.remoteLen =
        ReadHistoryLength(sessionPtr, profile->sessionOffsetHistorySecondaryVec);
    staging.pingMs = -1;
    if (profile->sessionOffsetPingMs != 0)
    {
        (void)GuardedRead(
            reinterpret_cast<const void*>(sessionPtr + profile->sessionOffsetPingMs),
            &staging.pingMs);
    }

    staging.effectHash = ComputeEffectRingHash(gameSys, &staging);
    staging.rngState = ReadRevivalRngState();

    EnterCriticalSection(&g_lock);
    if (InterlockedCompareExchange(&g_sessionActive, 0, 0) == 0)
    {
        LeaveCriticalSection(&g_lock);
        return;
    }

    g_lastSampledFrame = frame;

    if (!g_maskFrozen)
    {
        // Calibration: accumulate the set of bytes that mutate frame to
        // frame.  No checksums are recorded or exchanged until the mask is
        // frozen, but the region ring still records for the dumps.
        if (g_prevRegionValid)
        {
            for (size_t i = 0; i < kRecordRegionBytes; ++i)
            {
                if (staging.bytes[i] != g_prevRegionBytes[i]
                    && !IsRenderOwnedRegionByte(i))
                {
                    g_changeMask[i] = 1;
                }
            }
            ++g_maskSamples;
        }
        std::memcpy(g_prevRegionBytes, staging.bytes, kRecordRegionBytes);
        g_prevRegionValid = true;

        if (g_maskSamples >= kMaskCalibrationSamples)
        {
            g_maskFrozen = true;
            g_maskByteCount = 0;
            for (size_t i = 0; i < kRecordRegionBytes; ++i)
            {
                g_maskByteCount += g_changeMask[i];
            }
            g_maskHash = Fnv1a(
                2166136261u, g_changeMask, sizeof(g_changeMask));
            if (g_maskHash == 0)
                g_maskHash = 1;
            if (g_evidenceTriggered)
            {
                // An effect/RNG trigger may precede gameplay-mask calibration.
                // Publish only the complete frozen mask into the eventual dump.
                SnapshotEvidenceMaskLocked();
            }
            mod::Log(
                "DESYNC_MONITOR: change mask frozen at frame %d "
                "(%u of %u bytes participate, identity=0x%08lX)",
                frame,
                g_maskByteCount,
                static_cast<unsigned>(kRecordRegionBytes),
                static_cast<unsigned long>(g_maskHash));
        }
    }

    uint32_t checksum = 2166136261u;
    if (g_maskFrozen)
    {
        for (size_t i = 0; i < kRecordRegionBytes; ++i)
        {
            if (g_changeMask[i] != 0)
            {
                checksum ^= staging.bytes[i];
                checksum *= 16777619u;
            }
        }
        checksum = Fnv1a(checksum, &staging.p1Input, sizeof(staging.p1Input));
        checksum = Fnv1a(checksum, &staging.p2Input, sizeof(staging.p2Input));
    }
    staging.checksum = checksum;

    if (!g_evidenceTriggered)
    {
        std::memcpy(&g_regionRing[g_regionRingNext], &staging, sizeof(staging));
        g_regionRingNext = (g_regionRingNext + 1) % kRegionRingDepth;
    }
    else if (g_postEvidenceFrameCount < kPostEvidenceDepth)
    {
        std::memcpy(
            &g_postEvidenceFrames[g_postEvidenceFrameCount++],
            &staging,
            sizeof(staging));
        // This fixed array simply stops accepting records when full. Keep the
        // independent type-47 ring tracing until capture/session teardown;
        // stopping here would create a peer-asymmetric 64-frame boundary.
    }

    // Effect/RNG attribution starts at the negotiated frame; it must not wait
    // for the optional selected-gameplay mask. Pre-calibration samples carry
    // a zero mask identity, so gameplay comparison remains disabled while the
    // canonical effect projection and RNG scalar are still compared.
    ChecksumSample& sample = g_localRing[g_localRingNext];
    g_localRingNext = (g_localRingNext + 1) % kSampleRingDepth;
    sample.frame = frame;
    sample.checksum = checksum;
    sample.effectHash = staging.effectHash;
    sample.rngState = staging.rngState;
    sample.maskHash = g_maskFrozen ? g_maskHash : 0;
    sample.maskByteCount = g_maskFrozen
        ? static_cast<uint16_t>(g_maskByteCount)
        : 0;
    if (frame > g_highestLocalFrame)
    {
        g_highestLocalFrame = frame;
    }

    ++g_sampleCount;
    LeaveCriticalSection(&g_lock);
}

} // namespace netplay::bridge::desync_monitor
