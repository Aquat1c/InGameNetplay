// -----------------------------------------------------------------------------
// stock_rollback_probe.cpp  --  rollback observer module (part of
// stock_debug_probe).
//
// Covers BOTH the "batch shape" and "snapshot round-trip" diagnostics by
// hooking Revival's savestate SAVE (sub_1006F390) and LOAD (sub_1006F270):
//
//  * BATCH SHAPE: the sequence of save/load events with their frame/commit
//    is the rollback partition. Diff two stock peers - if their save/load/
//    frame patterns match (same rollback depth and iteration count around the
//    throw), the batch shape is symmetric, which is why stock stays in sync.
//    (The InGameNetplay host showed an asymmetric shape - steady Load-1/iter-2
//    vs the client's oscillating shape - which is the timing perturbation that
//    makes its re-execution non-idempotent.)
//
//  * ROUND-TRIP FIDELITY: small FNV hashes of the sync-relevant character
//    windows + effect-ring occupancy are taken at save ENTRY and load EXIT,
//    alongside each peer's decoded move/anim/position. A load whose hashes
//    equal the save it rewinds to proves the restore is byte-faithful for that
//    state; comparing across peers proves symmetry. (This is where the modded
//    host's re-execution diverged - throw contact resolved on a different anim
//    frame after restore.)
//
// Windows are kept small so the probe does not perturb the timing it measures.
// Observe-only, fail-closed on prologue mismatch. Registers one ProbeModule;
// the shared host (probe_host.cpp) drives it.
//
// Output: stock_rollback_trace_<pid>.csv. Diff across the two stock peers.
// -----------------------------------------------------------------------------

#include "probe_common.h"

#include <MinHook.h>

