#include "netplay/bridge/takeover_internal.h"

#include "netplay/core/mod_settings.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace netplay::bridge::takeover
{
namespace
{
volatile LONG g_j102DiagnosticSequence = 0;
volatile LONG g_j102SnapshotSequence = 0;

constexpr uintptr_t kJ102CompactVtableRva = 0x0016FEB0u;
constexpr uintptr_t kJ102RollbackVtableRva = 0x0016FEF0u;
constexpr uintptr_t kJ102SpectatorVtableRva = 0x0016FF20u;
constexpr uintptr_t kJ102ReplayVtableRva = 0x0016FF50u;
constexpr uintptr_t kJ102PracticeVtableRva = 0x0016FF80u;

struct J102SessionDescriptor
{
    uintptr_t vtableRva;
    const char* name;
    size_t objectSize;
};

constexpr J102SessionDescriptor kJ102SessionDescriptors[] = {
    {kJ102CompactVtableRva, "compact", 0x380u},
    {kJ102RollbackVtableRva, "rollback", 0x778u},
    {kJ102SpectatorVtableRva, "spectator", 0x6A8u},
    // Replay is a mode-switch identity within Compact's 0x380-byte object.
    {kJ102ReplayVtableRva, "replay", 0x380u},
    {kJ102PracticeVtableRva, "practice", 0x2F0u},
};

const char* ContextOrUnknown(const char* context)
{
    return context != nullptr && context[0] != '\0' ? context : "unknown";
}

bool IsActiveJ102Profile()
{
    const bool activeProfileMatches = g_activeRevival != nullptr
        && g_activeRevival->versionTag != nullptr
        && std::strcmp(g_activeRevival->versionTag, "1.02j") == 0;
    if (activeProfileMatches)
    {
        return true;
    }

    // Injected helper callbacks can run before their copy of the bridge has
    // selected g_activeRevival. The host publishes the verified PE timestamp
    // in the shared block, which is sufficient to gate helper-side step logs.
    const SharedBlock* block = g_hostBlock != nullptr ? g_hostBlock : g_injectedBlock;
    return block != nullptr
        && block->hostRevivalTimestamp == kRevival_1_02j.peTimestamp;
}

const J102SessionDescriptor* FindJ102SessionDescriptor(uintptr_t vtableRva)
{
    for (const J102SessionDescriptor& descriptor : kJ102SessionDescriptors)
    {
        if (descriptor.vtableRva == vtableRva)
        {
            return &descriptor;
        }
    }
    return nullptr;
}

int ReadJ102SpanCount(uintptr_t sessionPtr, uintptr_t beginOffset, uintptr_t endOffset)
{
    uintptr_t begin = 0;
    uintptr_t end = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + beginOffset), &begin)
        || !SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + endOffset), &end)
        || begin == 0
        || end < begin)
    {
        return -1;
    }
    return static_cast<int>((end - begin) / sizeof(uint16_t));
}

