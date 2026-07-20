// Batch-dispatch timing stabilizer. See include/netplay/bridge/batch_stabilizer.h
// for the empirical background (bisection-verified suppressor; mechanism class
// matches Wine's dinput message-wait fix for the same game).
//
// Faithfulness matters: the verified suppressor was the standalone probe's
// wrap, whose per-batch cost is CONSTANT for the whole process lifetime
// (snprintf + string allocation + mutex + queue push, with a worker draining
// the queue every 500 ms). The first in-mod port kept the lines in a capped
// ring with no drain, so once the cap filled (mid-session) the allocation and
// push silently stopped - and the protection faded, which is exactly the
// "works for one session, desyncs after rehost" failure observed in the
// postfix capture. This unit reproduces the probe's steady-state exactly:
// the sink is drained (and discarded) on a worker thread, so every batch pays
// the same cost forever.
//
// Version coverage: a verified per-version RVA table (each entry's own prologue
// byte-checked, fail-closed) covering 1.02e/f/f-fs/g/h/i (MSVC, RVAs isolated
// by bisection + independent byte scan) and 1.02j (MinGW, RVA + convention
// byte-derived by structural analogy - see the table's honesty caveat), plus a
// masked-signature scan of EfzRevival.dll's .text for any unknown MSVC-family
// version without a table entry. The signature is the 1.02h dispatcher head
// (64 bytes, operand fields wildcarded); it is accepted only on a UNIQUE match
// whose prologue also matches, so a wrong build can never get a wrap on the
// wrong function. A future unknown MinGW build won't match the MSVC signature
// and stays cleanly fail-closed until byte-verified into the table.

#include "netplay/bridge/batch_stabilizer.h"

#include "netplay/bridge/takeover_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>

#include <MinHook.h>