namespace
{
using namespace probe;

// EfzRevival.dll 1.02h RVAs (byte-verified prologues below).
constexpr uintptr_t kSaveRva = 0x0006F390u; // sub_1006F390 __thiscall(void**, char)
constexpr uintptr_t kLoadRva = 0x0006F270u; // sub_1006F270 __thiscall(void*)
const uint8_t kSavePrologue[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68}; // reloc-safe head
const uint8_t kLoadPrologue[] = {0x55, 0x8B, 0xEC, 0x64, 0xA1};

// Sync-relevant hash windows (small on purpose).
constexpr size_t kCharWindow = 0x400;                 // positions/anim/state/box-ptr
constexpr size_t kEffectFlagsWindow = 4u * kEffectRingSlots; // active-flag array

uintptr_t g_revivalBase = 0;
uintptr_t g_sessionPtrGlobal = 0;
uintptr_t g_saveAddr = 0;
uintptr_t g_loadAddr = 0;

using SaveFn = void(__thiscall*)(void*, char);
using LoadFn = void*(__thiscall*)(void*);
SaveFn g_origSave = nullptr;
LoadFn g_origLoad = nullptr;

LineSink g_sink;
volatile LONG g_seq = 0;
volatile LONG g_saves = 0;
volatile LONG g_loads = 0;
volatile LONG g_lastFrame = -1;

// ---------------------------------------------------------------------------
// FULL RAW DUMP: at every save-entry (once per tick, including rollback
// re-executions) the complete savestate-relevant regions are copied into a
// ring and written to debug_logs\full_dump_<pid>.bin by a worker thread:
//   charP1 0x3448 + charP2 0x3448 + battleContext 0x598 + gameSystem 0x142F0
// (~110 KB/record, ~7 MB/s during battle - EXPECT GB-SCALE FILES). A row per
// record goes to full_dump_index_<pid>.csv (seq,frame,commit,tick_ms,engine,
// substeps,ok_mask,offset) for offline seeking. Diffing two peers' dumps
// byte-for-byte finds ANY divergent field with no sampling assumptions - the
// hunt for the mid-frame 5px carrier needs exactly this.
// Game-thread cost: one guarded ~110 KB memcpy per tick (~20 us); disk I/O is
// worker-only. Ring overflow drops records (counted, reported on console).
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct DumpHeader
{
    uint32_t magic;      // 'EFZD'
    uint32_t seq;
    int32_t frame;
    int32_t commit;
    uint32_t tickMs;
    int32_t engine;      // minstd state at capture
    uint32_t substeps;   // (bc+1400 u16)<<16 | (gameSys+82560 u16)
    uint32_t okMask;     // bit0 charP1, bit1 charP2, bit2 battle, bit3 gameSys
};
#pragma pack(pop)
constexpr uint32_t kDumpMagic = 0x445A4645u; // 'EFZD' little-endian
constexpr size_t kDumpDataSize =
    kCharRegionSize * 2 + kBattleScreenRegionSize + kGameSysRegionSize;
constexpr size_t kDumpRecordSize = sizeof(DumpHeader) + kDumpDataSize;
constexpr uint32_t kDumpSlots = 128; // ~14 MB ring; absorbs disk bursts

uint8_t* g_dumpRing = nullptr;
volatile LONG g_dumpWrite = 0;   // produced records (game thread only)
volatile LONG g_dumpFlushed = 0; // consumed records (worker only)
volatile LONG g_dumpDropped = 0;
volatile LONG g_dumpSeq = 0;
HANDLE g_dumpFile = INVALID_HANDLE_VALUE;
HANDLE g_dumpIndex = INVALID_HANDLE_VALUE;
HANDLE g_dumpWorker = nullptr;
volatile LONG g_dumpStop = 0;
unsigned long long g_dumpBytesWritten = 0;
uintptr_t g_rngEngineAddrDump = 0;

void CaptureFullDump()
{
    if (g_dumpRing == nullptr)
    {
        return;
    }
    uintptr_t bs = 0, gameSys = 0, p1 = 0, p2 = 0;
    if (!ResolveBattle(&bs, &gameSys, &p1, &p2) || p1 == 0 || p2 == 0
        || gameSys == 0)
    {
        return; // not in battle
    }
    const LONG produced = g_dumpWrite;
    if (produced - g_dumpFlushed >= static_cast<LONG>(kDumpSlots))
    {
        InterlockedIncrement(&g_dumpDropped);
        return;
    }
    uint8_t* slot = g_dumpRing
        + (static_cast<uint32_t>(produced) % kDumpSlots) * kDumpRecordSize;
    DumpHeader* h = reinterpret_cast<DumpHeader*>(slot);
    h->magic = kDumpMagic;
    h->seq = static_cast<uint32_t>(InterlockedIncrement(&g_dumpSeq) - 1);
    h->frame = SessionFrame(g_sessionPtrGlobal);
    h->commit = SessionCommit(g_sessionPtrGlobal);
    h->tickMs = TickMs();
    h->engine = g_rngEngineAddrDump ? ReadI32(g_rngEngineAddrDump) : -1;
    h->substeps =
        (static_cast<uint32_t>(ReadU16(bs + kBcSubStepCount)) << 16)
        | ReadU16(gameSys + kGsSubStepCount);
    uint8_t* d = slot + sizeof(DumpHeader);
    uint32_t ok = 0;
    if (GuardedCopy(d, p1, kCharRegionSize)) ok |= 1u;
    d += kCharRegionSize;
    if (GuardedCopy(d, p2, kCharRegionSize)) ok |= 2u;
    d += kCharRegionSize;
    if (GuardedCopy(d, bs, kBattleScreenRegionSize)) ok |= 4u;
    d += kBattleScreenRegionSize;
    if (GuardedCopy(d, gameSys, kGameSysRegionSize)) ok |= 8u;
    h->okMask = ok;
    InterlockedIncrement(&g_dumpWrite); // publish AFTER the slot is complete
}

void DumpFlushOnce()
{
    while (g_dumpFlushed < g_dumpWrite)
    {
        const uint8_t* slot = g_dumpRing
            + (static_cast<uint32_t>(g_dumpFlushed) % kDumpSlots) * kDumpRecordSize;
        const DumpHeader* h = reinterpret_cast<const DumpHeader*>(slot);
        DWORD w = 0;
        if (g_dumpFile != INVALID_HANDLE_VALUE)
        {
            WriteFile(g_dumpFile, slot, static_cast<DWORD>(kDumpRecordSize), &w, nullptr);
        }
        if (g_dumpIndex != INVALID_HANDLE_VALUE)
        {
            char line[160];
            const int n = std::snprintf(
                line, sizeof(line), "%lu,%ld,%ld,%lu,%ld,%08lX,%u,%llu\r\n",
                static_cast<unsigned long>(h->seq), static_cast<long>(h->frame),
                static_cast<long>(h->commit), static_cast<unsigned long>(h->tickMs),
                static_cast<long>(h->engine), static_cast<unsigned long>(h->substeps),
                static_cast<unsigned>(h->okMask), g_dumpBytesWritten);
            DWORD wi = 0;
            if (n > 0) WriteFile(g_dumpIndex, line, static_cast<DWORD>(n), &wi, nullptr);
        }
        g_dumpBytesWritten += kDumpRecordSize;
        InterlockedIncrement(&g_dumpFlushed);
    }
}

DWORD WINAPI DumpWorkerProc(LPVOID)
{
    while (InterlockedCompareExchange(&g_dumpStop, 0, 0) == 0)
    {
        Sleep(100);
        DumpFlushOnce();
    }
    DumpFlushOnce();
    return 0;
}

bool StartFullDump()
{
    if (!ProbeConfigEnabled("full_dump"))
    {
        Breadcrumb("stock_rollback_probe",
                   "full dump DISABLED via debug_probe.ini (bisection)");
        return false;
    }
    g_dumpRing = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, kDumpRecordSize * kDumpSlots, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
    if (g_dumpRing == nullptr)
    {
        Breadcrumb("stock_rollback_probe", "full-dump ring alloc failed; dump off.");
        return false;
    }
    EnsureLogDir();
    char path[MAX_PATH];
    BuildLogPath(path, sizeof(path), "full_dump", "bin");
    g_dumpFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    BuildLogPath(path, sizeof(path), "full_dump_index", "csv");
    g_dumpIndex = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_dumpIndex != INVALID_HANDLE_VALUE)
    {
        const char* hd = "seq,frame,commit,tick_ms,engine,substeps,ok_mask,offset\r\n";
        DWORD w = 0;
        WriteFile(g_dumpIndex, hd, static_cast<DWORD>(std::strlen(hd)), &w, nullptr);
    }
    if (g_dumpFile == INVALID_HANDLE_VALUE)
    {
        Breadcrumb("stock_rollback_probe", "full-dump bin open failed; dump off.");
        return false;
    }
    g_dumpWorker = CreateThread(nullptr, 0, &DumpWorkerProc, nullptr, 0, nullptr);
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "full dump ON: %u bytes/record at every save-entry (expect GB-scale)",
                  static_cast<unsigned>(kDumpRecordSize));
    Breadcrumb("stock_rollback_probe", msg);
    return true;
}

