// -----------------------------------------------------------------------------
// stock_batch_probe.cpp  --  rollback-batch-shape observer module (part of
// stock_debug_probe).
//
// Hooks Revival's rollback batch dispatcher sub_10072D30 (the function that
// saves current state, rewinds to a confirmed frame, and re-simulates forward
// once per outer tick). For each outer batch it logs the session frame/commit
// at ENTRY and EXIT plus the effect-ring cursors and per-peer position/move, so
// the per-tick advance, re-simulation depth, and net effect of each batch are
// visible at the coarse (outer) level - complementing stock_rollback_probe's
// per-save/load view.
//
// The point: diff two stock peers' batch streams. If stock keeps a SYMMETRIC
// batch shape (same enter/exit frames, advances, and cursor/position deltas
// around the throw), that is why it stays in sync. InGameNetplay's host showed
// an ASYMMETRIC shape (the desync3 steady Load-1/iter-2 vs the client's
// oscillating shape) - the timing perturbation that makes its re-execution
// non-idempotent.
//
// Observe-only, fail-closed on a prologue mismatch. Registers one ProbeModule;
// the shared host (probe_host.cpp) drives it.
//
// Output: stock_batch_trace_<pid>.csv.
// -----------------------------------------------------------------------------

#include "probe_common.h"

#include <MinHook.h>

namespace
{
using namespace probe;

// EfzRevival.dll 1.02h: char __thiscall sub_10072D30(int* this, int a2).
constexpr uintptr_t kBatchRva = 0x00072D30u;
const uint8_t kBatchPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x53, 0x56};

uintptr_t g_revivalBase = 0;
uintptr_t g_sessionPtrGlobal = 0;
uintptr_t g_batchAddr = 0;

using BatchFn = char(__thiscall*)(void*, int);
BatchFn g_origBatch = nullptr;

LineSink g_sink;
volatile LONG g_seq = 0;
volatile LONG g_lastFrame = -1;
volatile LONG g_lastSpikeTick = -600000; // rate limit for dur_ms spike events

// Coarse per-batch context: effect-ring cursors + each peer's world Y (raw
// double bits) and move id. Sampled at ENTRY and EXIT so a batch's net effect -
// how many effect draws it added and whether a position/move shifted across the
// re-sim - is visible without the finer save/load view. The type47 spawn shift
// that seeds the desync appears here as an alloc-cursor / p*_y delta that
// differs between the two peers.
struct BatchCtx
{
    uint16_t alloc, proc;
    uint16_t p1Move, p2Move;
    uint64_t p1yBits, p2yBits;
    uint32_t renderCtr;     // battleScreen+4 rendered counter (local cadence)
    uint32_t renderToggle;  // gameSys+4968 frame-parity toggle (local cadence)
    uint16_t fpuCw;         // x87 control word (batch-boundary normalization check)
};
BatchCtx ReadCtx()
{
    BatchCtx c{0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0, 0, 0, 0, 0};
    uint32_t mxcsrIgnored = 0;
    ReadFpuState(&c.fpuCw, &mxcsrIgnored);
    uintptr_t bs = 0, gameSys = 0, p1 = 0, p2 = 0;
    ResolveBattle(&bs, &gameSys, &p1, &p2);
    if (gameSys)
    {
        c.alloc = ReadU16(gameSys + kEffectAllocCursor);
        c.proc = ReadU16(gameSys + kEffectProcCursor);
        c.renderToggle = ReadU32(gameSys + kGsRenderToggle);
    }
    if (bs) c.renderCtr = ReadU32(bs + kBsRenderCtr04);
    if (p1) { c.p1Move = ReadU16(p1 + kCharMove); c.p1yBits = ReadDoubleBits(p1 + kCharWorldY); }
    if (p2) { c.p2Move = ReadU16(p2 + kCharMove); c.p2yBits = ReadDoubleBits(p2 + kCharWorldY); }
    return c;
}