namespace netplay::bridge::batch_stabilizer
{
namespace
{

// --- target location ---------------------------------------------------------

// Dispatcher head prologues, checked byte-for-byte before a hook is placed.
//
// MSVC family (1.02e/f/f-fs/g/h/i) share this head: push ebp; mov ebp,esp;
// sub esp,8; push ebx; push esi. Also the anchor for the signature scan.
constexpr uint8_t kPrologueMsvc[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x53, 0x56};

// 1.02j (MinGW) dispatcher head (see the profile note below the table).
constexpr uint8_t kPrologueJ[] = {0x83, 0xEC, 0x24, 0x89, 0x6C, 0x24, 0x20, 0x8B,
                                  0x6C, 0x24, 0x28, 0x85, 0xED, 0x0F, 0x8E};

// 1.02j (MinGW) dispatcher head at RVA 0x509B0, byte-verified from the shipped
// DLL: sub esp,0x24; mov [esp+0x20],ebp; mov ebp,[esp+0x28]; test ebp,ebp; jle.
// The function is a callee-clean __thiscall - it ends `add esp,0x24; ret 4`
// (one stack arg cleaned) with `this` arriving in ECX (`mov ebx,ecx` right
// after the prologue, then reads this+0x380/+0x384) - i.e. the SAME calling
// convention as the MSVC dispatcher, so the __fastcall(ecx,edx,a2) hook and the
// char(__thiscall*)(void*,int) trampoline typedef apply unchanged. It is the
// once-per-tick rollback batch executor (a2 = iteration count, loop over the
// per-frame resim), uniquely reachable from the rollback tick virtual 0x54CD0
// via the module's ONLY rel32 call to it (@0x552FB, E8 B0 B6 FF FF).
//
// PROFILE for future reference (1.02j, MinGW, image base 0x70000000, all
// byte-verified): batch dispatcher 0x509B0; unique caller 0x552FB (in tick
// virtual 0x54CD0, vtable[+0xC] after startInitPlayer 0x54CD0's vtable slot);
// perFrameTick 0x40AD0 (tail loads session from [0x7014E980] = profile
// sessionPtr, tail-jumps [vtable+0xC]). NOTE the structural difference from the
// MSVC builds: the savestate SAVE is HOISTED OUT of the batch loop - the save
// equivalent is 0x7C0E0 (prologue 55 89 E5 57 56 53 89 CB 81 EC 60 01 00 00),
// called from the tick virtual via 0x4EE60, NOT from inside 0x509B0. This wrap
// targets the batch EXECUTOR for its timing role (fixed work bracketing the
// once-per-tick rollback batch, the empirically verified suppression
// mechanism); it does not depend on save-under-loop, which the wrap never used.
//
// HONESTY CAVEAT: the MSVC entry set was isolated by BISECTION on real desyncs;
// the 1.02j entry is a STRUCTURAL analogy (same-role, same-convention function,
// same once-per-tick bracket) built entirely from j's own bytes - it has NOT
// been desync-repro-verified (no 1.02j desync capture exists). It is a
// best-effort "just in case" and can be removed/adjusted if a real j capture
// ever shows it is the wrong point.
struct DispatcherRva
{
    const char* versionTag;
    uintptr_t rva;
    const uint8_t* prologue;
    size_t prologueLen;
};
constexpr DispatcherRva kKnownDispatchers[] = {
    {"1.02e", 0x000723A0u, kPrologueMsvc, sizeof(kPrologueMsvc)},
    {"1.02f", 0x000723D0u, kPrologueMsvc, sizeof(kPrologueMsvc)},
    {"1.02f-framestepping", 0x000725D0u, kPrologueMsvc, sizeof(kPrologueMsvc)},
    {"1.02g", 0x00072600u, kPrologueMsvc, sizeof(kPrologueMsvc)},
    {"1.02h", 0x00072D30u, kPrologueMsvc, sizeof(kPrologueMsvc)},
    {"1.02i", 0x000731A0u, kPrologueMsvc, sizeof(kPrologueMsvc)},
    {"1.02j", 0x000509B0u, kPrologueJ, sizeof(kPrologueJ)},
};

// Dispatcher head at 1.02h RVA 0x72D30 with operand fields wildcarded
// (0x00 in kSigMask = wildcard). Structure: `this` into esi, an IAT call
// stored to [ebp-8], three E8 calls seeding [esi]/[esi+4]/[esi+8], then the
// iteration-count loop head (mov eax,[ebp+8]; xor edi,edi; test eax,eax; jle).
constexpr uint8_t kSigBytes[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x53, 0x56,
    0x57, 0x8B, 0xF1, 0xFF, 0x15, 0x00, 0x00, 0x00,
    0x00, 0x6A, 0x00, 0x89, 0x45, 0xF8, 0xE8, 0x00,
    0x00, 0x00, 0x00, 0x6A, 0x01, 0x89, 0x46, 0x04,
    0xE8, 0x00, 0x00, 0x00, 0x00, 0x6A, 0x00, 0x89,
    0x46, 0x08, 0xE8, 0x00, 0x00, 0x00, 0x00, 0x8B,
    0x45, 0x08, 0x33, 0xFF, 0x85, 0xC0, 0x0F, 0x8E,
    0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0x45, 0xFC,
};
constexpr uint8_t kSigMask[] = {
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 0, 0, 0,
    0, 1, 1, 1, 1, 1, 1, 0,
    0, 0, 0, 1, 1, 1, 1, 1,
    1, 0, 0, 0, 0, 1, 1, 1,
    1, 1, 1, 0, 0, 0, 0, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 0, 1, 1, 1, 1,
};
static_assert(sizeof(kSigBytes) == sizeof(kSigMask), "signature/mask mismatch");

bool BytesMatchGuarded(uintptr_t addr, const uint8_t* expect, size_t n)
{
    __try
    {
        return std::memcmp(reinterpret_cast<const void*>(addr), expect, n) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Locate the dispatcher inside the loaded module: table RVA first (its
// version-specific prologue verified), else a unique masked-signature hit in
// .text (MSVC family only). On success *outPrologue/*outPrologueLen hold the
// prologue that matched, for InstallAt's TOCTOU recheck.
uintptr_t LocateDispatcher(
    uintptr_t base, const uint8_t** outPrologue, size_t* outPrologueLen)
{
    const netplay::bridge::takeover::RevivalAddressProfile* profile =
        netplay::bridge::takeover::g_activeRevival;
    if (profile != nullptr && profile->versionTag != nullptr)
    {
        for (const DispatcherRva& e : kKnownDispatchers)
        {
            if (std::strcmp(e.versionTag, profile->versionTag) == 0 && e.rva != 0)
            {
                const uintptr_t addr = base + e.rva;
                if (BytesMatchGuarded(addr, e.prologue, e.prologueLen))
                {
                    *outPrologue = e.prologue;
                    *outPrologueLen = e.prologueLen;
                    return addr;
                }
                mod::Log(
                    "BATCH_STABILIZER: table RVA 0x%08lX prologue mismatch for "
                    "%s; falling back to signature scan",
                    static_cast<unsigned long>(e.rva), e.versionTag);
            }
        }
    }

    // Signature scan over .text (MSVC-family builds share the codegen; the
    // MinGW 1.02j rebuild will simply not match and stays fail-closed until
    // its RVA is verified and added to the table).
    uintptr_t textVa = 0;
    uint32_t textSize = 0;
    __try
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(base);
        const uint32_t eLfanew = *reinterpret_cast<const uint32_t*>(b + 0x3C);
        const uint8_t* nt = b + eLfanew;
        const uint16_t sections = *reinterpret_cast<const uint16_t*>(nt + 6);
        const uint16_t optSize = *reinterpret_cast<const uint16_t*>(nt + 20);
        const uint8_t* sec = nt + 24 + optSize;
        for (uint16_t i = 0; i < sections; ++i, sec += 40)
        {
            if (std::memcmp(sec, ".text", 5) == 0)
            {
                textVa = base + *reinterpret_cast<const uint32_t*>(sec + 12);
                textSize = *reinterpret_cast<const uint32_t*>(sec + 8);
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
    if (textVa == 0 || textSize < sizeof(kSigBytes))
    {
        return 0;
    }

    uintptr_t hit = 0;
    int hits = 0;
    __try
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(textVa);
        const size_t span = textSize - sizeof(kSigBytes);
        for (size_t i = 0; i <= span; ++i)
        {
            size_t j = 0;
            for (; j < sizeof(kSigBytes); ++j)
            {
                if (kSigMask[j] != 0 && p[i + j] != kSigBytes[j])
                {
                    break;
                }
            }
            if (j == sizeof(kSigBytes))
            {
                ++hits;
                hit = textVa + i;
                if (hits > 1)
                {
                    break;
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
    if (hits != 1)
    {
        mod::Log(
            "BATCH_STABILIZER: signature scan hits=%d (need exactly 1); "
            "fail-closed for this Revival build",
            hits);
        return 0;
    }
    // The signature is the MSVC head, so a scan hit is MSVC-prologue-shaped.
    *outPrologue = kPrologueMsvc;
    *outPrologueLen = sizeof(kPrologueMsvc);
    mod::Log(
        "BATCH_STABILIZER: dispatcher located by signature at RVA 0x%08lX",
        static_cast<unsigned long>(hit - base));
    return hit;
}

// --- the wrap ---------------------------------------------------------------

using BatchDispatchFn = char(__thiscall*)(void*, int);
BatchDispatchFn g_original = nullptr;
uintptr_t g_hookedAddr = 0;
uintptr_t g_hookedBase = 0;
bool g_installLogged = false;
volatile LONG g_installBusy = 0;

// Health counters for the periodic heartbeat: a long session must be able to
// prove from the log alone that the wrap is still firing at full cost.
volatile LONG g_wrapCalls = 0;     // dispatcher wraps executed (lifetime)
volatile LONG g_sinkDropped = 0;   // lines dropped at the cap (should stay 0)
volatile LONG g_sinkDrained = 0;   // lines drained by the worker (lifetime)
volatile LONG g_repairs = 0;       // lost-patch re-enables
volatile LONG g_reinstalls = 0;    // module-rebase reinstalls

// Steady-cost sink replicating the probe's LineSink: producer does mutex +
// string allocation + push on EVERY batch (capped only against a stuck
// worker); the worker drains-and-discards every 500 ms so the cap is never
// reached in practice and the per-batch cost stays constant forever. This
// constancy IS the fix - see the file header.
std::mutex g_sinkMutex;
std::deque<std::string> g_sinkLines;
constexpr size_t kSinkCap = 1u << 18;
bool g_sinkWorkerStarted = false;

void SinkWorkerMain()
{
    // Heartbeat cadence: every 600 drain cycles = ~5 minutes. The delta of
    // g_wrapCalls between heartbeats is the liveness proof - during an active
    // battle it must be nonzero; wraps=+0 across a heartbeat while a session
    // runs means the wrap stopped firing (the failure mode the postfix rehost
    // capture exposed, now impossible to miss in the log).
    LONG lastWraps = 0;
    unsigned cycle = 0;
    for (;;)
    {
        Sleep(500);
        std::deque<std::string> drained;
        {
            std::lock_guard<std::mutex> lock(g_sinkMutex);
            drained.swap(g_sinkLines);
        }
        // Discarded: the wrap's value is its timing profile, not the data.
        InterlockedExchangeAdd(&g_sinkDrained, static_cast<LONG>(drained.size()));

        if (++cycle >= 600)
        {
            cycle = 0;
            const LONG wraps = InterlockedCompareExchange(&g_wrapCalls, 0, 0);
            mod::Log(
                "BATCH_STABILIZER: heartbeat wraps=%ld (+%ld) drained=%ld "
                "dropped=%ld repairs=%ld reinstalls=%ld hooked=%d",
                static_cast<long>(wraps), static_cast<long>(wraps - lastWraps),
                static_cast<long>(InterlockedCompareExchange(&g_sinkDrained, 0, 0)),
                static_cast<long>(InterlockedCompareExchange(&g_sinkDropped, 0, 0)),
                static_cast<long>(InterlockedCompareExchange(&g_repairs, 0, 0)),
                static_cast<long>(InterlockedCompareExchange(&g_reinstalls, 0, 0)),
                g_hookedAddr != 0 ? 1 : 0);
            lastWraps = wraps;
        }
    }
}

struct SampleCtx
{
    uint16_t alloc = 0xFFFF, proc = 0xFFFF;
    uint16_t p1Move = 0xFFFF, p2Move = 0xFFFF;
    uint32_t p1yLo = 0, p1yHi = 0, p2yLo = 0, p2yHi = 0;
    uint32_t renderCtr = 0, renderToggle = 0;
    uint16_t fpuCw = 0;
};

uint32_t ReadDwordGuarded(uintptr_t addr)
{
    __try
    {
        return *reinterpret_cast<const volatile uint32_t*>(addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

uint16_t ReadWordGuarded(uintptr_t addr)
{
    __try
    {
        return *reinterpret_cast<const volatile uint16_t*>(addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0xFFFFu;
    }
}

int32_t ReadIntGuarded(uintptr_t addr)
{
    __try
    {
        return *reinterpret_cast<const volatile int32_t*>(addr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

void ReadSampleCtx(SampleCtx* c)
{
#if defined(_MSC_VER) && defined(_M_IX86)
    {
        uint16_t cw = 0;
        __asm fnstcw cw
        c->fpuCw = cw;
    }
#endif
    const uintptr_t bs =
        static_cast<uintptr_t>(ReadDwordGuarded(0x00790110u + 4u * 3u));
    const uintptr_t gameSys = static_cast<uintptr_t>(ReadDwordGuarded(0x0079010Cu));
    if (bs != 0)
    {
        c->renderCtr = ReadDwordGuarded(bs + 4u);
        const uintptr_t p1 = static_cast<uintptr_t>(ReadDwordGuarded(bs + 12u));
        const uintptr_t p2 = static_cast<uintptr_t>(ReadDwordGuarded(bs + 16u));
        if (p1 != 0)
        {
            c->p1Move = ReadWordGuarded(p1 + 8u);
            c->p1yLo = ReadDwordGuarded(p1 + 40u);
            c->p1yHi = ReadDwordGuarded(p1 + 44u);
        }
        if (p2 != 0)
        {
            c->p2Move = ReadWordGuarded(p2 + 8u);
            c->p2yLo = ReadDwordGuarded(p2 + 40u);
            c->p2yHi = ReadDwordGuarded(p2 + 44u);
        }
    }
    if (gameSys != 0)
    {
        c->alloc = ReadWordGuarded(gameSys + 4992u);
        c->proc = ReadWordGuarded(gameSys + 4994u);
        c->renderToggle = ReadDwordGuarded(gameSys + 4968u);
    }
}

void ReadSessionFrames(int* frame, int* commit)
{
    *frame = -1;
    *commit = -1;
    const netplay::bridge::takeover::RevivalAddressProfile* profile =
        netplay::bridge::takeover::g_activeRevival;
    if (profile == nullptr || g_hookedBase == 0
        || profile->sessionPtrOffsetCount <= 0)
    {
        return;
    }
    const uintptr_t session = static_cast<uintptr_t>(
        ReadDwordGuarded(g_hookedBase + profile->sessionPtrOffsets[0]));
    if (session == 0)
    {
        return;
    }
    if (profile->sessionOffsetCurrentFrame != 0)
    {
        *frame = ReadIntGuarded(session + profile->sessionOffsetCurrentFrame);
    }
    if (profile->sessionOffsetGameModeSnapshot != 0)
    {
        *commit =
            ReadIntGuarded(session + profile->sessionOffsetGameModeSnapshot + 16u);
    }
}

char __fastcall DispatcherWrap(void* thisPtr, void* /*edx*/, int a2)
{
    InterlockedIncrement(&g_wrapCalls);
    LARGE_INTEGER qpcBefore = {};
    QueryPerformanceCounter(&qpcBefore);
    int frameEnter = -1, commitEnter = -1;
    ReadSessionFrames(&frameEnter, &commitEnter);
    SampleCtx enter;
    ReadSampleCtx(&enter);

    const char result = g_original(thisPtr, a2);

    int frameExit = -1, commitExit = -1;
    ReadSessionFrames(&frameExit, &commitExit);
    SampleCtx exitCtx;
    ReadSampleCtx(&exitCtx);
    LARGE_INTEGER qpcAfter = {};
    QueryPerformanceCounter(&qpcAfter);

    char line[512];
    const int n = std::snprintf(
        line, sizeof(line),
        "0x%08lX,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,"
        "%08lX%08lX,%08lX%08lX,%u,%u,%08lX%08lX,%08lX%08lX,%lld,0x%04X,0x%04X,"
        "%lu,%lu",
        static_cast<unsigned long>(a2),
        frameEnter, commitEnter, frameExit, commitExit,
        frameExit - frameEnter,
        static_cast<int>(static_cast<unsigned char>(result)),
        static_cast<unsigned>(enter.alloc), static_cast<unsigned>(exitCtx.alloc),
        static_cast<unsigned>(enter.proc), static_cast<unsigned>(exitCtx.proc),
        static_cast<unsigned>(enter.p1Move), static_cast<unsigned>(exitCtx.p1Move),
        static_cast<unsigned long>(enter.p1yHi), static_cast<unsigned long>(enter.p1yLo),
        static_cast<unsigned long>(exitCtx.p1yHi), static_cast<unsigned long>(exitCtx.p1yLo),
        static_cast<unsigned>(enter.p2Move), static_cast<unsigned>(exitCtx.p2Move),
        static_cast<unsigned long>(enter.p2yHi), static_cast<unsigned long>(enter.p2yLo),
        static_cast<unsigned long>(exitCtx.p2yHi), static_cast<unsigned long>(exitCtx.p2yLo),
        static_cast<long long>(qpcAfter.QuadPart - qpcBefore.QuadPart),
        static_cast<unsigned>(enter.fpuCw), static_cast<unsigned>(exitCtx.fpuCw),
        static_cast<unsigned long>(enter.renderCtr),
        static_cast<unsigned long>(enter.renderToggle));
    if (n > 0)
    {
        std::lock_guard<std::mutex> lock(g_sinkMutex);
        if (g_sinkLines.size() < kSinkCap)
        {
            g_sinkLines.emplace_back(line, static_cast<size_t>(n));
        }
        else
        {
            InterlockedIncrement(&g_sinkDropped);
        }
    }
    return result;
}

bool InstallAt(uintptr_t base)
{
    const uint8_t* prologue = nullptr;
    size_t prologueLen = 0;
    const uintptr_t addr = LocateDispatcher(base, &prologue, &prologueLen);
    if (addr == 0)
    {
        return false;
    }
    if (prologue == nullptr || !BytesMatchGuarded(addr, prologue, prologueLen))
    {
        mod::Log(
            "BATCH_STABILIZER: prologue mismatch at 0x%08lX; fail-closed",
            static_cast<unsigned long>(addr));
        return false;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        mod::Log("BATCH_STABILIZER: MH_Initialize failed status=%d", init);
        return false;
    }
    const MH_STATUS create = MH_CreateHook(
        reinterpret_cast<void*>(addr),
        reinterpret_cast<void*>(&DispatcherWrap),
        reinterpret_cast<void**>(&g_original));
    if (create != MH_OK && create != MH_ERROR_ALREADY_CREATED)
    {
        mod::Log("BATCH_STABILIZER: MH_CreateHook failed status=%d", create);
        return false;
    }
    if (g_original == nullptr)
    {
        // ALREADY_CREATED without a trampoline pointer would make the wrap
        // call through null. Never enable in that state.
        mod::Log("BATCH_STABILIZER: no trampoline after create (status=%d); "
                 "fail-closed", create);
        return false;
    }
    const MH_STATUS enable = MH_EnableHook(reinterpret_cast<void*>(addr));
    if (enable != MH_OK && enable != MH_ERROR_ENABLED)
    {
        mod::Log("BATCH_STABILIZER: MH_EnableHook failed status=%d", enable);
        return false;
    }
    g_hookedAddr = addr;
    g_hookedBase = base;
    if (!g_sinkWorkerStarted)
    {
        try
        {
            std::thread(&SinkWorkerMain).detach();
            g_sinkWorkerStarted = true;
        }
        catch (...)
        {
            // Without the drain the cap eventually freezes the per-batch cost
            // profile; keep running but say so loudly.
            mod::Log("BATCH_STABILIZER: sink worker failed to start");
        }
    }
    if (!g_installLogged)
    {
        g_installLogged = true;
        mod::Log(
            "BATCH_STABILIZER: installed @0x%08lX (dispatcher wrap; "
            "bisection-verified desync suppressor)",
            static_cast<unsigned long>(addr));
    }
    else
    {
        mod::Log(
            "BATCH_STABILIZER: reinstalled @0x%08lX after module change",
            static_cast<unsigned long>(addr));
    }
    return true;
}

} // namespace

void EnsurePerTick()
{
    // Fast path first and lock-free: a single guarded byte read. Only on an
    // anomaly do we touch GetModuleHandleA (which briefly takes the loader
    // lock - not something to pay every tick on the game thread).
    if (g_hookedAddr != 0)
    {
        uint8_t first = 0;
        __try
        {
            first = *reinterpret_cast<const volatile uint8_t*>(g_hookedAddr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            first = 0;
        }
        if (first == 0xE9)
        {
            return; // healthy - the overwhelmingly common exit
        }

        const HMODULE revival = GetModuleHandleA("EfzRevival.dll");
        const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
        if (base == g_hookedBase)
        {
            // Patch byte lost (someone restored the prologue): re-enable.
            if (InterlockedExchange(&g_installBusy, 1) == 0)
            {
                (void)MH_DisableHook(reinterpret_cast<void*>(g_hookedAddr));
                const MH_STATUS re =
                    MH_EnableHook(reinterpret_cast<void*>(g_hookedAddr));
                InterlockedIncrement(&g_repairs);
                mod::Log(
                    "BATCH_STABILIZER: repaired lost patch @0x%08lX status=%d",
                    static_cast<unsigned long>(g_hookedAddr), re);
                InterlockedExchange(&g_installBusy, 0);
            }
            return;
        }
        // Module unloaded or rebased: the old MinHook entry's target memory is
        // gone; abandon it (MinHook keeps the slot, we hook the new address).
        InterlockedIncrement(&g_reinstalls);
        mod::Log(
            "BATCH_STABILIZER: EfzRevival.dll moved 0x%08lX -> 0x%08lX; "
            "reinstalling",
            static_cast<unsigned long>(g_hookedBase),
            static_cast<unsigned long>(base));
        g_hookedAddr = 0;
        g_hookedBase = 0;
        g_original = nullptr;
    }

    const HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return; // retry next tick
    }
    if (InterlockedExchange(&g_installBusy, 1) != 0)
    {
        return;
    }
    (void)InstallAt(reinterpret_cast<uintptr_t>(revival));
    InterlockedExchange(&g_installBusy, 0);
}

} // namespace netplay::bridge::batch_stabilizer
