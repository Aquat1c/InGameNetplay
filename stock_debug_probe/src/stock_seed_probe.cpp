// -----------------------------------------------------------------------------
// stock_seed_probe.cpp  --  RNG seed-apply observer module (part of debug.dll).
//
// Hooks Revival's minstd seed() normalize body (1.02h RVA 0x111D0; MSVC builds
// e-i share the prologue 55 8B EC 8B 4D 08). This one function serves BOTH the
// direct seed-apply paths (battle-start synced seed, spectator init, DLL-load
// default) AND the savestate-restore reapply, so it catches every engine
// (re)seed. On the modded captures the seed was already proven identical
// across peers; this module gives the same guarantee for STOCK runs - if two
// stock peers' seed streams differ, sync is broken before any effect draw.
//
// Rows are DEDUPED by seed value: per-frame idempotent restore reapplies of the
// same seed collapse into a suppressed_repeats count on the NEXT change row, so
// the CSV stays tiny while any genuinely new/changed seed is captured with the
// engine state before/after.
//
// Observe-only, fail-closed on prologue mismatch. Registers one ProbeModule;
// the shared host (probe_host.cpp) drives it.
//
// Output: debug_logs\stock_seed_trace_<pid>.csv.
// -----------------------------------------------------------------------------

#include "probe_common.h"

#include <MinHook.h>

namespace
{
using namespace probe;

constexpr uintptr_t kSeedApplyRva = 0x000111D0u; // 1.02h (h/i); e-g use 0x11060
const uint8_t kSeedPrologue[] = {0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08};

uintptr_t g_seedAddr = 0;
uintptr_t g_rngEngineAddr = 0;
uintptr_t g_sessionPtrGlobal = 0;

using SeedApplyFn = char(__stdcall*)(unsigned int);
SeedApplyFn g_origSeedApply = nullptr;

LineSink g_sink;
volatile LONG g_seq = 0;
volatile LONG g_applies = 0;
volatile LONG g_lastSeed = -1;   // last seed value seen (dedup key)
volatile LONG g_repeats = 0;     // identical applies since the last emitted row

char __stdcall HookSeedApply(unsigned int seed)
{
    const int32_t before = ReadI32(g_rngEngineAddr);
    const char result = g_origSeedApply(seed);
    const int32_t after = ReadI32(g_rngEngineAddr);
    InterlockedIncrement(&g_applies);

    const LONG prev = InterlockedExchange(&g_lastSeed, static_cast<LONG>(seed));
    if (prev == static_cast<LONG>(seed))
    {
        InterlockedIncrement(&g_repeats);
        return result;
    }
    const LONG repeats = InterlockedExchange(&g_repeats, 0);

    const LONG seq = InterlockedIncrement(&g_seq) - 1;
    const int32_t frame = SessionFrame(g_sessionPtrGlobal);

    // Negotiated per-battle config from the Init blob (session+944): local
    // side, input delay, max rollback, synced seed. Byte-diffing these across
    // peers proves the whole battle config matched, not just the seed.
    const uintptr_t s = ReadPtr(g_sessionPtrGlobal);
    const uint8_t cfgSide = s ? ReadU8(s + kSessionCfgSide) : 0xFF;
    const uint8_t cfgDelay = s ? ReadU8(s + kSessionCfgDelay) : 0xFF;
    const uint8_t cfgMaxRb = s ? ReadU8(s + kSessionCfgMaxRb) : 0xFF;
    const uint32_t cfgSeed = s ? ReadU32(s + kSessionCfgSeed) : 0;

    char line[224];
    const int n = std::snprintf(
        line, sizeof(line), "%ld,%lu,%ld,%ld,%lu,%ld,%ld,%ld,%u,%u,%u,%lu\r\n",
        static_cast<long>(seq), static_cast<unsigned long>(TickMs()),
        static_cast<long>(frame),
        static_cast<long>(SessionCommit(g_sessionPtrGlobal)),
        static_cast<unsigned long>(seed),
        static_cast<long>(before), static_cast<long>(after),
        static_cast<long>(repeats),
        static_cast<unsigned>(cfgSide), static_cast<unsigned>(cfgDelay),
        static_cast<unsigned>(cfgMaxRb), static_cast<unsigned long>(cfgSeed));
    if (n > 0) g_sink.Emit(line);

    // Seed changes are rare (battle starts / genuine reseeds) - surface each
    // one live on the console. Compare these values across the peers by eye.
    char ev[128];
    std::snprintf(ev, sizeof(ev),
                  "seed change: %lu (frame %ld, engine %ld->%ld, side=%u delay=%u maxrb=%u)",
                  static_cast<unsigned long>(seed), static_cast<long>(frame),
                  static_cast<long>(before), static_cast<long>(after),
                  static_cast<unsigned>(cfgSide), static_cast<unsigned>(cfgDelay),
                  static_cast<unsigned>(cfgMaxRb));
    PushEvent("stock_seed_probe", ev);
    return result;
}

// --- module interface (driven by probe_host.cpp) -----------------------------
bool Install(uintptr_t revivalBase)
{
    g_seedAddr = revivalBase + kSeedApplyRva;
    g_rngEngineAddr = revivalBase + kRngEngineStateRva;
    g_sessionPtrGlobal = revivalBase + kSessionPtrGlobalRva;

    if (!BytesMatch(g_seedAddr, kSeedPrologue, sizeof(kSeedPrologue)))
    {
        Breadcrumb("stock_seed_probe",
                   "seed-apply prologue mismatch (not 1.02h?); idle, fail-closed.");
        return false;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(g_seedAddr),
            reinterpret_cast<void*>(&HookSeedApply),
            reinterpret_cast<void**>(&g_origSeedApply)) != MH_OK
        || MH_EnableHook(reinterpret_cast<void*>(g_seedAddr)) != MH_OK)
    {
        Breadcrumb("stock_seed_probe", "hook install failed.");
        return false;
    }
    if (!g_sink.Open("stock_seed_trace"))
    {
        Breadcrumb("stock_seed_probe", "CSV open failed.");
        return false;
    }
    g_sink.Header(
        "seq,tick_ms,frame,commit,seed,engine_before,engine_after,"
        "suppressed_repeats,cfg_side,cfg_delay,cfg_maxrb,cfg_seed\r\n");
    char msg[96];
    std::snprintf(msg, sizeof(msg), "installed. seed_apply=0x%08lX",
                  static_cast<unsigned long>(g_seedAddr));
    Breadcrumb("stock_seed_probe", msg);
    return true;
}

void Shutdown()
{
    g_sink.Stop();
}

int Status(char* buf, size_t cap)
{
    return std::snprintf(buf, cap, "seed n=%ld last=%lu",
                         static_cast<long>(InterlockedCompareExchange(&g_applies, 0, 0)),
                         static_cast<unsigned long>(
                             InterlockedCompareExchange(&g_lastSeed, 0, 0)));
}

ProbeRegistrar g_registrar("stock_seed_probe", &Install, &Shutdown, &Status);

} // namespace
