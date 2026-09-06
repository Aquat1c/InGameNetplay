// -----------------------------------------------------------------------------
// probe_host.cpp  --  single entry point / driver for debug.dll.
//
// debug.dll packs several observe-only probes (rng / rollback / batch / seed /
// session, and more later) into ONE module. Each probe self-registers a
// ProbeModule (see probe_common.h). The host here:
//   * owns the one and only DllMain,
//   * creates debug_logs\ and the live-status console (status messages only),
//   * waits once for EfzRevival.dll (probes no longer each spin their own wait),
//   * does the single MH_Initialize, then installs every registered probe,
//   * runs a 5 s heartbeat composing each armed probe's live counters,
//   * on teardown, flushes/closes every probe and unwinds MinHook.
//
// The host logic (HostRun/HostStop) is deliberately split from DllMain so the
// exact same registry can later be driven from a standalone EXE's main() when
// this grows into a general EFZ debugging tool.
//
// Teardown is loader-lock aware: on real process exit (lpReserved != nullptr)
// the worker threads are already gone and calling MinHook under the loader lock
// is unsafe, so we ONLY flush each probe's files inline; on an explicit
// FreeLibrary (lpReserved == nullptr) we additionally disable and uninitialize
// MinHook so the trampolines don't dangle in a still-running process.
// -----------------------------------------------------------------------------

#include "probe_common.h"

#include <MinHook.h>

namespace
{
using namespace probe;

volatile LONG g_started = 0;   // HostRun ran (once)
volatile LONG g_stopped = 0;   // HostStop ran (once)
volatile LONG g_mhReady = 0;   // MH_Initialize succeeded (guards teardown)
volatile LONG g_hbStop = 0;    // heartbeat thread stop request
HANDLE g_hbThread = nullptr;
bool g_consoleAllocated = false;

// Which registered modules actually armed, for the heartbeat composition.
std::vector<const ProbeModule*> g_armed;

bool ProbeEnabledForRun(const char* name)
{
    // The RNG+effect-slot observer alone distinguishes the two four-call raw
    // paths. Keep every broader/heavier probe opt-in so an absent INI cannot
    // accidentally turn a minimal causal capture into a timing stress test.
    const bool defaultOn =
        name != nullptr && std::strcmp(name, "stock_rng_probe") == 0;
    return ProbeConfigEnabled(name, defaultOn);
}

uintptr_t WaitForRevival()
{
    HMODULE revival = nullptr;
    for (int i = 0; i < 600 && revival == nullptr; ++i)
    {
        revival = GetModuleHandleA("EfzRevival.dll");
        if (revival == nullptr) Sleep(100);
    }
    return reinterpret_cast<uintptr_t>(revival);
}

// Real-time monitoring console. Status messages ONLY (the Breadcrumb stream +
// the heartbeat below) - CSV data never goes here. QuickEdit is disabled so an
// accidental click-select in the window can't block a writer.
void SetupConsole()
{
    if (!AllocConsole()) return; // process may already own a console
    g_consoleAllocated = true;
    SetConsoleTitleA("EFZ debug.dll - live status");
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != INVALID_HANDLE_VALUE && out != nullptr) g_statusConsole = out;
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in != INVALID_HANDLE_VALUE && in != nullptr)
    {
        DWORD mode = 0;
        if (GetConsoleMode(in, &mode))
        {
            SetConsoleMode(in, (mode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);
        }
    }
}

// Monitor thread: drains the live event queue every 100 ms (probes' notable
// events reach the console/status log near-instantly) and every 5 s emits one
// console heartbeat line composed from each armed probe's status() counters,
// so capture progress is visible live without opening the CSVs. Heartbeats are
// console-only by design - the status FILE stays events-only.
DWORD WINAPI HeartbeatThreadProc(LPVOID)
{
    char line[512];
    char frag[128];
    while (InterlockedCompareExchange(&g_hbStop, 0, 0) == 0)
    {
        for (int i = 0; i < 50; ++i)  // 5s in 100ms slices for prompt shutdown
        {
            if (InterlockedCompareExchange(&g_hbStop, 0, 0) != 0)
            {
                DrainEvents();
                return 0;
            }
            Sleep(100);
            DrainEvents();
        }
        if (g_statusConsole == INVALID_HANDLE_VALUE) continue;

        SYSTEMTIME st;
        GetLocalTime(&st);
        int n = std::snprintf(line, sizeof(line), "[%02u:%02u:%02u] [hb]",
                              static_cast<unsigned>(st.wHour),
                              static_cast<unsigned>(st.wMinute),
                              static_cast<unsigned>(st.wSecond));
        for (const ProbeModule* m : g_armed)
        {
            if (m->status == nullptr) continue;
            const int fn = m->status(frag, sizeof(frag));
            if (fn <= 0) continue;
            n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n),
                               " | %s", frag);
            if (n >= static_cast<int>(sizeof(line)) - 1) break;
        }
        n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), "\r\n");
        DWORD w = 0;
        WriteConsoleA(g_statusConsole, line, static_cast<DWORD>(n), &w, nullptr);
    }
    return 0;
}

