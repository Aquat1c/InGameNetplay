// -----------------------------------------------------------------------------
// stock_session_probe.cpp  --  hook-free session timeline sampler (part of
// debug.dll).
//
// No detours at all: a worker thread samples the live session/engine state
// every 250 ms via SEH-guarded reads - session frame + commit, minstd engine
// dword, effect-ring cursors, and each peer's move/world-Y. A row is written
// when frame or commit changed since the last row (plus a forced keep-alive row
// every ~5 s), so the CSV is a compact wall-clock timeline of the whole run:
// when the battle started, how the frame/commit pair advanced, where the
// engine state was at any wall-clock moment.
//
// This is the correlation backbone for cross-peer analysis: every other CSV
// carries the same monotonic tick_ms, and this file maps tick_ms onto session
// progress even in stretches where no hooked event fired (menus, pauses,
// stalls). Values are sampled off-thread, so treat them as timeline markers,
// not exact per-frame truth - the hooked probes give the exact values.
//
// Observe-only (reads only; SEH-guarded so it is safe on any Revival version -
// on a non-1.02h layout the columns just read as -1/0). Registers one
// ProbeModule; the shared host (probe_host.cpp) drives it.
//
// Output: debug_logs\stock_session_trace_<pid>.csv.
// -----------------------------------------------------------------------------

#include "probe_common.h"

namespace
{
using namespace probe;

uintptr_t g_sessionPtrGlobal = 0;
uintptr_t g_rngEngineAddr = 0;

LineSink g_sink;
HANDLE g_thread = nullptr;
volatile LONG g_stop = 0;
volatile LONG g_rows = 0;
volatile LONG g_lastSeenFrame = -1;

// Revival's named shared-memory wire rings (plain names on 1.02e-i; the _Spec
// suffix is 1.02j-only). Each has an 8-byte header: head DWORD then tail DWORD.
// Sampling head/tail against tick_ms measures input-arrival vs drain timing -
// the input-availability skew that drives rollback batch shape. Opened lazily
// (they only exist once a netplay session is up).
struct WireRing
{
    const char* name;
    HANDLE mapping;
    volatile uint32_t* view;
};
WireRing g_rings[4] = {
    {"InputP1", nullptr, nullptr},
    {"InputP2", nullptr, nullptr},
    {"Sync", nullptr, nullptr},
    {"Net", nullptr, nullptr},
};

void TryOpenRings()
{
    for (WireRing& r : g_rings)
    {
        if (r.view != nullptr) continue;
        HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, r.name);
        if (m == nullptr) continue;
        void* v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 8);
        if (v == nullptr) { CloseHandle(m); continue; }
        r.mapping = m;
        r.view = static_cast<volatile uint32_t*>(v);
        char ev[64];
        std::snprintf(ev, sizeof(ev), "wire ring '%s' mapped", r.name);
        PushEvent("stock_session_probe", ev);
    }
}

void CloseRings()
{
    for (WireRing& r : g_rings)
    {
        if (r.view != nullptr) UnmapViewOfFile(const_cast<uint32_t*>(r.view));
        if (r.mapping != nullptr) CloseHandle(r.mapping);
        r.view = nullptr;
        r.mapping = nullptr;
    }
}

