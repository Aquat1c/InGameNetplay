// -----------------------------------------------------------------------------
// probe_common.h  --  shared, header-only helpers for stock_debug_probe DLLs.
//
// Observe-only building blocks: SEH-guarded memory reads, region hashing, EFZ
// battle-region resolution, and a lightweight line sink (string queue + flush
// worker) so probes never do disk I/O on the game/rollback thread. Every probe
// must stay a pure observer and fail-closed on a version/signature mismatch.
// -----------------------------------------------------------------------------
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace probe
{

// --- EfzRevival.dll 1.02h RVAs (byte-verified) -------------------------------
constexpr uintptr_t kRngEngineStateRva = 0x000A070Cu;
constexpr uintptr_t kSessionPtrGlobalRva = 0x000A02ECu;
constexpr uintptr_t kSessionCurrentFrame = 708u;   // session + 708
constexpr uintptr_t kSessionCommitFrame = 732u;    // session + 716 (gameMode snap) + 16

// --- efz.exe absolute addresses (main module @ 0x400000; invariant across
// Revival versions) -----------------------------------------------------------
constexpr uintptr_t kScreenTableAddr = 0x00790110u;
constexpr uintptr_t kScreenIndexAddr = 0x00790148u; // current screen id byte (3 = battle)
constexpr uintptr_t kGameSystemPtrAddr = 0x0079010Cu; // fixed global -> gameSystem
constexpr uintptr_t kScreenBattleIndex = 3u;
constexpr uintptr_t kOffsetGameSystem = 0x1Cu;     // battleScreen + 0x1C -> gameSystem
                                                   // (mod-verified; 0x28 was WRONG)
constexpr uintptr_t kOffsetBattleP1 = 12u;         // battleScreen + 12 -> P1 char*
constexpr uintptr_t kOffsetBattleP2 = 16u;         // battleScreen + 16 -> P2 char*
constexpr uintptr_t kOffsetBattleSaveP1 = 20u;     // battleScreen + 20 -> savestate P1 (double-indirect)
constexpr uintptr_t kOffsetBattleSaveP2 = 24u;     // battleScreen + 24 -> savestate P2 (double-indirect)
constexpr uintptr_t kBsRenderCtr04 = 4u;           // rendered-vs-elapsed counters:
constexpr uintptr_t kBsRenderCtr08 = 8u;           //   render-owned, LOCAL cadence -
constexpr uintptr_t kBsRenderCtr0C = 12u;          //   never compare across peers

// Savestate-copied region sizes (Revival save coverage; hash these for
// round-trip fidelity).
constexpr size_t kCharRegionSize = 0x3448u;        // per-char copy (min of Size[] table)
constexpr size_t kGameSysRegionSize = 0x142F0u;    // full gameSystem block
constexpr size_t kBattleScreenRegionSize = 0x598u; // battleContext block @ battleScreen

// gameSystem effect ring.
constexpr uintptr_t kEffectAllocCursor = 4992u;    // WORD
constexpr uintptr_t kEffectProcCursor = 4994u;     // WORD
constexpr uintptr_t kEffectActiveFlagBase = 4996u; // gameSystem + 4996 + 4*i
constexpr uintptr_t kEffectStatusBase = 7556u;     // gameSystem + 7556 + 4*i (status DWORDs)
constexpr uintptr_t kEffectRecordBase = 10760u;    // + 112*i
constexpr uintptr_t kEffectRecordStride = 112u;
constexpr uintptr_t kEffBehavior = 0u;             // WORD
constexpr uintptr_t kEffAnimFrame = 2u;            // WORD
constexpr uintptr_t kEffAnimTick = 4u;             // WORD
constexpr uintptr_t kEffPosX = 24u;                // double
constexpr uintptr_t kEffPosY = 32u;                // double
constexpr uintptr_t kEffVelX = 40u;                // double
constexpr uintptr_t kEffVelY = 48u;                // double
constexpr uintptr_t kEffDirection = 76u;           // WORD/DWORD direction/branch data
constexpr uint32_t  kEffectRingSlots = 640u;

// gameSystem sim/round/replay state (mod-verified offsets).
constexpr uintptr_t kGsCollGate40 = 4940u;         // u8 master collision-enable gate
constexpr uintptr_t kGsCollGate44 = 4944u;         // u8 collision gate
constexpr uintptr_t kGsCollGate48 = 4948u;         // u32 collision gate
constexpr uintptr_t kGsRoundNo = 4952u;            // u8 round number
constexpr uintptr_t kGsReadyMask = 4956u;          // u32 ready mask
constexpr uintptr_t kGsEffectsSetting = 4966u;     // u8 effects on/off (1 = full)
constexpr uintptr_t kGsRenderToggle = 4968u;       // u32 frame-parity toggle - RENDER-OWNED,
                                                   //   local cadence, never sync-compare
constexpr uintptr_t kGsNormalizer = 4972u;         // u32 (forced 1 by battle-entry normalizer)
constexpr uintptr_t kGsWordstreamMode = 82563u;    // u8 replay word-stream selector
constexpr uintptr_t kGsWordstreamCtr = 82576u;     // u32 reader counter (82564 struct + 12)
constexpr uintptr_t kGsSubStepCount = 82560u;      // sub-step count source (setEventTrigger
                                                   //   constants 1/2/3; feeds bc+1400)
constexpr uintptr_t kBcSubStepCount = 1400u;       // battleContext(+battleScreen) sub-step
                                                   //   count consumed by the tick loop

// Character struct fields (contact/collision inputs).
// DECOMP CORRECTION (audit 2026-07-20): char+0x20/+0x28 (=32/40) are the ONE
// true world-position pair, sim-written only - the collision/spawn math's
// "+216/+224" and "anchors +204/+206" are offsets into a STACK-BUILT collision
// context (copies of +32/+40, PAT-table words, facing), NOT char fields; the
// char-struct bytes at 204/206/216/224 are unused by the entire binary.
// Earlier captures' columns read from those offsets are garbage.
constexpr uintptr_t kCharMove = 8u;                // WORD move/PAT id
constexpr uintptr_t kCharAnimFrame = 10u;          // WORD (selects the PAT box row)
constexpr uintptr_t kCharAnimTick = 12u;           // WORD
constexpr uintptr_t kCharWorldX = 32u;             // double - REAL world X (spawn input)
constexpr uintptr_t kCharWorldY = 40u;             // double - REAL world Y (spawn input)
constexpr uintptr_t kCharVelX = 48u;               // double
constexpr uintptr_t kCharVelY = 56u;               // double
constexpr uintptr_t kCharFacing = 80u;             // i32 persistent facing sign (spawn input)
constexpr uintptr_t kCharMomX = 176u;              // double momentum X
constexpr uintptr_t kCharKbX = 192u;               // double knockback X (cross-object write)
constexpr uintptr_t kCharKbY = 200u;               // double knockback Y
constexpr uintptr_t kCharPushFlag = 208u;          // u32 (low word used)
constexpr uintptr_t kCharReactFlag = 300u;         // i32 victim reaction branch
constexpr uintptr_t kCharHitstun = 316u;           // WORD
constexpr uintptr_t kCharMeter = 328u;             // WORD
constexpr uintptr_t kCharFreeze = 330u;            // WORD freeze/hitstop (gates anim advance)
constexpr uintptr_t kCharFreeze2 = 332u;           // WORD opponent-secondary freeze
constexpr uintptr_t kCharBoxTable = 356u;          // u32 -> PAT box table (entry = tbl+8*move+4)
constexpr uintptr_t kCharContact = 360u;           // low word: 3 = throw accepted
constexpr uintptr_t kCharMoveTimer = 364u;         // WORD active-frame/move timer
constexpr uintptr_t kCharThrowCtr = 12600u;        // i32 throw/mash counter

// EfzRevival session struct (1.02h; e-i identical, j differs).
constexpr uintptr_t kSessionInputDelay = 688u;     // i32 input delay
constexpr uintptr_t kSessionWindowDelay = 692u;    // i32 window base delay (prediction on)
constexpr uintptr_t kSessionSyncFeed = 736u;       // i32 gmBase+20 Sync-feed ctr (log-only:
                                                   //   freezes after mid-prediction exits)
constexpr uintptr_t kSessionLocalHistVec = 788u;   // begin/end ptrs, 2-byte elements
constexpr uintptr_t kSessionRemoteHistVec = 800u;  // begin/end ptrs, 2-byte elements
constexpr uintptr_t kSessionPingMs = 936u;         // i32 ping in ms
constexpr uintptr_t kSessionInitBlob = 944u;       // 0x114 negotiated-config record:
constexpr uintptr_t kSessionCfgSide = 944u;        //   u8 active/local side
constexpr uintptr_t kSessionCfgDelay = 948u;       //   u8 negotiated input delay
constexpr uintptr_t kSessionCfgMaxRb = 949u;       //   u8 negotiated max rollback
constexpr uintptr_t kSessionCfgSeed = 952u;        //   u32 synced RNG seed
constexpr uintptr_t kSessionHighestFrame = 1236u;  // i32 highest frame reached

inline bool BytesMatch(uintptr_t addr, const uint8_t* expect, size_t n)
{
    __try { return std::memcmp(reinterpret_cast<const void*>(addr), expect, n) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline int32_t ReadI32(uintptr_t addr)
{
    __try { return *reinterpret_cast<const volatile int32_t*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

inline uint16_t ReadU16(uintptr_t addr)
{
    __try { return *reinterpret_cast<const volatile uint16_t*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFu; }
}

inline uint8_t ReadU8(uintptr_t addr)
{
    __try { return *reinterpret_cast<const volatile uint8_t*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFu; }
}

inline uint32_t ReadU32(uintptr_t addr)
{
    __try { return *reinterpret_cast<const volatile uint32_t*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0xFFFFFFFFu; }
}

// FPU control state at event time - the ONE spawn-math input a memcpy
// savestate cannot restore (Revival normalizes it only at batch boundaries).
// Raw register reads, no memory access, sampled BEFORE any SEH-guarded game
// read so a faulting read can't lose them. Healthy stock netplay values:
// x87 = 0x027F (double precision), mxcsr = 0x1FA0.
inline void ReadFpuState(uint16_t* x87Cw, uint32_t* mxcsr)
{
#if defined(_MSC_VER) && defined(_M_IX86)
    uint16_t cw = 0;
    uint32_t csr = 0;
    __asm fnstcw cw
    __asm stmxcsr csr
    *x87Cw = cw;
    *mxcsr = csr;
#else
    *x87Cw = 0;
    *mxcsr = 0;
#endif
}

// Count minstd (x48271 mod 2^31-1) advances from before->after; -1 if not
// reachable within maxSteps (the distribution adapter can reject-sample, so a
// single logical draw may advance the engine more than once).
inline int MinstdAdvances(int32_t before, int32_t after, int maxSteps)
{
    if (before <= 0 || after <= 0) return -1;
    if (before == after) return 0;
    int64_t s = before;
    for (int i = 1; i <= maxSteps; ++i)
    {
        s = (s * 48271) % 2147483647;
        if (static_cast<int32_t>(s) == after) return i;
    }
    return -1;
}

inline uintptr_t ReadPtr(uintptr_t addr)
{
    __try { return *reinterpret_cast<const volatile uintptr_t*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

inline uint64_t ReadDoubleBits(uintptr_t addr)
{
    __try
    {
        const uint32_t lo = *reinterpret_cast<const volatile uint32_t*>(addr);
        const uint32_t hi = *reinterpret_cast<const volatile uint32_t*>(addr + 4u);
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// SEH-guarded bulk copy for the full-dump path; false if any byte faulted.
inline bool GuardedCopy(void* dst, uintptr_t src, size_t n)
{
    __try
    {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// FNV-1a over a raw region; 0 on fault. Keep windows small on hot paths so the
// probe does not perturb the very timing it measures.
inline uint32_t Fnv1a(uintptr_t base, size_t size)
{
    uint32_t h = 2166136261u;
    __try
    {
        const volatile uint8_t* b = reinterpret_cast<const volatile uint8_t*>(base);
        for (size_t i = 0; i < size; ++i) { h ^= b[i]; h *= 16777619u; }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return h;
}

// Resolve the live EFZ battle regions the way the sim reads them. gameSystem
// comes from the anchor-free fixed global 0x79010C (identical object to
// battleScreen+0x1C - the mod uses both interchangeably).
inline bool ResolveBattle(uintptr_t* battleScreen, uintptr_t* gameSys,
                          uintptr_t* p1, uintptr_t* p2)
{
    const uintptr_t bs = ReadPtr(kScreenTableAddr + 4u * kScreenBattleIndex);
    if (bs == 0) return false;
    if (battleScreen) *battleScreen = bs;
    if (gameSys) *gameSys = ReadPtr(kGameSystemPtrAddr);
    if (p1) *p1 = ReadPtr(bs + kOffsetBattleP1);
    if (p2) *p2 = ReadPtr(bs + kOffsetBattleP2);
    return true;
}

inline int32_t SessionFrame(uintptr_t sessionPtrGlobal)
{
    const uintptr_t s = ReadPtr(sessionPtrGlobal);
    return s ? ReadI32(s + kSessionCurrentFrame) : -1;
}
inline int32_t SessionCommit(uintptr_t sessionPtrGlobal)
{
    const uintptr_t s = ReadPtr(sessionPtrGlobal);
    return s ? ReadI32(s + kSessionCommitFrame) : -1;
}

// --- Output layout -----------------------------------------------------------
// Everything this DLL writes lands in debug_logs\ (relative to the game's
// working directory), never in the game root: per-probe CSVs plus ONE shared
// status log (debug_status_<pid>.log) that replaces the old per-probe one-line
// breadcrumb files.
constexpr const char* kLogDir = "debug_logs";

inline void EnsureLogDir()
{
    CreateDirectoryA(kLogDir, nullptr); // idempotent; fails silently if exists
}

inline void BuildLogPath(char* out, size_t cap, const char* baseName, const char* ext)
{
    std::snprintf(out, cap, "%s\\%s_%lu.%s", kLogDir, baseName,
                  static_cast<unsigned long>(GetCurrentProcessId()), ext);
}

// Millisecond wall-clock for the tick_ms CSV columns (QPC-backed: GetTickCount's
// ~15.6 ms granularity is useless against a 16.7 ms game frame). Monotonic
// since boot; correlate with wall time via the host's "tick_ms=" anchor line in
// the status log, and across peers via deltas around a shared event.
inline uint32_t TickMs()
{
    static LONGLONG s_freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart != 0 ? f.QuadPart : 1;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint32_t>((c.QuadPart * 1000) / s_freq);
}

// --- Lightweight line sink: enqueue formatted CSV lines on the game thread;
//     a worker writes them to disk. No game-thread disk I/O. -----------------
class LineSink
{
public:
    bool Open(const char* baseName)
    {
        EnsureLogDir();
        char path[MAX_PATH];
        BuildLogPath(path, sizeof(path), baseName, "csv");
        file_ = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return false;
        worker_ = CreateThread(nullptr, 0, &LineSink::ThreadThunk, this, 0, nullptr);
        return true;
    }
    void Header(const char* h) { Emit(h); }
    void Emit(const char* line)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() < kMaxQueued) queue_.emplace_back(line);
    }
    void Stop()
    {
        InterlockedExchange(&stop_, 1);
        if (worker_) { WaitForSingleObject(worker_, 2000); CloseHandle(worker_); worker_ = nullptr; }
        Drain();
        if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
    }
private:
    static constexpr size_t kMaxQueued = 1u << 18;
    static DWORD WINAPI ThreadThunk(LPVOID p)
    {
        LineSink* self = static_cast<LineSink*>(p);
        while (InterlockedCompareExchange(&self->stop_, 0, 0) == 0)
        {
            Sleep(500);
            self->Drain();
        }
        return 0;
    }
    void Drain()
    {
        std::deque<std::string> local;
        { std::lock_guard<std::mutex> lock(mutex_); local.swap(queue_); }
        for (const std::string& s : local)
        {
            DWORD w = 0;
            WriteFile(file_, s.data(), static_cast<DWORD>(s.size()), &w, nullptr);
        }
        if (!local.empty()) FlushFileBuffers(file_);
    }
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE worker_ = nullptr;
    volatile LONG stop_ = 0;
    std::mutex mutex_;
    std::deque<std::string> queue_;
};

// --- Unified status channel --------------------------------------------------
// ALL status messages (host + every probe: install/idle/version/error) go to
// ONE timestamped file, debug_logs\debug_status_<pid>.log, tagged per source -
// replacing the old stack of one-line-per-probe breadcrumb files. If the host
// allocated a console, each line is echoed there for real-time monitoring.
// Status volume is low (events + a slow heartbeat) and is NEVER written from a
// game-thread hook, so console blocking can't stall the game.
inline HANDLE g_statusConsole = INVALID_HANDLE_VALUE;
inline std::mutex g_statusMutex;

inline void Breadcrumb(const char* tag, const char* msg)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[640];
    const int n = std::snprintf(
        line, sizeof(line), "[%02u:%02u:%02u.%03u] [%s] %s\r\n",
        static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond), static_cast<unsigned>(st.wMilliseconds),
        tag, msg);
    if (n <= 0) return;

    std::lock_guard<std::mutex> lock(g_statusMutex);
    EnsureLogDir();
    char path[MAX_PATH];
    BuildLogPath(path, sizeof(path), "debug_status", "log");
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        WriteFile(h, line, static_cast<DWORD>(n), &w, nullptr);
        CloseHandle(h);
    }
    if (g_statusConsole != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        WriteConsoleA(g_statusConsole, line, static_cast<DWORD>(n), &w, nullptr);
    }
}

// --- Live event queue --------------------------------------------------------
// Probes push short NOTABLE-EVENT lines (seed change, battle start, type47
// activity, stall...) from ANY thread — including game-thread hooks, because a
// push is just a mutex+deque append, no I/O. The host's monitor thread drains
// the queue every ~100 ms into the status channel (console + status log), so
// events show up live without the hooks ever touching a file or the console.
// Keep pushes RARE (rate-limit at the call site); this is not a data path.
inline std::mutex g_eventMutex;
inline std::deque<std::pair<std::string, std::string>>& EventQueue()
{
    static std::deque<std::pair<std::string, std::string>> q;
    return q;
}

inline void PushEvent(const char* tag, const char* msg)
{
    std::lock_guard<std::mutex> lock(g_eventMutex);
    if (EventQueue().size() < 512) EventQueue().emplace_back(tag, msg);
}

inline void DrainEvents()
{
    std::deque<std::pair<std::string, std::string>> local;
    {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        local.swap(EventQueue());
    }
    for (const auto& e : local) Breadcrumb(e.first.c_str(), e.second.c_str());
}

// --- Probe module registry ---------------------------------------------------
// One DLL, many probes. Each probe .cpp defines Install(base)/Shutdown() and
// self-registers one ProbeModule through a file-scope ProbeRegistrar. The host
// (probe_host.cpp) waits for EfzRevival, does the single MH_Initialize, then
// drives install/shutdown across the registry. Adding a probe = drop a
// src/<probe>.cpp with a ProbeRegistrar and add it to the CMake source list;
// no host edits. Every probe .cpp compiles directly into the DLL (not via a
// static lib), so its registrar's initializer is never stripped.
//
// The registry lives behind a function-local static (constructed on first use)
// to sidestep static-init-order issues: registrars in any TU may run before or
// after each other, but all run before the host reads the registry in DllMain's
// init thread.
//
//   install(base): create+enable this probe's hooks and open its sink; return
//                  true if armed, false if it idled (e.g. prologue mismatch).
//   shutdown()   : flush + close this probe's files. Must NOT call any MinHook
//                  API (the host owns the global MinHook lifecycle) and must be
//                  safe to run during process teardown (worker threads gone).
//   status()     : OPTIONAL (may be null). Write a short one-line fragment of
//                  live counters ("rows=1234") into buf for the host's console
//                  heartbeat; return chars written, or <=0 for nothing. Called
//                  from the host heartbeat thread only - must be cheap and
//                  lock-free (read counters via Interlocked, no file I/O).
struct ProbeModule
{
    const char* name;
    bool (*install)(uintptr_t revivalBase);
    void (*shutdown)();
    int (*status)(char* buf, size_t cap);
};

inline std::vector<ProbeModule>& Registry()
{
    static std::vector<ProbeModule> modules;
    return modules;
}

// Per-probe config for bisection experiments: debug_probe.ini placed NEXT TO
// debug.dll itself (CWD proved unreliable - a launcher's working directory ate
// the first bisection attempt). Section [probes]; keys are module names
// (stock_rng_probe, stock_rollback_probe, stock_batch_probe, stock_seed_probe,
// stock_session_probe) plus "full_dump" for the raw dump inside the rollback
// probe. The host defaults only the decisive stock_rng_probe ON; heavier
// rollback/batch/session probes and full_dump default OFF. Falls back to the
// CWD ini if none exists beside the DLL. ProbeConfigIniPath() lets the host
// print exactly which file was consulted.
inline const char* ProbeConfigIniPath()
{
    static char s_path[MAX_PATH] = {};
    if (s_path[0] == '\0')
    {
        HMODULE self = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&ProbeConfigIniPath), &self);
        char modPath[MAX_PATH] = {};
        if (self != nullptr && GetModuleFileNameA(self, modPath, MAX_PATH) != 0)
        {
            char* slash = std::strrchr(modPath, '\\');
            if (slash != nullptr)
            {
                *(slash + 1) = '\0';
                std::snprintf(s_path, sizeof(s_path), "%sdebug_probe.ini", modPath);
            }
        }
        if (s_path[0] != '\0'
            && GetFileAttributesA(s_path) == INVALID_FILE_ATTRIBUTES)
        {
            // No ini beside the DLL - fall back to the working directory.
            std::snprintf(s_path, sizeof(s_path), ".\\debug_probe.ini");
        }
        if (s_path[0] == '\0')
        {
            std::snprintf(s_path, sizeof(s_path), ".\\debug_probe.ini");
        }
    }
    return s_path;
}

inline bool ProbeConfigEnabled(const char* name, bool defaultOn = true)
{
    return GetPrivateProfileIntA(
               "probes", name, defaultOn ? 1 : 0, ProbeConfigIniPath()) != 0;
}

struct ProbeRegistrar
{
    ProbeRegistrar(const char* name, bool (*install)(uintptr_t), void (*shutdown)(),
                   int (*status)(char*, size_t) = nullptr)
    {
        Registry().push_back(ProbeModule{name, install, shutdown, status});
    }
};

} // namespace probe