void LogJ102SessionSemanticState(
    const char* context,
    uintptr_t sessionPtr,
    uintptr_t vtableRva)
{
    const char* ctx = ContextOrUnknown(context);
    auto readInt = [sessionPtr](uintptr_t offset) {
        int value = -1;
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + offset), &value);
        return value;
    };
    auto readPtr = [sessionPtr](uintptr_t offset) {
        uintptr_t value = 0;
        (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + offset), &value);
        return value;
    };
    auto readByte = [sessionPtr](uintptr_t offset) {
        uint8_t value = 0xFFu;
        (void)SafeReadByte(reinterpret_cast<const void*>(sessionPtr + offset), &value);
        return value;
    };

    if (vtableRva == kJ102SpectatorVtableRva)
    {
        const uintptr_t p1NameChars = readPtr(0x170u);
        const int p1NameLength = readInt(0x174u);
        const uintptr_t p2NameChars = readPtr(0x188u);
        const int p2NameLength = readInt(0x18Cu);
        mod::Log(
            "J102_DIAG[%s]: SPECTATOR frameLimit=%d frames=(%d,%d) "
            "screens=(%d->%d) inputs=(p1:%d p2:%d) peerHandle=0x%08lX "
            "helperPid=%d init=0x%02X replay=0x%02X wins=%d-%d",
            ctx,
            readInt(0x018u),
            readInt(0x1A0u),
            readInt(0x1A4u),
            readInt(0x1A8u),
            readInt(0x1ACu),
            ReadJ102SpanCount(sessionPtr, 0x04Cu, 0x050u),
            ReadJ102SpanCount(sessionPtr, 0x058u, 0x05Cu),
            static_cast<unsigned long>(readPtr(0x064u)),
            readInt(0x2E0u),
            static_cast<unsigned>(readByte(0x1B0u)),
            static_cast<unsigned>(readByte(0x1B1u)),
            readInt(0x1C4u),
            readInt(0x1C8u));
        mod::Log(
            "J102_DIAG[%s]: SPECTATOR names p1=(chars=0x%08lX len=%d sso=0x%08lX) "
            "p2=(chars=0x%08lX len=%d sso=0x%08lX) wireQueue=0x%08lX "
            "netCtrl=0x%08lX netAux=0x%08lX",
            ctx,
            static_cast<unsigned long>(p1NameChars),
            p1NameLength,
            static_cast<unsigned long>(sessionPtr + 0x178u),
            static_cast<unsigned long>(p2NameChars),
            p2NameLength,
            static_cast<unsigned long>(sessionPtr + 0x190u),
            static_cast<unsigned long>(sessionPtr + 0x4D0u),
            static_cast<unsigned long>(readPtr(0x588u)),
            static_cast<unsigned long>(readPtr(0x58Cu)));
        return;
    }

    if (vtableRva == kJ102RollbackVtableRva)
    {
        mod::Log(
            "J102_DIAG[%s]: ROLLBACK active=%d queue=%d delay=%d frame=%d "
            "match=%d syncFrame=%d helperHandle=0x%08lX helperPid=%d "
            "ping=%d wins=%d-%d sentinel=0x%08X",
            ctx,
            readInt(0x308u),
            readInt(0x30Cu),
            readInt(0x310u),
            readInt(0x324u),
            readInt(0x328u),
            readInt(0x340u),
            static_cast<unsigned long>(readPtr(0x31Cu)),
            readInt(0x560u),
            readInt(0x3B4u),
            readInt(0x564u),
            readInt(0x568u),
            static_cast<unsigned>(readInt(0x570u)));
        return;
    }

    if (vtableRva == kJ102PracticeVtableRva)
    {
        mod::Log(
            "J102_DIAG[%s]: PRACTICE stepPending=%u stepCounter=%d paused=%u "
            "lastScreen=%d hotkeys=(pause:%d step:%d save:%d load:%d) "
            "netCtrl=0x%08lX netAux=0x%08lX",
            ctx,
            static_cast<unsigned>(readByte(0x0D4u)),
            readInt(0x0D8u),
            static_cast<unsigned>(readByte(0x0DCu)),
            readInt(0x0E4u),
            readInt(0x200u),
            readInt(0x204u),
            readInt(0x208u),
            readInt(0x20Cu),
            static_cast<unsigned long>(readPtr(0x2DCu)),
            static_cast<unsigned long>(readPtr(0x2E0u)));
        return;
    }

    if (vtableRva == kJ102CompactVtableRva
        || vtableRva == kJ102ReplayVtableRva)
    {
        mod::Log(
            "J102_DIAG[%s]: COMPACT/REPLAY lastScreen=%d matchCount=%d scores=%d-%d",
            ctx,
            readInt(0x150u),
            readInt(0x368u),
            readInt(0x36Cu),
            readInt(0x370u));
    }
}

