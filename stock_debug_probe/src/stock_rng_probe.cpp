// -----------------------------------------------------------------------------
// stock_rng_probe.cpp  --  RNG-draw observer module (part of stock_debug_probe).
//
// Hooks Revival's rand() replacement sub_1006E1A0 (every logical game draw) and
// EFZ's per-effect processor sub_401C20 (to tag the current effect slot). For
// each draw it records the efz.exe callsite, the minstd engine before/after (+
// how many internal steps the adapter consumed), and - for effect draws - the
// slot's behavior/anim/position/velocity. Run on BOTH stock peers and diff the
// stock_rng_trace_<pid>.csv files: if stock is deterministic/symmetric the
// streams are identical (no extra 0x00404911 bounce, matching type47 spawn).
//
// Observe-only, fail-closed on prologue mismatch, no game-thread disk I/O. This
// TU registers one ProbeModule; the shared host (probe_host.cpp) drives it.
// Output: stock_rng_trace_<pid>.csv ; breadcrumb: stock_rng_probe_<pid>.log.
// -----------------------------------------------------------------------------

#include "probe_common.h"

#include <MinHook.h>

#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

namespace
{
using namespace probe;

constexpr uintptr_t kRngReplacementRva = 0x0006E1A0u;
constexpr uintptr_t kProcessEffectAddr = 0x00401C20u;
const uint8_t kRngPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08};

struct DrawRecord
{
    uint32_t seq;
    int32_t frame;
    int32_t commit;
    uint8_t resim;
    uint32_t returnAddr;
    int32_t stateBefore;
    int32_t stateAfter;
    int32_t internalAdvances;
    uint16_t slot;
    uint16_t behavior;
    uint16_t animFrame;
    uint16_t animTick;
    uint16_t allocCursor;
    uint16_t procCursor;
    uint64_t xPosBits;
    uint64_t yPosBits;
    uint64_t xVelBits;
    uint64_t yVelBits;
    uint32_t tickMs;
    // Ported from the mod's instrumentation:
    uint32_t threadId;     // async rand pollutant detector (must be constant)
    uint16_t fpuX87Cw;     // register read at draw time (healthy: 0x027F)
    uint32_t fpuMxcsr;     // register read (healthy: 0x1FA0)
    uint8_t wsMode;        // gameSys+82563 replay word-stream selector
    uint32_t wsCtr;        // gameSys+82576 recorded-stream reader counter
    uint32_t effStatus;    // gameSys+7556+4*slot status DWORD
    uint16_t effDir;       // record+76 direction/branch word
    uint16_t p1Move, p1Anim, p1Tick, p1Contact;  // attacker/victim vintage at
    uint16_t p2Move, p2Anim, p2Tick, p2Contact;  //   the draw (contact 3=throw)
};

// 512K records (~64 MB ring) - about 3.5 h of continuous battle at the volume
// the first test run measured, without wrapping.
constexpr uint32_t kRingCap = 1u << 19;
DrawRecord* g_ring = nullptr;
volatile LONG g_writeIndex = 0;
volatile LONG g_flushIndex = 0;
volatile LONG g_maxFrame = -1;

uintptr_t g_revivalBase = 0;
uintptr_t g_rngEngineAddr = 0;
uintptr_t g_sessionPtrGlobal = 0;
uintptr_t g_rngAddr = 0;

using RngFn = char*(__cdecl*)();
RngFn g_origRng = nullptr;
using ProcessEffectFn = int(__fastcall*)(int, void*, int16_t);
ProcessEffectFn g_origProcessEffect = nullptr;

volatile LONG g_curGameSys = 0;
volatile LONG g_curSlot = 0xFFFF;
volatile LONG g_lastT47Tick = -600000; // last type47 console event (rate limit)

HANDLE g_csv = INVALID_HANDLE_VALUE;
HANDLE g_flushThread = nullptr;
volatile LONG g_stop = 0;