void StopFullDump()
{
    InterlockedExchange(&g_dumpStop, 1);
    if (g_dumpWorker != nullptr)
    {
        WaitForSingleObject(g_dumpWorker, 3000);
        CloseHandle(g_dumpWorker);
        g_dumpWorker = nullptr;
    }
    DumpFlushOnce();
    if (g_dumpFile != INVALID_HANDLE_VALUE) { CloseHandle(g_dumpFile); g_dumpFile = INVALID_HANDLE_VALUE; }
    if (g_dumpIndex != INVALID_HANDLE_VALUE) { CloseHandle(g_dumpIndex); g_dumpIndex = INVALID_HANDLE_VALUE; }
}

// Per-peer sync inputs decoded the way collision/throw resolution reads them:
// move id, anim frame/tick, and world X/Y (raw double bits so no float printing
// distorts the value). The throw contact the modded host resolved a frame apart
// lives in exactly these fields, so recording them at save-entry and load-exit
// shows whether the restore is byte-faithful for the character regions too.
// DECOMP CORRECTION: xBits/yBits now read the REAL world positions (+32/+40,
// the actual spawn-math inputs); the old +216/+224 reads were unused bytes.
// The former "screen pair" (duplicate of world) is repurposed as velocities
// (+48/+56) - with positions they discriminate "position advanced k extra
// sub-steps" at a contact. The phantom "+204/+206 anchors" (unused bytes) are
// replaced by the facing sign (+80), a true spawn input.
struct PeerFields
{
    uint16_t move, anim, tick;
    uint64_t xBits, yBits;
    int32_t facing;
    uint64_t vxBits, vyBits;
};
PeerFields ReadPeer(uintptr_t c)
{
    PeerFields f{0xFFFFu, 0xFFFFu, 0xFFFFu, 0, 0, -999, 0, 0};
    if (c != 0)
    {
        f.move = ReadU16(c + kCharMove);
        f.anim = ReadU16(c + kCharAnimFrame);
        f.tick = ReadU16(c + kCharAnimTick);
        f.xBits = ReadDoubleBits(c + kCharWorldX);
        f.yBits = ReadDoubleBits(c + kCharWorldY);
        f.facing = ReadI32(c + kCharFacing);
        f.vxBits = ReadDoubleBits(c + kCharVelX);
        f.vyBits = ReadDoubleBits(c + kCharVelY);
    }
    return f;
}