void LogMemoryRegion(const char* context, const char* label, uintptr_t address)
{
    MEMORY_BASIC_INFORMATION mbi = {};
    const SIZE_T queried = VirtualQuery(
        reinterpret_cast<LPCVOID>(address),
        &mbi,
        sizeof(mbi));
    if (queried != sizeof(mbi))
    {
        mod::Log(
            "J102_DIAG[%s]: REGION %s addr=0x%08lX query_failed error=%lu",
            ContextOrUnknown(context),
            label != nullptr ? label : "?",
            static_cast<unsigned long>(address),
            static_cast<unsigned long>(GetLastError()));
        return;
    }

    mod::Log(
        "J102_DIAG[%s]: REGION %s addr=0x%08lX base=0x%08lX allocBase=0x%08lX "
        "size=0x%lX state=0x%lX protect=0x%lX type=0x%lX",
        ContextOrUnknown(context),
        label != nullptr ? label : "?",
        static_cast<unsigned long>(address),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(mbi.AllocationBase)),
        static_cast<unsigned long>(mbi.RegionSize),
        static_cast<unsigned long>(mbi.State),
        static_cast<unsigned long>(mbi.Protect),
        static_cast<unsigned long>(mbi.Type));
}

void LogSharedBlockState(const char* context)
{
    const char* ctx = ContextOrUnknown(context);
    if (g_hostBlock == nullptr)
    {
        mod::Log("J102_DIAG[%s]: IPC hostBlock=null", ctx);
        return;
    }

    const SharedBlock* block = g_hostBlock;
    mod::Log(
        "J102_DIAG[%s]: IPC block=0x%08lX magic=0x%08lX version=%lu "
        "hostPid=%lu hostBase=0x%08lX hostTs=0x%08lX initSerial=%ld "
        "init=(%d,%d) console=%ld aux=%ld error=%ld peerQuit=%ld "
        "listener=%ld/%ld/%ld/pid=%ld expectedPort=%ld",
        ctx,
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(block)),
        static_cast<unsigned long>(block->magic),
        static_cast<unsigned long>(block->version),
        static_cast<unsigned long>(block->hostPid),
        static_cast<unsigned long>(block->hostRevivalBase),
        static_cast<unsigned long>(block->hostRevivalTimestamp),
        static_cast<long>(block->initSerial),
        block->initParams[0],
        block->initParams[1],
        static_cast<long>(block->consoleSerial),
        static_cast<long>(block->consoleAuxSerial),
        static_cast<long>(block->consoleErrorSerial),
        static_cast<long>(block->peerQuitDiagnosticSerial),
        static_cast<long>(block->hostListenerSerial),
        static_cast<long>(block->hostListenerFamily),
        static_cast<long>(block->hostListenerPort),
        static_cast<long>(block->hostListenerProcessId),
        static_cast<long>(block->hostExpectedListenerPort));
    mod::Log(
        "J102_DIAG[%s]: IPC delay prompt=%ld/%ld metrics=%ld avg=%d min=%d max=%d "
        "recommend=%d range=%d..%d input=%ld/%ld value=%d",
        ctx,
        static_cast<long>(block->delayPromptSerial),
        static_cast<long>(block->delayPromptServedSerial),
        static_cast<long>(block->delayMetricsSerial),
        block->delayAveragePingMs,
        block->delayMinPingMs,
        block->delayMaxPingMs,
        block->delayRecommended,
        block->delayRangeMin,
        block->delayRangeMax,
        static_cast<long>(block->delayInputSerial),
        static_cast<long>(block->delayInputServedSerial),
        block->delayInputValue);
    mod::Log(
        "J102_DIAG[%s]: IPC consoleHandoff host=%ld wake=%ld/%ld "
        "promptTransition=%ld nativeTimeout=%ld/%ld requiredWake=%ld",
        ctx,
        static_cast<long>(block->isHostSession),
        static_cast<long>(block->consoleControlWakeRequestSerial),
        static_cast<long>(block->consoleControlWakeServedSerial),
        static_cast<long>(block->delayPromptTransitionWakeSerial),
        static_cast<long>(block->nativeDelayTimeoutSerial),
        static_cast<long>(block->nativeDelayTimeoutHandledSerial),
        static_cast<long>(
            block->nativeDelayTimeoutRequiredWakeSerial));
    mod::Log(
        "J102_DIAG[%s]: IPC spectate prompt=%ld/%ld kind=%d input=%ld/%ld value=%d "
        "hits read=%ld auto=%ld createProcess=%ld writeProcess=%ld remoteThread=%ld",
        ctx,
        static_cast<long>(block->spectateConfirmPromptSerial),
        static_cast<long>(block->spectateConfirmPromptServedSerial),
        block->spectateConfirmPromptKind,
        static_cast<long>(block->spectateConfirmInputSerial),
        static_cast<long>(block->spectateConfirmInputServedSerial),
        block->spectateConfirmInputValue,
        static_cast<long>(block->dbgReadConsoleHits),
        static_cast<long>(block->dbgReadConsoleAutoHits),
        static_cast<long>(block->dbgCreateProcessHits),
        static_cast<long>(block->dbgWriteProcessHits),
        static_cast<long>(block->dbgCreateRemoteThreadHits));

    const size_t rawHeaderSize = std::min<size_t>(
        offsetof(SharedBlock, peerQuitDiagnosticText),
        0x300u);
    LogRevival102jDeepBytes(
        ctx,
        "ipc.shared_block_header",
        reinterpret_cast<uintptr_t>(block),
        rawHeaderSize);
}