char* __cdecl HookRng()
{
    const uint32_t retAddr = reinterpret_cast<uint32_t>(_ReturnAddress());
    // FPU registers FIRST, before any guarded memory read, so a faulting read
    // can't lose them (mod discipline).
    uint16_t fpuCw = 0;
    uint32_t fpuCsr = 0;
    ReadFpuState(&fpuCw, &fpuCsr);
    const int32_t before = ReadI32(g_rngEngineAddr);
    char* const result = g_origRng();
    const int32_t after = ReadI32(g_rngEngineAddr);

    const uint16_t slot =
        static_cast<uint16_t>(InterlockedCompareExchange(&g_curSlot, 0xFFFF, 0xFFFF));
    const uintptr_t gs =
        static_cast<uintptr_t>(InterlockedCompareExchange(&g_curGameSys, 0, 0));

    uint16_t behavior = 0xFFFF, animFrame = 0xFFFF, animTick = 0xFFFF;
    uint16_t alloc = 0xFFFF, proc = 0xFFFF;
    uint64_t xp = 0, yp = 0, xv = 0, yv = 0;
    uint8_t wsMode = 0xFF;
    uint32_t wsCtr = 0xFFFFFFFF, effStatus = 0xFFFFFFFF;
    uint16_t effDir = 0xFFFF;
    if (gs != 0)
    {
        alloc = ReadU16(gs + kEffectAllocCursor);
        proc = ReadU16(gs + kEffectProcCursor);
        wsMode = ReadU8(gs + kGsWordstreamMode);
        wsCtr = ReadU32(gs + kGsWordstreamCtr);
        if (slot < kEffectRingSlots)
        {
            const uintptr_t rec = gs + kEffectRecordBase + kEffectRecordStride * slot;
            behavior = ReadU16(rec + kEffBehavior);
            animFrame = ReadU16(rec + kEffAnimFrame);
            animTick = ReadU16(rec + kEffAnimTick);
            xp = ReadDoubleBits(rec + kEffPosX);
            yp = ReadDoubleBits(rec + kEffPosY);
            xv = ReadDoubleBits(rec + kEffVelX);
            yv = ReadDoubleBits(rec + kEffVelY);
            effDir = ReadU16(rec + kEffDirection);
            effStatus = ReadU32(gs + kEffectStatusBase + 4u * slot);
        }
    }

    // Both chars' vintage at the draw - the exact fields throw contact and the
    // spawn midpoint resolve on.
    uint16_t p1m = 0xFFFF, p1a = 0xFFFF, p1t = 0xFFFF, p1c = 0xFFFF;
    uint16_t p2m = 0xFFFF, p2a = 0xFFFF, p2t = 0xFFFF, p2c = 0xFFFF;
    uintptr_t p1 = 0, p2 = 0;
    if (ResolveBattle(nullptr, nullptr, &p1, &p2))
    {
        if (p1 != 0)
        {
            p1m = ReadU16(p1 + kCharMove);
            p1a = ReadU16(p1 + kCharAnimFrame);
            p1t = ReadU16(p1 + kCharAnimTick);
            p1c = ReadU16(p1 + kCharContact);
        }
        if (p2 != 0)
        {
            p2m = ReadU16(p2 + kCharMove);
            p2a = ReadU16(p2 + kCharAnimFrame);
            p2t = ReadU16(p2 + kCharAnimTick);
            p2c = ReadU16(p2 + kCharContact);
        }
    }

    const int32_t frame = SessionFrame(g_sessionPtrGlobal);
    uint8_t resim = 0;
    if (frame >= 0)
    {
        const LONG maxSeen = InterlockedCompareExchange(&g_maxFrame, 0, 0);
        resim = (frame < maxSeen) ? 1 : 0;
        if (frame > maxSeen) InterlockedExchange(&g_maxFrame, frame);
    }

    const LONG idx = InterlockedIncrement(&g_writeIndex) - 1;
    if (idx >= 0 && static_cast<uint32_t>(idx) < kRingCap && g_ring != nullptr)
    {
        DrawRecord& r = g_ring[idx];
        r.seq = static_cast<uint32_t>(idx);
        r.frame = frame;
        r.commit = SessionCommit(g_sessionPtrGlobal);
        r.resim = resim;
        r.returnAddr = retAddr;
        r.stateBefore = before;
        r.stateAfter = after;
        r.internalAdvances = MinstdAdvances(before, after, 64);
        r.slot = slot;
        r.behavior = behavior;
        r.animFrame = animFrame;
        r.animTick = animTick;
        r.allocCursor = alloc;
        r.procCursor = proc;
        r.xPosBits = xp;
        r.yPosBits = yp;
        r.xVelBits = xv;
        r.yVelBits = yv;
        r.tickMs = TickMs();
        r.threadId = GetCurrentThreadId();
        r.fpuX87Cw = fpuCw;
        r.fpuMxcsr = fpuCsr;
        r.wsMode = wsMode;
        r.wsCtr = wsCtr;
        r.effStatus = effStatus;
        r.effDir = effDir;
        r.p1Move = p1m; r.p1Anim = p1a; r.p1Tick = p1t; r.p1Contact = p1c;
        r.p2Move = p2m; r.p2Anim = p2a; r.p2Tick = p2t; r.p2Contact = p2c;
    }

    // Type47 is THE desync-critical particle - surface the START of each burst
    // live on the console (one event per burst; bursts separated by >5 s).
    if (behavior == 47)
    {
        const LONG now = static_cast<LONG>(TickMs());
        const LONG last = InterlockedExchange(&g_lastT47Tick, now);
        if (now - last > 5000)
        {
            char ev[96];
            std::snprintf(ev, sizeof(ev),
                          "type47 activity: slot %u frame %ld (watch this window)",
                          static_cast<unsigned>(slot), static_cast<long>(frame));
            PushEvent("stock_rng_probe", ev);
        }
    }
    return result;
}