DWORD WINAPI SampleThreadProc(LPVOID)
{
    int32_t lastFrame = -2, lastCommit = -2; // -2: differs from any real value
    uint32_t lastEmitTick = 0;
    uint32_t lastAdvanceTick = 0;
    bool stallAnnounced = false;
    LONG seq = 0;
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
    {
        Sleep(250);
        const uint32_t tick = TickMs();
        const int32_t frame = SessionFrame(g_sessionPtrGlobal);
        const int32_t commit = SessionCommit(g_sessionPtrGlobal);
        InterlockedExchange(&g_lastSeenFrame, frame);

        // Live transition events for the console: session appearing/resetting
        // (battle start / return to menu) and frame stalls (>3 s no advance
        // while a session is live) - the things worth noticing in real time.
        char ev[96];
        if (frame != lastFrame)
        {
            if (lastFrame < 0 && frame >= 0)
            {
                std::snprintf(ev, sizeof(ev), "session live at frame %ld",
                              static_cast<long>(frame));
                PushEvent("stock_session_probe", ev);
            }
            else if (frame < 0 && lastFrame >= 0)
            {
                std::snprintf(ev, sizeof(ev), "session ended (was frame %ld)",
                              static_cast<long>(lastFrame));
                PushEvent("stock_session_probe", ev);
            }
            else if (frame >= 0 && frame + 60 < lastFrame)
            {
                std::snprintf(ev, sizeof(ev), "frame reset %ld -> %ld (new round?)",
                              static_cast<long>(lastFrame), static_cast<long>(frame));
                PushEvent("stock_session_probe", ev);
            }
            lastAdvanceTick = tick;
            stallAnnounced = false;
        }
        else if (frame >= 0 && !stallAnnounced && tick - lastAdvanceTick > 3000u)
        {
            std::snprintf(ev, sizeof(ev), "frame STALLED at %ld for >3s",
                          static_cast<long>(frame));
            PushEvent("stock_session_probe", ev);
            stallAnnounced = true;
        }

        const bool changed = frame != lastFrame || commit != lastCommit;
        const bool keepAlive = tick - lastEmitTick >= 5000u;
        if (!changed && !keepAlive) continue;
        lastFrame = frame;
        lastCommit = commit;
        lastEmitTick = tick;

        const int32_t engine = ReadI32(g_rngEngineAddr);
        uintptr_t gameSys = 0, p1 = 0, p2 = 0;
        ResolveBattle(nullptr, &gameSys, &p1, &p2);
        const uint16_t alloc = gameSys ? ReadU16(gameSys + kEffectAllocCursor) : 0xFFFFu;
        const uint16_t proc = gameSys ? ReadU16(gameSys + kEffectProcCursor) : 0xFFFFu;
        const uint16_t p1Move = p1 ? ReadU16(p1 + kCharMove) : 0xFFFFu;
        const uint16_t p2Move = p2 ? ReadU16(p2 + kCharMove) : 0xFFFFu;
        const uint64_t p1y = p1 ? ReadDoubleBits(p1 + kCharWorldY) : 0;
        const uint64_t p2y = p2 ? ReadDoubleBits(p2 + kCharWorldY) : 0;

        // Prediction/stall counters from the Revival session struct (1.02h
        // layout) - quantify prediction depth and input starvation per sample.
        const uintptr_t s = ReadPtr(g_sessionPtrGlobal);
        int32_t inputDelay = -1, windowDelay = -1, pingMs = -1;
        int32_t syncFeed = -1, highestFrame = -1;
        int32_t localLen = -1, remoteLen = -1;
        if (s != 0)
        {
            inputDelay = ReadI32(s + kSessionInputDelay);
            windowDelay = ReadI32(s + kSessionWindowDelay);
            pingMs = ReadI32(s + kSessionPingMs);
            syncFeed = ReadI32(s + kSessionSyncFeed);
            highestFrame = ReadI32(s + kSessionHighestFrame);
            const uintptr_t lb = ReadPtr(s + kSessionLocalHistVec);
            const uintptr_t le = ReadPtr(s + kSessionLocalHistVec + 4u);
            const uintptr_t rb = ReadPtr(s + kSessionRemoteHistVec);
            const uintptr_t re = ReadPtr(s + kSessionRemoteHistVec + 4u);
            if (le >= lb) localLen = static_cast<int32_t>((le - lb) >> 1);
            if (re >= rb) remoteLen = static_cast<int32_t>((re - rb) >> 1);
        }

        // Wire-ring head/tail (input-arrival vs drain timing). Lazy-open until
        // the session has created the mappings.
        TryOpenRings();
        uint32_t rh[4] = {0, 0, 0, 0}, rt[4] = {0, 0, 0, 0};
        for (int r = 0; r < 4; ++r)
        {
            if (g_rings[r].view != nullptr)
            {
                rh[r] = g_rings[r].view[0];
                rt[r] = g_rings[r].view[1];
            }
        }

        char line[512];
        const int n = std::snprintf(
            line, sizeof(line),
            "%ld,%lu,%ld,%ld,%ld,%u,%u,%u,%u,%016llX,%016llX,"
            "%ld,%ld,%ld,%ld,%ld,%ld,%ld,"
            "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\r\n",
            static_cast<long>(seq++), static_cast<unsigned long>(tick),
            static_cast<long>(frame), static_cast<long>(commit),
            static_cast<long>(engine),
            static_cast<unsigned>(alloc), static_cast<unsigned>(proc),
            static_cast<unsigned>(p1Move), static_cast<unsigned>(p2Move),
            static_cast<unsigned long long>(p1y),
            static_cast<unsigned long long>(p2y),
            static_cast<long>(inputDelay), static_cast<long>(windowDelay),
            static_cast<long>(pingMs), static_cast<long>(syncFeed),
            static_cast<long>(highestFrame),
            static_cast<long>(localLen), static_cast<long>(remoteLen),
            static_cast<unsigned long>(rh[0]), static_cast<unsigned long>(rt[0]),
            static_cast<unsigned long>(rh[1]), static_cast<unsigned long>(rt[1]),
            static_cast<unsigned long>(rh[2]), static_cast<unsigned long>(rt[2]),
            static_cast<unsigned long>(rh[3]), static_cast<unsigned long>(rt[3]));
        if (n > 0)
        {
            g_sink.Emit(line);
            InterlockedIncrement(&g_rows);
        }
    }
    return 0;
}

// --- module interface (driven by probe_host.cpp) -----------------------------
bool Install(uintptr_t revivalBase)
{
    g_sessionPtrGlobal = revivalBase + kSessionPtrGlobalRva;
    g_rngEngineAddr = revivalBase + kRngEngineStateRva;

    if (!g_sink.Open("stock_session_trace"))
    {
        Breadcrumb("stock_session_probe", "CSV open failed.");
        return false;
    }
    g_sink.Header(
        "seq,tick_ms,frame,commit,engine,alloc_cursor,proc_cursor,"
        "p1_move,p2_move,p1_y_bits,p2_y_bits,"
        "input_delay,window_delay,ping_ms,sync_feed,highest_frame,"
        "local_len,remote_len,"
        "in1_head,in1_tail,in2_head,in2_tail,sync_head,sync_tail,"
        "net_head,net_tail\r\n");
    g_thread = CreateThread(nullptr, 0, &SampleThreadProc, nullptr, 0, nullptr);
    if (g_thread == nullptr)
    {
        Breadcrumb("stock_session_probe", "sampler thread create failed.");
        g_sink.Stop();
        return false;
    }
    Breadcrumb("stock_session_probe", "installed (hook-free 4 Hz sampler).");
    return true;
}

void Shutdown()
{
    InterlockedExchange(&g_stop, 1);
    if (g_thread != nullptr)
    {
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    CloseRings();
    g_sink.Stop();
}

int Status(char* buf, size_t cap)
{
    return std::snprintf(buf, cap, "sess frame=%ld rows=%ld",
                         static_cast<long>(
                             InterlockedCompareExchange(&g_lastSeenFrame, 0, 0)),
                         static_cast<long>(InterlockedCompareExchange(&g_rows, 0, 0)));
}

ProbeRegistrar g_registrar("stock_session_probe", &Install, &Shutdown, &Status);

} // namespace