char __fastcall HookBatch(void* thisPtr, void* /*edx*/, int a2)
{
    const uint32_t tickEnter = TickMs();
    const int32_t frameEnter = SessionFrame(g_sessionPtrGlobal);
    const int32_t commitEnter = SessionCommit(g_sessionPtrGlobal);
    const BatchCtx ctxEnter = ReadCtx();
    const char result = g_origBatch(thisPtr, a2);
    const int32_t frameExit = SessionFrame(g_sessionPtrGlobal);
    const int32_t commitExit = SessionCommit(g_sessionPtrGlobal);
    const BatchCtx ctxExit = ReadCtx();
    const uint32_t durMs = TickMs() - tickEnter;

    const LONG seq = InterlockedIncrement(&g_seq) - 1;
    InterlockedExchange(&g_lastFrame, frameExit);
    char line[512];
    const int n = std::snprintf(
        line, sizeof(line),
        "%ld,0x%08lX,%ld,%ld,%ld,%ld,%ld,%d,"
        "%u,%u,%u,%u,%u,%u,%016llX,%016llX,%u,%u,%016llX,%016llX,%lu,%lu,"
        "%lu,%lu,%lu,%lu,0x%04X,0x%04X\r\n",
        static_cast<long>(seq), static_cast<unsigned long>(a2),
        static_cast<long>(frameEnter), static_cast<long>(commitEnter),
        static_cast<long>(frameExit), static_cast<long>(commitExit),
        static_cast<long>(frameExit - frameEnter),
        static_cast<int>(static_cast<unsigned char>(result)),
        static_cast<unsigned>(ctxEnter.alloc), static_cast<unsigned>(ctxExit.alloc),
        static_cast<unsigned>(ctxEnter.proc), static_cast<unsigned>(ctxExit.proc),
        static_cast<unsigned>(ctxEnter.p1Move), static_cast<unsigned>(ctxExit.p1Move),
        static_cast<unsigned long long>(ctxEnter.p1yBits),
        static_cast<unsigned long long>(ctxExit.p1yBits),
        static_cast<unsigned>(ctxEnter.p2Move), static_cast<unsigned>(ctxExit.p2Move),
        static_cast<unsigned long long>(ctxEnter.p2yBits),
        static_cast<unsigned long long>(ctxExit.p2yBits),
        static_cast<unsigned long>(tickEnter), static_cast<unsigned long>(durMs),
        static_cast<unsigned long>(ctxEnter.renderCtr),
        static_cast<unsigned long>(ctxExit.renderCtr),
        static_cast<unsigned long>(ctxEnter.renderToggle),
        static_cast<unsigned long>(ctxExit.renderToggle),
        static_cast<unsigned>(ctxEnter.fpuCw), static_cast<unsigned>(ctxExit.fpuCw));
    if (n > 0) g_sink.Emit(line);

    // A batch that took abnormally long is exactly the timing perturbation this
    // investigation chases - surface it live (rate-limited to one per 5 s).
    if (durMs > 50)
    {
        const LONG now = static_cast<LONG>(TickMs());
        const LONG last = InterlockedCompareExchange(&g_lastSpikeTick, 0, 0);
        if (now - last > 5000)
        {
            InterlockedExchange(&g_lastSpikeTick, now);
            char ev[96];
            std::snprintf(ev, sizeof(ev), "batch dur spike: %lu ms at frame %ld",
                          static_cast<unsigned long>(durMs),
                          static_cast<long>(frameExit));
            PushEvent("stock_batch_probe", ev);
        }
    }
    return result;
}

// --- module interface (driven by probe_host.cpp) -----------------------------
bool Install(uintptr_t revivalBase)
{
    g_revivalBase = revivalBase;
    g_sessionPtrGlobal = revivalBase + kSessionPtrGlobalRva;
    g_batchAddr = revivalBase + kBatchRva;

    if (!BytesMatch(g_batchAddr, kBatchPrologue, sizeof(kBatchPrologue)))
    {
        Breadcrumb("stock_batch_probe",
                   "dispatcher prologue mismatch (not 1.02h?); idle, fail-closed.");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(g_batchAddr),
            reinterpret_cast<void*>(&HookBatch),
            reinterpret_cast<void**>(&g_origBatch)) != MH_OK
        || MH_EnableHook(reinterpret_cast<void*>(g_batchAddr)) != MH_OK)
    {
        Breadcrumb("stock_batch_probe", "hook install failed.");
        return false;
    }
    if (!g_sink.Open("stock_batch_trace"))
    {
        Breadcrumb("stock_batch_probe", "CSV open failed.");
        return false;
    }
    g_sink.Header(
        "seq,a2_arg,frame_enter,commit_enter,frame_exit,commit_exit,advance,result,"
        "alloc_enter,alloc_exit,proc_enter,proc_exit,"
        "p1_move_enter,p1_move_exit,p1_y_enter,p1_y_exit,"
        "p2_move_enter,p2_move_exit,p2_y_enter,p2_y_exit,tick_ms,dur_ms,"
        "rt04_enter,rt04_exit,toggle_enter,toggle_exit,fpu_enter,fpu_exit\r\n");
    char msg[96];
    std::snprintf(msg, sizeof(msg), "installed. dispatcher=0x%08lX",
                  static_cast<unsigned long>(g_batchAddr));
    Breadcrumb("stock_batch_probe", msg);
    return true;
}

void Shutdown()
{
    g_sink.Stop();
}

int Status(char* buf, size_t cap)
{
    return std::snprintf(buf, cap, "batch n=%ld frame=%ld",
                         static_cast<long>(InterlockedCompareExchange(&g_seq, 0, 0)),
                         static_cast<long>(InterlockedCompareExchange(&g_lastFrame, 0, 0)));
}

ProbeRegistrar g_registrar("stock_batch_probe", &Install, &Shutdown, &Status);

} // namespace