void LogWireMappings(const char* context)
{
    static const char* const kLegacyNames[] = {
        "Init", "Net", "Sync", "Quit", "LoadMatch",
        "InputP1", "InputP2", "PaletteP1", "PaletteP2",
    };

    for (const char* legacyName : kLegacyNames)
    {
        const char* wireName = RevivalWireName(legacyName);
        SetLastError(ERROR_SUCCESS);
        HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, wireName);
        if (mapping == nullptr)
        {
            mod::Log(
                "J102_DIAG[%s]: WIRE legacy=%s selected=%s mapping=missing error=%lu",
                ContextOrUnknown(context),
                legacyName,
                wireName,
                static_cast<unsigned long>(GetLastError()));
            continue;
        }

        void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (view == nullptr)
        {
            const DWORD error = GetLastError();
            mod::Log(
                "J102_DIAG[%s]: WIRE legacy=%s selected=%s handle=0x%p map_failed error=%lu",
                ContextOrUnknown(context),
                legacyName,
                wireName,
                static_cast<void*>(mapping),
                static_cast<unsigned long>(error));
            CloseHandle(mapping);
            continue;
        }

        int head = -1;
        int tail = -1;
        const bool headOk = SafeReadInt(view, &head);
        const bool tailOk = SafeReadInt(
            static_cast<const uint8_t*>(view) + sizeof(uint32_t),
            &tail);
        mod::Log(
            "J102_DIAG[%s]: WIRE legacy=%s selected=%s handle=0x%p view=0x%08lX "
            "head=%s%d tail=%s%d depth=%d",
            ContextOrUnknown(context),
            legacyName,
            wireName,
            static_cast<void*>(mapping),
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(view)),
            headOk ? "" : "?",
            head,
            tailOk ? "" : "?",
            tail,
            headOk && tailOk ? tail - head : -1);
        LogRevival102jDeepBytes(
            context,
            wireName,
            reinterpret_cast<uintptr_t>(view),
            0x40u);

        UnmapViewOfFile(view);
        CloseHandle(mapping);
    }
}

void LogProcessAndControlState(const char* context)
{
    DWORD exitCode = 0xFFFFFFFFu;
    DWORD processIdFromHandle = 0;
    BOOL exitCodeOk = FALSE;
    if (g_revivalProcess != nullptr)
    {
        processIdFromHandle = GetProcessId(g_revivalProcess);
        exitCodeOk = GetExitCodeProcess(g_revivalProcess, &exitCode);
    }

    mod::Log(
        "J102_DIAG[%s]: CONTROL localRole=%d netRole=%d localInit=%d specAttempt=%d "
        "specOk=%d insideFrame=%d deferredWork=%ld deferredSelection=%ld tournamentPending=%d",
        ContextOrUnknown(context),
        g_localRoleFlag,
        g_netplayRole,
        g_localInitAppliedForSession ? 1 : 0,
        g_spectatorPostInitAttemptedForSession ? 1 : 0,
        g_spectatorPostInitSucceededForSession ? 1 : 0,
        IsInsideFrameTick() ? 1 : 0,
        static_cast<long>(InterlockedCompareExchange(&g_deferredLifecycleWorkRequested, 0, 0)),
        static_cast<long>(InterlockedCompareExchange(&g_deferredTitleSelection, 0, 0)),
        g_tournamentReturnCleanupPending ? 1 : 0);
    mod::Log(
        "J102_DIAG[%s]: HANDLES helper=0x%p storedPid=%lu handlePid=%lu exitOk=%d "
        "exitCode=0x%08lX hostMap=0x%p hostBlock=0x%08lX initEvent=0x%p "
        "consoleEvent=0x%p (events intentionally not waited by diagnostics)",
        ContextOrUnknown(context),
        static_cast<void*>(g_revivalProcess),
        static_cast<unsigned long>(g_revivalProcessId),
        static_cast<unsigned long>(processIdFromHandle),
        exitCodeOk ? 1 : 0,
        static_cast<unsigned long>(exitCode),
        static_cast<void*>(g_hostMapHandle),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(g_hostBlock)),
        static_cast<void*>(g_hostInitEvent),
        static_cast<void*>(g_hostConsoleEvent));
}