void EmitEvent(const char* kind)
{
    // FPU registers before any guarded memory read (mod discipline).
    uint16_t fpuCw = 0;
    uint32_t fpuCsr = 0;
    ReadFpuState(&fpuCw, &fpuCsr);

    uintptr_t bs = 0, gameSys = 0, p1 = 0, p2 = 0;
    ResolveBattle(&bs, &gameSys, &p1, &p2);
    // Narrow windows (fast localization) + FULL savestate-region hashes
    // (round-trip fidelity over everything Revival actually copies).
    const uint32_t hp1 = p1 ? Fnv1a(p1, kCharWindow) : 0;
    const uint32_t hp2 = p2 ? Fnv1a(p2, kCharWindow) : 0;
    const uint32_t heff =
        gameSys ? Fnv1a(gameSys + kEffectActiveFlagBase, kEffectFlagsWindow) : 0;
    const uint32_t hp1Full = p1 ? Fnv1a(p1, kCharRegionSize) : 0;
    const uint32_t hp2Full = p2 ? Fnv1a(p2, kCharRegionSize) : 0;
    const uint32_t hGs = gameSys ? Fnv1a(gameSys, kGameSysRegionSize) : 0;
    const uint32_t hBattle = bs ? Fnv1a(bs, kBattleScreenRegionSize) : 0;
    const uint32_t hEffStatus =
        gameSys ? Fnv1a(gameSys + kEffectStatusBase, 4u * kEffectRingSlots) : 0;

    const uint16_t alloc = gameSys ? ReadU16(gameSys + kEffectAllocCursor) : 0xFFFFu;
    const uint16_t proc = gameSys ? ReadU16(gameSys + kEffectProcCursor) : 0xFFFFu;
    const PeerFields f1 = ReadPeer(p1);
    const PeerFields f2 = ReadPeer(p2);

    // Sim vs savestate char-object identity (a restore that targets a different
    // object than the sim reads = Sync-matches-but-sim-diverges mechanism).
    const uintptr_t saveP1 = bs ? ReadPtr(ReadPtr(bs + kOffsetBattleSaveP1)) : 0;
    const uintptr_t saveP2 = bs ? ReadPtr(ReadPtr(bs + kOffsetBattleSaveP2)) : 0;

    // Render-cadence witnesses: LOCAL cadence only, never sync-compare - they
    // timestamp whether a render intervened between Save and the first re-sim.
    const uint32_t rt04 = bs ? ReadU32(bs + kBsRenderCtr04) : 0;
    const uint32_t rt08 = bs ? ReadU32(bs + kBsRenderCtr08) : 0;
    const uint32_t rt0c = bs ? ReadU32(bs + kBsRenderCtr0C) : 0;
    const uint32_t renderToggle = gameSys ? ReadU32(gameSys + kGsRenderToggle) : 0;

    // Collision/round gates + replay word-stream state.
    const uint8_t gate40 = gameSys ? ReadU8(gameSys + kGsCollGate40) : 0xFF;
    const uint8_t gate44 = gameSys ? ReadU8(gameSys + kGsCollGate44) : 0xFF;
    const uint32_t gate48 = gameSys ? ReadU32(gameSys + kGsCollGate48) : 0;
    const uint8_t roundNo = gameSys ? ReadU8(gameSys + kGsRoundNo) : 0xFF;
    const uint32_t readyMask = gameSys ? ReadU32(gameSys + kGsReadyMask) : 0;
    const uint8_t effects4966 = gameSys ? ReadU8(gameSys + kGsEffectsSetting) : 0xFF;
    const uint32_t norm4972 = gameSys ? ReadU32(gameSys + kGsNormalizer) : 0;
    const uint8_t wsMode = gameSys ? ReadU8(gameSys + kGsWordstreamMode) : 0xFF;
    const uint32_t wsCtr = gameSys ? ReadU32(gameSys + kGsWordstreamCtr) : 0;
    // Sub-step counts (never probed before): the tick's inner loop runs the
    // movement integrator once per sub-step - a cross-peer difference here is
    // a k*velocity position shift at contact with everything else equal.
    const uint32_t substeps =
        (static_cast<uint32_t>(bs ? ReadU16(bs + kBcSubStepCount) : 0xFFFFu) << 16)
        | (gameSys ? ReadU16(gameSys + kGsSubStepCount) : 0xFFFFu);

    const int32_t frame = SessionFrame(g_sessionPtrGlobal);
    const int32_t commit = SessionCommit(g_sessionPtrGlobal);
    const LONG seq = InterlockedIncrement(&g_seq) - 1;
    InterlockedExchange(&g_lastFrame, frame);

    char line[1024];
    const int n = std::snprintf(
        line, sizeof(line),
        "%ld,%s,%ld,%ld,%08lX,%08lX,%08lX,%u,%u,"
        "%u,%u,%u,%016llX,%016llX,%u,%u,%u,%016llX,%016llX,%lu,"
        "%08lX,%08lX,%08lX,%08lX,%08lX,"
        "%ld,%ld,%016llX,%016llX,%016llX,%016llX,"
        "%08lX,%08lX,%08lX,%08lX,"
        "%lu,%lu,%lu,%lu,0x%04X,0x%08lX,"
        "%u,%u,%lu,%u,%lu,%u,%lu,%u,%lu,%08lX\r\n",
        static_cast<long>(seq), kind,
        static_cast<long>(frame), static_cast<long>(commit),
        static_cast<unsigned long>(hp1), static_cast<unsigned long>(hp2),
        static_cast<unsigned long>(heff),
        static_cast<unsigned>(alloc), static_cast<unsigned>(proc),
        static_cast<unsigned>(f1.move), static_cast<unsigned>(f1.anim),
        static_cast<unsigned>(f1.tick),
        static_cast<unsigned long long>(f1.xBits),
        static_cast<unsigned long long>(f1.yBits),
        static_cast<unsigned>(f2.move), static_cast<unsigned>(f2.anim),
        static_cast<unsigned>(f2.tick),
        static_cast<unsigned long long>(f2.xBits),
        static_cast<unsigned long long>(f2.yBits),
        static_cast<unsigned long>(TickMs()),
        static_cast<unsigned long>(hp1Full), static_cast<unsigned long>(hp2Full),
        static_cast<unsigned long>(hGs), static_cast<unsigned long>(hBattle),
        static_cast<unsigned long>(hEffStatus),
        static_cast<long>(f1.facing), static_cast<long>(f2.facing),
        static_cast<unsigned long long>(f1.vxBits),
        static_cast<unsigned long long>(f1.vyBits),
        static_cast<unsigned long long>(f2.vxBits),
        static_cast<unsigned long long>(f2.vyBits),
        static_cast<unsigned long>(p1), static_cast<unsigned long>(p2),
        static_cast<unsigned long>(saveP1), static_cast<unsigned long>(saveP2),
        static_cast<unsigned long>(rt04), static_cast<unsigned long>(rt08),
        static_cast<unsigned long>(rt0c), static_cast<unsigned long>(renderToggle),
        static_cast<unsigned>(fpuCw), static_cast<unsigned long>(fpuCsr),
        static_cast<unsigned>(gate40), static_cast<unsigned>(gate44),
        static_cast<unsigned long>(gate48), static_cast<unsigned>(roundNo),
        static_cast<unsigned long>(readyMask), static_cast<unsigned>(effects4966),
        static_cast<unsigned long>(norm4972), static_cast<unsigned>(wsMode),
        static_cast<unsigned long>(wsCtr), static_cast<unsigned long>(substeps));
    if (n > 0) g_sink.Emit(line);
}