int __fastcall HookProcessEffect(int gameSys, void* /*edx*/, int16_t slot)
{
    InterlockedExchange(&g_curGameSys, static_cast<LONG>(gameSys));
    InterlockedExchange(&g_curSlot, static_cast<LONG>(static_cast<uint16_t>(slot)));
    const int result = g_origProcessEffect(gameSys, nullptr, slot);
    InterlockedExchange(&g_curSlot, 0xFFFF);
    return result;
}

void FlushNewRecords()
{
    if (g_csv == INVALID_HANDLE_VALUE) return;
    const LONG end = InterlockedCompareExchange(&g_writeIndex, 0, 0);
    LONG i = InterlockedCompareExchange(&g_flushIndex, 0, 0);
    char line[640];
    for (; i < end && static_cast<uint32_t>(i) < kRingCap; ++i)
    {
        const DrawRecord& r = g_ring[i];
        const int n = std::snprintf(
            line, sizeof(line),
            "%lu,%ld,%ld,%u,0x%08lX,%ld,%ld,%d,%u,%u,%u,%u,%u,%u,"
            "%016llX,%016llX,%016llX,%016llX,%lu,"
            "%lu,0x%04X,0x%08lX,%u,%lu,0x%08lX,%u,"
            "%u,%u,%u,%u,%u,%u,%u,%u\r\n",
            static_cast<unsigned long>(r.seq), static_cast<long>(r.frame),
            static_cast<long>(r.commit), static_cast<unsigned>(r.resim),
            static_cast<unsigned long>(r.returnAddr),
            static_cast<long>(r.stateBefore), static_cast<long>(r.stateAfter),
            r.internalAdvances, static_cast<unsigned>(r.slot),
            static_cast<unsigned>(r.behavior), static_cast<unsigned>(r.animFrame),
            static_cast<unsigned>(r.animTick), static_cast<unsigned>(r.allocCursor),
            static_cast<unsigned>(r.procCursor),
            static_cast<unsigned long long>(r.xPosBits),
            static_cast<unsigned long long>(r.yPosBits),
            static_cast<unsigned long long>(r.xVelBits),
            static_cast<unsigned long long>(r.yVelBits),
            static_cast<unsigned long>(r.tickMs),
            static_cast<unsigned long>(r.threadId),
            static_cast<unsigned>(r.fpuX87Cw),
            static_cast<unsigned long>(r.fpuMxcsr),
            static_cast<unsigned>(r.wsMode), static_cast<unsigned long>(r.wsCtr),
            static_cast<unsigned long>(r.effStatus), static_cast<unsigned>(r.effDir),
            static_cast<unsigned>(r.p1Move), static_cast<unsigned>(r.p1Anim),
            static_cast<unsigned>(r.p1Tick), static_cast<unsigned>(r.p1Contact),
            static_cast<unsigned>(r.p2Move), static_cast<unsigned>(r.p2Anim),
            static_cast<unsigned>(r.p2Tick), static_cast<unsigned>(r.p2Contact));
        if (n > 0) { DWORD w = 0; WriteFile(g_csv, line, static_cast<DWORD>(n), &w, nullptr); }
    }
    InterlockedExchange(&g_flushIndex, i);
    FlushFileBuffers(g_csv);
}