void LogDllAndSessionState(const char* context)
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("J102_DIAG[%s]: DLL EfzRevival.dll not loaded", ContextOrUnknown(context));
        return;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t imageBase = 0;
    uintptr_t imageEnd = 0;
    const bool imageOk = ReadModuleImageRange(revival, &imageBase, &imageEnd);
    int roleFlag = -1;
    int initFlag = -1;
    uint8_t initByte = 0xFFu;
    uintptr_t sessionPtr = 0;
    uintptr_t globalState = 0;
    uintptr_t timerPtr = 0;
    uintptr_t renderContext = 0;
    uintptr_t renderContextBase = 0;

    (void)SafeReadInt(reinterpret_cast<const void*>(base + 0x0014EC40u), &roleFlag);
    (void)SafeReadInt(reinterpret_cast<const void*>(base + 0x0014EC44u), &initFlag);
    (void)SafeReadByte(reinterpret_cast<const void*>(base + 0x0014ED64u), &initByte);
    (void)SafeReadPtr(reinterpret_cast<const void*>(base + 0x0014E980u), &sessionPtr);
    (void)SafeReadPtr(reinterpret_cast<const void*>(base + 0x0014E924u), &globalState);
    (void)SafeReadPtr(reinterpret_cast<const void*>(base + 0x0014E914u), &timerPtr);
    (void)SafeReadPtr(reinterpret_cast<const void*>(base + 0x0014E8D8u), &renderContext);
    renderContextBase = base + 0x0014E8C0u;

    mod::Log(
        "J102_DIAG[%s]: DLL base=0x%08lX image=0x%08lX..0x%08lX imageOk=%d "
        "role=%d initFlag=%d initByte=0x%02X session=0x%08lX globalState=0x%08lX "
        "timer=0x%08lX render=0x%08lX patchCtx=0x%08lX",
        ContextOrUnknown(context),
        static_cast<unsigned long>(base),
        static_cast<unsigned long>(imageBase),
        static_cast<unsigned long>(imageEnd),
        imageOk ? 1 : 0,
        roleFlag,
        initFlag,
        static_cast<unsigned>(initByte),
        static_cast<unsigned long>(sessionPtr),
        static_cast<unsigned long>(globalState),
        static_cast<unsigned long>(timerPtr),
        static_cast<unsigned long>(renderContext),
        static_cast<unsigned long>(renderContextBase));

    LogRevival102jDeepBytes(context, "dll.patch_context_core", base + 0x0014E8C0u, 0x80u);
    LogRevival102jDeepBytes(context, "dll.session_global_window", base + 0x0014E900u, 0xB0u);
    LogRevival102jDeepBytes(context, "dll.role_flag_window", base + 0x0014EC20u, 0x60u);
    LogRevival102jDeepBytes(context, "dll.init_byte_window", base + 0x0014ED40u, 0x40u);
    LogRevival102jDeepBytes(context, "dll.frame_hook", base + 0x00040200u, 0x40u);
    LogRevival102jDeepBytes(context, "dll.per_frame_tick", base + 0x00040AD0u, 0x40u);
    LogRevival102jDeepBytes(context, "dll.tournament_exit_guard", base + 0x00046820u, 0x40u);

    if (sessionPtr == 0)
    {
        mod::Log("J102_DIAG[%s]: SESSION global pointer is null", ContextOrUnknown(context));
        return;
    }

    uintptr_t vtable = 0;
    const bool vtableOk = SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable);
    const uintptr_t vtableRva =
        vtableOk && vtable >= base && vtable < imageEnd ? vtable - base : 0;
    const J102SessionDescriptor* descriptor = FindJ102SessionDescriptor(vtableRva);
    mod::Log(
        "J102_DIAG[%s]: SESSION ptr=0x%08lX vtableOk=%d vtable=0x%08lX rva=0x%08lX "
        "kind=%s objectSize=0x%zX",
        ContextOrUnknown(context),
        static_cast<unsigned long>(sessionPtr),
        vtableOk ? 1 : 0,
        static_cast<unsigned long>(vtable),
        static_cast<unsigned long>(vtableRva),
        descriptor != nullptr ? descriptor->name : "unknown/neutralized",
        descriptor != nullptr ? descriptor->objectSize : 0x100u);
    LogMemoryRegion(context, "session", sessionPtr);
    if (vtable != 0)
    {
        LogMemoryRegion(context, "session.vtable", vtable);
        LogRevival102jDeepBytes(context, "session.vtable[0..9]", vtable, 10u * sizeof(uintptr_t));
    }
    LogRevival102jDeepBytes(
        context,
        descriptor != nullptr ? descriptor->name : "session.unknown",
        sessionPtr,
        descriptor != nullptr ? descriptor->objectSize : 0x100u);
    if (descriptor != nullptr)
    {
        LogJ102SessionSemanticState(context, sessionPtr, vtableRva);
    }
}