void __fastcall HookSave(void* thisPtr, void* /*edx*/, char mode)
{
    InterlockedIncrement(&g_saves);
    EmitEvent("save");            // state ABOUT to be captured
    CaptureFullDump();            // full raw regions at the same instant
    g_origSave(thisPtr, mode);
}

void* __fastcall HookLoad(void* thisPtr, void* /*edx*/)
{
    void* const result = g_origLoad(thisPtr);
    InterlockedIncrement(&g_loads);
    EmitEvent("load");            // state AS restored
    return result;
}

// --- module interface (driven by probe_host.cpp) -----------------------------
bool Install(uintptr_t revivalBase)
{
    g_revivalBase = revivalBase;
    g_sessionPtrGlobal = revivalBase + kSessionPtrGlobalRva;
    g_rngEngineAddrDump = revivalBase + kRngEngineStateRva;
    g_saveAddr = revivalBase + kSaveRva;
    g_loadAddr = revivalBase + kLoadRva;

    if (!BytesMatch(g_saveAddr, kSavePrologue, sizeof(kSavePrologue))
        || !BytesMatch(g_loadAddr, kLoadPrologue, sizeof(kLoadPrologue)))
    {
        Breadcrumb("stock_rollback_probe",
                   "save/load prologue mismatch (not 1.02h?); idle, fail-closed.");
        return false;
    }

    const MH_STATUS cs = MH_CreateHook(reinterpret_cast<void*>(g_saveAddr),
        reinterpret_cast<void*>(&HookSave), reinterpret_cast<void**>(&g_origSave));
    const MH_STATUS cl = MH_CreateHook(reinterpret_cast<void*>(g_loadAddr),
        reinterpret_cast<void*>(&HookLoad), reinterpret_cast<void**>(&g_origLoad));
    if (cs != MH_OK || cl != MH_OK
        || MH_EnableHook(reinterpret_cast<void*>(g_saveAddr)) != MH_OK
        || MH_EnableHook(reinterpret_cast<void*>(g_loadAddr)) != MH_OK)
    {
        Breadcrumb("stock_rollback_probe", "hook install failed.");
        return false;
    }

    if (!g_sink.Open("stock_rollback_trace"))
    {
        Breadcrumb("stock_rollback_probe", "CSV open failed.");
        return false;
    }
    g_sink.Header(
        "seq,kind,frame,commit,charP1_fnv,charP2_fnv,effflags_fnv,"
        "alloc_cursor,proc_cursor,"
        "p1_move,p1_anim,p1_tick,p1_x_bits,p1_y_bits,"
        "p2_move,p2_anim,p2_tick,p2_x_bits,p2_y_bits,tick_ms,"
        "charP1_full_fnv,charP2_full_fnv,gamesys_fnv,battle_fnv,effstatus_fnv,"
        "p1_facing,p2_facing,p1_vx_bits,p1_vy_bits,p2_vx_bits,p2_vy_bits,"
        "sim_p1,sim_p2,save_p1,save_p2,"
        "rt04,rt08,rt0c,render_toggle,fpu_x87cw,fpu_mxcsr,"
        "gate40,gate44,gate48,round_no,ready_mask,effects_4966,norm_4972,"
        "ws_mode,ws_ctr,substeps\r\n");
    (void)StartFullDump();
    char msg[128];
    std::snprintf(msg, sizeof(msg),
                  "installed. base=0x%08lX save=0x%08lX load=0x%08lX",
                  static_cast<unsigned long>(g_revivalBase),
                  static_cast<unsigned long>(g_saveAddr),
                  static_cast<unsigned long>(g_loadAddr));
    Breadcrumb("stock_rollback_probe", msg);
    return true;
}

void Shutdown()
{
    StopFullDump();
    g_sink.Stop();
}

int Status(char* buf, size_t cap)
{
    return std::snprintf(
        buf, cap, "rb save=%ld frame=%ld dump=%ldMB drop=%ld",
        static_cast<long>(InterlockedCompareExchange(&g_saves, 0, 0)),
        static_cast<long>(InterlockedCompareExchange(&g_lastFrame, 0, 0)),
        static_cast<long>(g_dumpBytesWritten >> 20),
        static_cast<long>(InterlockedCompareExchange(&g_dumpDropped, 0, 0)));
}

ProbeRegistrar g_registrar("stock_rollback_probe", &Install, &Shutdown, &Status);

} // namespace