DWORD WINAPI FlushThreadProc(LPVOID)
{
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0) { Sleep(500); FlushNewRecords(); }
    FlushNewRecords();
    return 0;
}

void OpenCsv()
{
    EnsureLogDir();
    char path[MAX_PATH];
    BuildLogPath(path, sizeof(path), "stock_rng_trace", "csv");
    g_csv = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_csv != INVALID_HANDLE_VALUE)
    {
        const char* h =
            "seq,frame,commit,resim,return_addr,state_before,state_after,"
            "internal_advances,slot,behavior,anim_frame,anim_tick,"
            "alloc_cursor,proc_cursor,xpos_bits,ypos_bits,xvel_bits,yvel_bits,"
            "tick_ms,thread_id,fpu_x87cw,fpu_mxcsr,ws_mode,ws_ctr,"
            "eff_status,eff_dir,p1_move,p1_anim,p1_tick,p1_contact,"
            "p2_move,p2_anim,p2_tick,p2_contact\r\n";
        DWORD w = 0;
        WriteFile(g_csv, h, static_cast<DWORD>(std::strlen(h)), &w, nullptr);
    }
}

// --- module interface (driven by probe_host.cpp) -----------------------------
bool Install(uintptr_t revivalBase)
{
    g_revivalBase = revivalBase;
    g_rngAddr = revivalBase + kRngReplacementRva;
    g_rngEngineAddr = revivalBase + kRngEngineStateRva;
    g_sessionPtrGlobal = revivalBase + kSessionPtrGlobalRva;

    if (!BytesMatch(g_rngAddr, kRngPrologue, sizeof(kRngPrologue)))
    {
        Breadcrumb("stock_rng_probe",
                   "rand-replacement prologue mismatch (not 1.02h?); idle.");
        return false;
    }
    g_ring = static_cast<DrawRecord*>(VirtualAlloc(
        nullptr, sizeof(DrawRecord) * kRingCap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (g_ring == nullptr) { Breadcrumb("stock_rng_probe", "ring alloc failed."); return false; }

    const MH_STATUS c1 = MH_CreateHook(reinterpret_cast<void*>(g_rngAddr),
        reinterpret_cast<void*>(&HookRng), reinterpret_cast<void**>(&g_origRng));
    const MH_STATUS c2 = MH_CreateHook(reinterpret_cast<void*>(kProcessEffectAddr),
        reinterpret_cast<void*>(&HookProcessEffect), reinterpret_cast<void**>(&g_origProcessEffect));
    if (c1 != MH_OK || c2 != MH_OK
        || MH_EnableHook(reinterpret_cast<void*>(g_rngAddr)) != MH_OK
        || MH_EnableHook(reinterpret_cast<void*>(kProcessEffectAddr)) != MH_OK)
    {
        Breadcrumb("stock_rng_probe", "hook install failed.");
        return false;
    }

    OpenCsv();
    g_flushThread = CreateThread(nullptr, 0, &FlushThreadProc, nullptr, 0, nullptr);
    char msg[128];
    std::snprintf(msg, sizeof(msg), "installed. base=0x%08lX rand=0x%08lX engine=0x%08lX",
                  static_cast<unsigned long>(g_revivalBase),
                  static_cast<unsigned long>(g_rngAddr),
                  static_cast<unsigned long>(g_rngEngineAddr));
    Breadcrumb("stock_rng_probe", msg);
    return true;
}

void Shutdown()
{
    InterlockedExchange(&g_stop, 1);
    if (g_flushThread != nullptr)
    {
        WaitForSingleObject(g_flushThread, 2000);
        CloseHandle(g_flushThread);
        g_flushThread = nullptr;
    }
    FlushNewRecords();
    if (g_csv != INVALID_HANDLE_VALUE) { CloseHandle(g_csv); g_csv = INVALID_HANDLE_VALUE; }
}

int Status(char* buf, size_t cap)
{
    const LONG rows = InterlockedCompareExchange(&g_writeIndex, 0, 0);
    const LONG frame = InterlockedCompareExchange(&g_maxFrame, 0, 0);
    return std::snprintf(buf, cap, "rng rows=%ld frame=%ld",
                         static_cast<long>(rows), static_cast<long>(frame));
}

ProbeRegistrar g_registrar("stock_rng_probe", &Install, &Shutdown, &Status);

} // namespace