void LogExePatchState(const char* context)
{
    if (netplay::bridge::IsCurrentProcessRevival())
    {
        mod::Log(
            "J102_DIAG[%s]: EXE patch windows skipped inside helper process",
            ContextOrUnknown(context));
        return;
    }

    LogRevival102jDeepBytes(context, "efz.frame_hook_0x401582", 0x00401570u, 0x40u);
    LogRevival102jDeepBytes(context, "efz.dispatch_0x401642", 0x00401630u, 0x40u);
    LogRevival102jDeepBytes(context, "efz.mode_ctor_pair", 0x00763E30u, 0x120u);
    LogRevival102jDeepBytes(context, "efz.tournament_754c1a", 0x00754C00u, 0x40u);
    LogRevival102jDeepBytes(context, "efz.tournament_7599ed", 0x007599D0u, 0x50u);
}
} // namespace

bool IsRevival102jDeepDiagnosticsEnabled()
{
    return netplay::mod_settings::IsVerboseRevival102jLifecycleLoggingEnabled()
        && IsActiveJ102Profile();
}

void LogRevival102jDeepStep(const char* context, const NetbridgeStatus* status)
{
    if (!IsRevival102jDeepDiagnosticsEnabled())
    {
        return;
    }

    const LONG sequence = InterlockedIncrement(&g_j102DiagnosticSequence);
    if (status != nullptr)
    {
        mod::Log(
            "J102_DIAG_STEP[%ld][%s]: tick=%lu thread=%lu phase=%s(%d) role=%d "
            "roleFlag=%d statusInit=%d processId=%lu error='%s'",
            static_cast<long>(sequence),
            ContextOrUnknown(context),
            static_cast<unsigned long>(GetTickCount()),
            static_cast<unsigned long>(GetCurrentThreadId()),
            netplay::bridge::PhaseToString(static_cast<NetbridgePhase>(status->phase)),
            status->phase,
            status->role,
            status->roleFlag,
            status->localInitApplied,
            static_cast<unsigned long>(status->processId),
            status->errorMsg);
    }
    else
    {
        mod::Log(
            "J102_DIAG_STEP[%ld][%s]: tick=%lu thread=%lu localRole=%d netRole=%d "
            "localInit=%d insideFrame=%d",
            static_cast<long>(sequence),
            ContextOrUnknown(context),
            static_cast<unsigned long>(GetTickCount()),
            static_cast<unsigned long>(GetCurrentThreadId()),
            g_localRoleFlag,
            g_netplayRole,
            g_localInitAppliedForSession ? 1 : 0,
            IsInsideFrameTick() ? 1 : 0);
    }
}