void HostRun()
{
    if (InterlockedExchange(&g_started, 1) != 0) return;

    EnsureLogDir();
    SetupConsole();
    Breadcrumb("host", "debug.dll loaded; waiting for EfzRevival.dll (max 60s)...");

    const uintptr_t base = WaitForRevival();
    if (base == 0)
    {
        Breadcrumb("host", "EfzRevival.dll not found after 60s; idle.");
        return;
    }

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        Breadcrumb("host", "MH_Initialize failed; idle.");
        return;
    }
    InterlockedExchange(&g_mhReady, 1);

    // Self-diagnosing config line: exactly which ini was consulted, whether it
    // exists, and every module's resolved on/off state - so a bisection run
    // can never silently use the wrong config again.
    {
        const char* iniPath = ProbeConfigIniPath();
        const bool iniFound =
            GetFileAttributesA(iniPath) != INVALID_FILE_ATTRIBUTES;
        char cfg[512];
        int n = std::snprintf(cfg, sizeof(cfg), "config ini='%s' found=%d |",
                              iniPath, iniFound ? 1 : 0);
        for (const ProbeModule& m : Registry())
        {
            n += std::snprintf(cfg + n, sizeof(cfg) - static_cast<size_t>(n),
                               " %s=%d", m.name,
                               ProbeEnabledForRun(m.name) ? 1 : 0);
            if (n >= static_cast<int>(sizeof(cfg)) - 24) break;
        }
        std::snprintf(cfg + n, sizeof(cfg) - static_cast<size_t>(n),
                      " full_dump=%d rng_verbose=%d",
                      ProbeConfigEnabled("full_dump", false) ? 1 : 0,
                      ProbeConfigEnabled("stock_rng_verbose", false) ? 1 : 0);
        Breadcrumb("host", cfg);
    }

    int armed = 0;
    const int total = static_cast<int>(Registry().size());
    for (const ProbeModule& m : Registry())
    {
        if (!ProbeEnabledForRun(m.name))
        {
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "%s DISABLED via debug_probe.ini (bisection)", m.name);
            Breadcrumb("host", msg);
            continue;
        }
        if (m.install != nullptr && m.install(base))
        {
            ++armed;
            g_armed.push_back(&m);
        }
    }

    // tick_ms here is the anchor tying this line's wall-clock timestamp to the
    // monotonic tick_ms column in every CSV.
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "host up. base=0x%08lX modules armed %d/%d; logs in %s\\; tick_ms=%lu",
                  static_cast<unsigned long>(base), armed, total, kLogDir,
                  static_cast<unsigned long>(TickMs()));
    Breadcrumb("host", msg);

    if (armed > 0)
    {
        g_hbThread = CreateThread(nullptr, 0, &HeartbeatThreadProc, nullptr, 0, nullptr);
    }
}

void HostStop(bool processTerminating)
{
    if (InterlockedExchange(&g_stopped, 1) != 0) return;

    InterlockedExchange(&g_hbStop, 1);
    if (!processTerminating && g_hbThread != nullptr)
    {
        WaitForSingleObject(g_hbThread, 1000);
        CloseHandle(g_hbThread);
        g_hbThread = nullptr;
    }

    // Take detours down before draining sinks so no in-flight hook writes into
    // a closing file. Skip all MinHook calls during process teardown: the game
    // threads are already dead (no new hook entries) and MinHook under the
    // loader lock can deadlock.
    const bool touchMinHook =
        !processTerminating && InterlockedCompareExchange(&g_mhReady, 0, 0) != 0;
    if (touchMinHook) MH_DisableHook(MH_ALL_HOOKS);

    for (const ProbeModule& m : Registry())
    {
        if (m.shutdown != nullptr) m.shutdown();
    }
    DrainEvents(); // flush any last queued events into the status log

    if (touchMinHook) MH_Uninitialize();

    if (!processTerminating && g_consoleAllocated)
    {
        g_statusConsole = INVALID_HANDLE_VALUE;
        FreeConsole();
    }
}

DWORD WINAPI InitThreadProc(LPVOID)
{
    HostRun();
    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hInst);
        CreateThread(nullptr, 0, &InitThreadProc, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        HostStop(/*processTerminating=*/lpReserved != nullptr);
    }
    return TRUE;
}