void LogRevival102jDeepBytes(
    const char* context,
    const char* label,
    uintptr_t address,
    size_t size)
{
    if (!IsRevival102jDeepDiagnosticsEnabled())
    {
        return;
    }

    const char* ctx = ContextOrUnknown(context);
    const char* rangeLabel = label != nullptr ? label : "?";
    constexpr size_t kLineBytes = 16u;
    constexpr size_t kMaximumDumpBytes = 0x800u;
    const size_t dumpSize = std::min(size, kMaximumDumpBytes);
    mod::Log(
        "J102_DIAG[%s]: BYTES_BEGIN %s addr=0x%08lX requested=0x%zX dump=0x%zX%s",
        ctx,
        rangeLabel,
        static_cast<unsigned long>(address),
        size,
        dumpSize,
        size > dumpSize ? " truncated" : "");
    LogMemoryRegion(ctx, rangeLabel, address);

    for (size_t lineOffset = 0; lineOffset < dumpSize; lineOffset += kLineBytes)
    {
        char hex[kLineBytes * 3u + 1u] = {};
        char ascii[kLineBytes + 1u] = {};
        size_t hexUsed = 0;
        const size_t lineSize = std::min(kLineBytes, dumpSize - lineOffset);
        for (size_t i = 0; i < kLineBytes; ++i)
        {
            const bool inRange = i < lineSize;
            uint8_t value = 0;
            const bool readable = inRange && SafeReadByte(
                reinterpret_cast<const void*>(address + lineOffset + i),
                &value);
            const char* token = !inRange ? "  " : (readable ? nullptr : "??");
            int written = 0;
            if (token != nullptr)
            {
                written = std::snprintf(
                    hex + hexUsed,
                    sizeof(hex) - hexUsed,
                    "%s%s",
                    i == 0 ? "" : " ",
                    token);
            }
            else
            {
                written = std::snprintf(
                    hex + hexUsed,
                    sizeof(hex) - hexUsed,
                    "%s%02X",
                    i == 0 ? "" : " ",
                    static_cast<unsigned>(value));
            }
            if (written > 0 && static_cast<size_t>(written) < sizeof(hex) - hexUsed)
            {
                hexUsed += static_cast<size_t>(written);
            }
            ascii[i] = readable && std::isprint(static_cast<unsigned char>(value)) != 0
                ? static_cast<char>(value)
                : (inRange ? '.' : ' ');
        }

        mod::Log(
            "J102_DIAG[%s]: BYTES %s +0x%04zX @0x%08lX  %-47s |%s|",
            ctx,
            rangeLabel,
            lineOffset,
            static_cast<unsigned long>(address + lineOffset),
            hex,
            ascii);
    }
    mod::Log("J102_DIAG[%s]: BYTES_END %s", ctx, rangeLabel);
}

void LogRevival102jDeepSnapshot(const char* context, const NetbridgeStatus* status)
{
    if (!IsRevival102jDeepDiagnosticsEnabled())
    {
        return;
    }

    const LONG snapshot = InterlockedIncrement(&g_j102SnapshotSequence);
    const char* ctx = ContextOrUnknown(context);
    mod::Log(
        "J102_DIAG_SNAPSHOT[%ld][%s]: BEGIN schema=1 "
        "gate=VerboseRevival102jLifecycleLogging:1 tick=%lu thread=%lu",
        static_cast<long>(snapshot),
        ctx,
        static_cast<unsigned long>(GetTickCount()),
        static_cast<unsigned long>(GetCurrentThreadId()));
    LogRevival102jDeepStep(ctx, status);
    LogProcessAndControlState(ctx);
    LogSharedBlockState(ctx);
    LogExePatchState(ctx);
    LogDllAndSessionState(ctx);
    LogWireMappings(ctx);
    mod::Log(
        "J102_DIAG_SNAPSHOT[%ld][%s]: END",
        static_cast<long>(snapshot),
        ctx);
}
} // namespace netplay::bridge::takeover
