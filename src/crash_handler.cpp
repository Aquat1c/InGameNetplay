#include "crash_handler.h"
#include "netplay/bridge/session_bridge.h"

#include "logger.h"

#include <DbgHelp.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
std::mutex g_crashMutex;
PVOID g_vectoredHandle = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previousUnhandledFilter = nullptr;
char g_moduleDirectory[MAX_PATH] = {};
bool g_injectedMode = false;
std::atomic<bool> g_dumpWritten{false};// One-shot flag: the TOCTOU netplay recovery may only fire once per session.
// Without this guard the VEH would fire repeatedly if DLL code at the
// redirected address also crashes (e.g. partially-unwound DLL frames).
std::atomic<bool> g_toctouRecoveryFired{false};HMODULE g_dbgHelpModule = nullptr;
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(
    HANDLE,
    DWORD,
    HANDLE,
    MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION,
    PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_miniDumpWriteDump = nullptr;

bool ResolveMiniDumpWriteDump()
{
    if (g_miniDumpWriteDump != nullptr)
    {
        return true;
    }

    if (g_dbgHelpModule == nullptr)
    {
        const char* const candidates[] = {"dbgcore.dll", "dbghelp.dll"};
        for (const char* candidate : candidates)
        {
            g_dbgHelpModule = LoadLibraryA(candidate);
            if (g_dbgHelpModule == nullptr)
            {
                mod::Log("CrashHandler: LoadLibrary('%s') failed (%lu)", candidate, GetLastError());
                continue;
            }

            g_miniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
                GetProcAddress(g_dbgHelpModule, "MiniDumpWriteDump"));
            if (g_miniDumpWriteDump != nullptr)
            {
                mod::Log("CrashHandler: using %s for MiniDumpWriteDump", candidate);
                return true;
            }

            mod::Log("CrashHandler: %s does not export MiniDumpWriteDump", candidate);
            FreeLibrary(g_dbgHelpModule);
            g_dbgHelpModule = nullptr;
        }

        return false;
    }

    return true;
}

bool IsCrashCode(DWORD code)
{
    switch (code)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case EXCEPTION_DATATYPE_MISALIGNMENT:
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_INT_OVERFLOW:
    case EXCEPTION_INVALID_DISPOSITION:
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
        return true;
    default:
        return false;
    }
}

void ResolveModuleDirectory(HMODULE moduleHandle)
{
    char modulePath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        std::snprintf(g_moduleDirectory, sizeof(g_moduleDirectory), ".");
        return;
    }

    std::string path(modulePath);
    const std::size_t slashPos = path.find_last_of("\\/");
    if (slashPos == std::string::npos)
    {
        std::snprintf(g_moduleDirectory, sizeof(g_moduleDirectory), ".");
        return;
    }
    path.resize(slashPos);
    std::snprintf(g_moduleDirectory, sizeof(g_moduleDirectory), "%s", path.c_str());
}

std::string BuildArtifactPath(const char* extension)
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    char fileName[256] = {};
    std::snprintf(
        fileName,
        sizeof(fileName),
        "efz_netplay_mod_crash_%04u%02u%02u_%02u%02u%02u_pid%lu_tid%lu.%s",
        static_cast<unsigned>(st.wYear),
        static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay),
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond),
        static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()),
        extension);

    std::string path = g_moduleDirectory[0] != '\0' ? g_moduleDirectory : ".";
    path += "\\";
    path += fileName;
    return path;
}

// Read a DWORD safely using SEH — returns 0xDEADBEEF on fault.
// Isolated in its own function to avoid __try / C++ object unwinding conflict.
static uintptr_t SafeReadDword(uintptr_t addr)
{
    uintptr_t val = 0xDEADBEEFu;
    __try { val = *reinterpret_cast<const uintptr_t*>(addr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { val = 0xDEADBEEFu; }
    return val;
}

void WriteCrashInfoText(EXCEPTION_POINTERS* exceptionPointers, const char* reason, const char* dumpPath)
{
    const std::string txtPath = BuildArtifactPath("txt");
    FILE* file = nullptr;
    fopen_s(&file, txtPath.c_str(), "w");
    if (file == nullptr)
    {
        mod::Log("CrashHandler: failed to create crash text log path='%s'", txtPath.c_str());
        return;
    }

    const DWORD exceptionCode =
        (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr)
            ? exceptionPointers->ExceptionRecord->ExceptionCode
            : 0;
    const ULONG_PTR exceptionAddress =
        (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr)
            ? reinterpret_cast<ULONG_PTR>(exceptionPointers->ExceptionRecord->ExceptionAddress)
            : 0;

    std::fprintf(file, "EFZ Netplay Mod Crash Log\n");
    std::fprintf(file, "reason=%s\n", reason != nullptr ? reason : "unknown");
    std::fprintf(file, "mode=%s\n", g_injectedMode ? "injected_takeover" : "host");
    std::fprintf(file, "pid=%lu\n", static_cast<unsigned long>(GetCurrentProcessId()));
    std::fprintf(file, "tid=%lu\n", static_cast<unsigned long>(GetCurrentThreadId()));
    std::fprintf(file, "exception_code=0x%08lX\n", static_cast<unsigned long>(exceptionCode));
    std::fprintf(file, "exception_address=0x%p\n", reinterpret_cast<void*>(exceptionAddress));
    std::fprintf(file, "minidump=%s\n", dumpPath != nullptr ? dumpPath : "");

    // Module context: identify which module the crash address belongs to.
    {
        HMODULE crashModule = nullptr;
        char crashModuleName[MAX_PATH] = {};
        if (exceptionAddress != 0
            && GetModuleHandleExA(
                   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                   reinterpret_cast<LPCSTR>(exceptionAddress),
                   &crashModule))
        {
            GetModuleFileNameA(crashModule, crashModuleName, MAX_PATH);
            const uintptr_t modBase = reinterpret_cast<uintptr_t>(crashModule);
            const uintptr_t rva = exceptionAddress - modBase;
            std::fprintf(file, "crash_module=%s\n", crashModuleName);
            std::fprintf(file, "crash_module_base=0x%08lX\n", static_cast<unsigned long>(modBase));
            std::fprintf(file, "crash_rva=0x%08lX\n", static_cast<unsigned long>(rva));
        }

        // Revival DLL state.
        HMODULE revival = GetModuleHandleA("EfzRevival.dll");
        if (revival != nullptr)
        {
            const uintptr_t revBase = reinterpret_cast<uintptr_t>(revival);
            std::fprintf(file, "revival_base=0x%08lX\n", static_cast<unsigned long>(revBase));

            // Dump the render context global using the active version profile.
            const uintptr_t renderCtxOffset = netplay::bridge::GetRevivalRenderContextOffset();
            const uintptr_t renderCtxAddr = (renderCtxOffset != 0) ? revBase + renderCtxOffset : 0;
            const uintptr_t renderCtxVal = (renderCtxAddr != 0) ? SafeReadDword(renderCtxAddr) : 0;
            std::fprintf(file, "revival_renderCtx_addr=0x%08lX\n", static_cast<unsigned long>(renderCtxAddr));
            std::fprintf(file, "revival_renderCtx_value=0x%08lX\n", static_cast<unsigned long>(renderCtxVal));

            // Dump the session pointer using the active version profile.
            const uintptr_t sessionPtrOffset = netplay::bridge::GetRevivalSessionPtrOffset();
            const uintptr_t sessionAddr = (sessionPtrOffset != 0) ? revBase + sessionPtrOffset : 0;
            const uintptr_t sessionVal = (sessionAddr != 0) ? SafeReadDword(sessionAddr) : 0;
            std::fprintf(file, "revival_session_addr=0x%08lX\n", static_cast<unsigned long>(sessionAddr));
            std::fprintf(file, "revival_session_value=0x%08lX\n", static_cast<unsigned long>(sessionVal));

            // If session pointer is readable, dump its vtable.
            if (sessionVal != 0 && sessionVal != 0xDEADBEEFu)
            {
                const uintptr_t vtableVal = SafeReadDword(sessionVal);
                std::fprintf(file, "revival_session_vtable=0x%08lX\n", static_cast<unsigned long>(vtableVal));
            }

            // If renderCtx pointer is readable, dump its vtable.
            if (renderCtxVal != 0 && renderCtxVal != 0xDEADBEEFu)
            {
                const uintptr_t vtableVal = SafeReadDword(renderCtxVal);
                std::fprintf(file, "revival_renderCtx_vtable=0x%08lX\n", static_cast<unsigned long>(vtableVal));
            }
        }

        // Our mod DLL.
        HMODULE ourMod = GetModuleHandleA("efz_netplay_mod.dll");
        if (ourMod != nullptr)
        {
            std::fprintf(file, "mod_base=0x%08lX\n",
                         static_cast<unsigned long>(reinterpret_cast<uintptr_t>(ourMod)));
        }
    }

    // -----------------------------------------------------------------------
    // EFZ.exe game mode struct table dump
    //
    // The game mode struct table at 0x790110 holds up to 14 object pointers.
    // 0x790148 holds the current index.  EFZ_GameMode_InvokeAdvance() calls
    // vtable[1] on table[curIdx] — if corrupt, this is the crash site.
    // -----------------------------------------------------------------------
    {
        constexpr uintptr_t kTableAddr = 0x00790110u;
        constexpr uintptr_t kIndexAddr = 0x00790148u;
        constexpr int kMaxEntries = 14;

        const uintptr_t curIdx = SafeReadDword(kIndexAddr);
        std::fprintf(file, "\n=== Game Mode Struct Table ===\n");
        std::fprintf(file, "game_mode_index=0x%08lX (%ld)\n",
                     static_cast<unsigned long>(curIdx),
                     static_cast<long>(static_cast<int>(curIdx)));

        HMODULE revival = GetModuleHandleA("EfzRevival.dll");
        const uintptr_t revBase = revival
            ? reinterpret_cast<uintptr_t>(revival) : 0;

        for (int i = 0; i < kMaxEntries; ++i)
        {
            const uintptr_t entry = SafeReadDword(kTableAddr + 4u * i);
            if (entry == 0 || entry == 0xDEADBEEFu)
                continue;

            const uintptr_t vtable = SafeReadDword(entry);
            const uintptr_t vt0 = SafeReadDword(vtable);
            const uintptr_t vt1 = SafeReadDword(vtable + 4);
            const uintptr_t vt2 = SafeReadDword(vtable + 8);

            const uintptr_t vtRva = (revBase != 0 && vtable >= revBase
                                     && vtable < (revBase + 0x100000u))
                                        ? (vtable - revBase) : 0;

            std::fprintf(file,
                "game_mode_table[%d]=0x%08lX vtable=0x%08lX (RVA=0x%lX) "
                "vt[0]=0x%08lX vt[1]=0x%08lX vt[2]=0x%08lX%s\n",
                i,
                static_cast<unsigned long>(entry),
                static_cast<unsigned long>(vtable),
                static_cast<unsigned long>(vtRva),
                static_cast<unsigned long>(vt0),
                static_cast<unsigned long>(vt1),
                static_cast<unsigned long>(vt2),
                (static_cast<unsigned>(i) == (curIdx & 0xFFu)) ? " <<<CURRENT" : "");

            // Dump first 64 bytes of the current game mode object for analysis.
            if (static_cast<unsigned>(i) == (curIdx & 0xFFu))
            {
                std::fprintf(file, "  object_hex_dump:\n");
                for (int off = 0; off < 64; off += 16)
                {
                    const uintptr_t d0 = SafeReadDword(entry + off);
                    const uintptr_t d1 = SafeReadDword(entry + off + 4);
                    const uintptr_t d2 = SafeReadDword(entry + off + 8);
                    const uintptr_t d3 = SafeReadDword(entry + off + 12);
                    std::fprintf(file,
                        "    +0x%02X: %08lX %08lX %08lX %08lX\n",
                        off,
                        static_cast<unsigned long>(d0),
                        static_cast<unsigned long>(d1),
                        static_cast<unsigned long>(d2),
                        static_cast<unsigned long>(d3));
                }
            }
        }
    }

    if (exceptionPointers != nullptr && exceptionPointers->ContextRecord != nullptr)
    {
        const CONTEXT* ctx = exceptionPointers->ContextRecord;
#if defined(_M_IX86)
        std::fprintf(file, "EIP=0x%08lX\n", static_cast<unsigned long>(ctx->Eip));
        std::fprintf(file, "ESP=0x%08lX\n", static_cast<unsigned long>(ctx->Esp));
        std::fprintf(file, "EBP=0x%08lX\n", static_cast<unsigned long>(ctx->Ebp));
        std::fprintf(file, "EAX=0x%08lX\n", static_cast<unsigned long>(ctx->Eax));
        std::fprintf(file, "EBX=0x%08lX\n", static_cast<unsigned long>(ctx->Ebx));
        std::fprintf(file, "ECX=0x%08lX\n", static_cast<unsigned long>(ctx->Ecx));
        std::fprintf(file, "EDX=0x%08lX\n", static_cast<unsigned long>(ctx->Edx));
        std::fprintf(file, "ESI=0x%08lX\n", static_cast<unsigned long>(ctx->Esi));
        std::fprintf(file, "EDI=0x%08lX\n", static_cast<unsigned long>(ctx->Edi));
#elif defined(_M_X64)
        std::fprintf(file, "RIP=0x%016llX\n", static_cast<unsigned long long>(ctx->Rip));
        std::fprintf(file, "RSP=0x%016llX\n", static_cast<unsigned long long>(ctx->Rsp));
        std::fprintf(file, "RBP=0x%016llX\n", static_cast<unsigned long long>(ctx->Rbp));
        std::fprintf(file, "RAX=0x%016llX\n", static_cast<unsigned long long>(ctx->Rax));
        std::fprintf(file, "RBX=0x%016llX\n", static_cast<unsigned long long>(ctx->Rbx));
        std::fprintf(file, "RCX=0x%016llX\n", static_cast<unsigned long long>(ctx->Rcx));
        std::fprintf(file, "RDX=0x%016llX\n", static_cast<unsigned long long>(ctx->Rdx));
        std::fprintf(file, "RSI=0x%016llX\n", static_cast<unsigned long long>(ctx->Rsi));
        std::fprintf(file, "RDI=0x%016llX\n", static_cast<unsigned long long>(ctx->Rdi));
#endif
    }

    std::fclose(file);
    mod::Log("CrashHandler: wrote crash text log '%s'", txtPath.c_str());
}

void WriteCrashArtifacts(EXCEPTION_POINTERS* exceptionPointers, const char* reason)
{
    std::lock_guard<std::mutex> lock(g_crashMutex);

    if (g_dumpWritten.exchange(true))
    {
        return;
    }

    const std::string dmpPath = BuildArtifactPath("dmp");
    HANDLE dumpFile = CreateFileA(
        dmpPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    bool dumpOk = false;
    if (dumpFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = exceptionPointers;
        mei.ClientPointers = FALSE;

        if (ResolveMiniDumpWriteDump())
        {
            const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
                MiniDumpWithDataSegs |
                MiniDumpWithHandleData |
                MiniDumpWithThreadInfo |
                MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpScanMemory);

            dumpOk = g_miniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                dumpFile,
                dumpType,
                exceptionPointers != nullptr ? &mei : nullptr,
                nullptr,
                nullptr)
                == TRUE;
        }

        CloseHandle(dumpFile);
    }

    mod::Log(
        "CrashHandler: captured exception reason=%s code=0x%08lX addr=0x%p dump=%d path='%s'",
        reason != nullptr ? reason : "unknown",
        static_cast<unsigned long>(
            (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr)
                ? exceptionPointers->ExceptionRecord->ExceptionCode
                : 0),
        (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr)
            ? exceptionPointers->ExceptionRecord->ExceptionAddress
            : nullptr,
        dumpOk ? 1 : 0,
        dmpPath.c_str());

    WriteCrashInfoText(exceptionPointers, reason, dumpOk ? dmpPath.c_str() : "");
}

LONG WINAPI VectoredExceptionThunk(EXCEPTION_POINTERS* exceptionPointers)
{
    if (exceptionPointers == nullptr || exceptionPointers->ExceptionRecord == nullptr)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const DWORD code = exceptionPointers->ExceptionRecord->ExceptionCode;
    if (!IsCrashCode(code))
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // ---------------------------------------------------------------------------
    // TOCTOU netplay recovery
    //
    // Scenario: IsPeerProcessAlive() returned true so we returned global state=1
    // to EFZ.exe; the peer died between that check and EFZ.exe calling the DLL
    // rollback tick in state-4 (VS Human in-game).  NeutralizeExitProcess ran on
    // the main thread (frameJmpActive=0 — game-state-4 dispatches DLL sessions
    // via a direct vtable call, not through 0x401582/OurFrameDispatch), neutralised
    // the session vtable, and returned.  The instruction after 'call ExitProcess'
    // in EFZ_Main_RollbackLoopTick is a privileged instruction placed by the
    // compiler as unreachable marker code — executing it raises
    // STATUS_PRIV_INSTRUCTION (0xC0000096).
    //
    // Recovery: simulate 'leave; ret' from the crashing function, returning
    // cleanly to EFZ.exe's game loop.  The session vtable is already neutralised
    // (all subsequent DLL tick calls are no-ops).  g_revivalExitIntercepted is
    // set, so ConsumeRevivalExitInterception fires from HookedTitleUpdateImplBody
    // when EFZ.exe eventually returns to state-0 (e.g. player presses ESC),
    // which re-enters the netplay menu and completes the teardown sequence.
    // ---------------------------------------------------------------------------
#if defined(_M_IX86)
    if (code == EXCEPTION_PRIV_INSTRUCTION && !g_injectedMode)
    {
        // One-shot: only attempt TOCTOU recovery once.  Without this guard
        // the VEH would fire again if DLL code at the redirected address
        // also faults (partially-unwound DLL frames executing stale state).
        if (!g_toctouRecoveryFired.exchange(true)
            && netplay::bridge::IsNetplayExitInterceptionPending())
        {
            const ULONG_PTR crashAddr =
                reinterpret_cast<ULONG_PTR>(exceptionPointers->ExceptionRecord->ExceptionAddress);
            HMODULE revival = GetModuleHandleA("EfzRevival.dll");
            if (revival != nullptr)
            {
                // Confirm crash is inside EfzRevival.dll.
                MEMORY_BASIC_INFORMATION mbi = {};
                const bool inRevivalDll =
                    (VirtualQuery(reinterpret_cast<LPCVOID>(crashAddr),
                                  &mbi, sizeof(mbi)) != 0)
                    && (mbi.AllocationBase == static_cast<PVOID>(revival));
                if (inRevivalDll)
                {
                    const uintptr_t revBase = reinterpret_cast<uintptr_t>(revival);
                    CONTEXT* ctx = exceptionPointers->ContextRecord;

                    // Walk the EBP chain until we find a return address that
                    // is NOT inside EfzRevival.dll.  A simple one-frame
                    // leave;ret lands back in other DLL code which may also
                    // fault, causing the VEH to loop.  By walking past all
                    // DLL frames in one step we return directly into
                    // EFZ.exe's game loop, which can continue cleanly with
                    // the already-neutralised dummy vtable.
                    uintptr_t walkEbp = ctx->Ebp;
                    uintptr_t foundRet = 0;
                    uintptr_t foundEbp = 0;
                    uintptr_t foundEsp = 0;
                    bool foundExeFrame = false;
                    int depth = 0;

                    for (; depth < 40; ++depth)
                    {
                        const uintptr_t curSavedEbp = SafeReadDword(walkEbp);
                        const uintptr_t curRet      = SafeReadDword(walkEbp + 4);

                        if (curRet == 0xDEADBEEFu || curRet == 0
                            || curSavedEbp == 0xDEADBEEFu
                            || curSavedEbp <= walkEbp)
                        {
                            break; // unreadable or non-standard frame
                        }

                        // Check whether curRet is in EfzRevival.dll.
                        MEMORY_BASIC_INFORMATION retMbi = {};
                        const bool retInRevival =
                            (VirtualQuery(reinterpret_cast<LPCVOID>(curRet),
                                          &retMbi, sizeof(retMbi)) != 0)
                            && (retMbi.AllocationBase == static_cast<PVOID>(revival));

                        if (!retInRevival)
                        {
                            // This frame's return address is outside
                            // EfzRevival.dll — it's EFZ.exe (or our mod DLL).
                            foundRet = curRet;
                            foundEbp = curSavedEbp;
                            foundEsp = walkEbp + 8; // EBP+4 = retaddr, +4 = size
                            foundExeFrame = true;
                            break;
                        }

                        walkEbp = curSavedEbp;
                    }

                    if (foundExeFrame)
                    {
                        mod::Log(
                            "CrashHandler: TOCTOU netplay recovery — "
                            "walked %d DLL frame(s) from RVA 0x%lX, "
                            "resuming at EXE addr 0x%08lX "
                            "(EBP 0x%08lX ESP 0x%08lX)",
                            depth,
                            static_cast<unsigned long>(crashAddr - revBase),
                            static_cast<unsigned long>(foundRet),
                            static_cast<unsigned long>(foundEbp),
                            static_cast<unsigned long>(foundEsp));

                        // NeutralizeExitProcess already installed the dummy
                        // vtable on the session object before the hlt fired.
                        // Do NOT call ForceLocalPlayInit here: creating a fresh
                        // local-play session at this point means that session
                        // will also eventually call ExitProcess (outside
                        // OurFrameDispatch's setjmp scope), producing a second
                        // hlt that the one-shot VEH can no longer catch.
                        //
                        // The dummy vtable returns 0 safely from every method,
                        // so EFZ.exe's char-select / game loop can continue
                        // cleanly (all DLL session calls are no-ops).  The
                        // title-screen hook will call ConsumeRevivalExitInterception
                        // on the first mode-0 frame, which does the full proper
                        // tear-down (TerminateProcess / patch restore /
                        // ForceLocalPlayInit / text disable) in a controlled way.
                        //
                        // Force the game mode to title screen (0) so that the
                        // title-screen hook runs immediately on the next main-
                        // loop iteration instead of waiting for the match to
                        // end naturally.
                        const bool modeForced = netplay::bridge::ForceGameModeToTitle();
                        mod::Log(
                            "CrashHandler: TOCTOU recovery — ForceGameModeToTitle "
                            "result=%d",
                            modeForced ? 1 : 0);

                        ctx->Eip = static_cast<DWORD>(foundRet);
                        ctx->Esp = static_cast<DWORD>(foundEsp);
                        ctx->Ebp = static_cast<DWORD>(foundEbp);
                        ctx->Eax = 0;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }

                    mod::Log(
                        "CrashHandler: TOCTOU netplay recovery — "
                        "could not find EXE frame after %d steps "
                        "(crashRVA=0x%lX EBP=0x%08lX), falling through",
                        depth,
                        static_cast<unsigned long>(crashAddr - revBase),
                        static_cast<unsigned long>(ctx->Ebp));
                }
            }
        }
    }
#endif

    // Do NOT write crash artifacts here.  The VEH fires for ALL exceptions
    // that match IsCrashCode(), including those that are subsequently handled
    // by frame-based SEH handlers (e.g. guard pages, copy-on-write, internal
    // library exception flow).  Writing here produces spurious empty crash
    // files when the game hasn't actually crashed.
    // Crash artifacts are written only from UnhandledExceptionThunk, which
    // fires exclusively for truly fatal, unhandled exceptions.
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI UnhandledExceptionThunk(EXCEPTION_POINTERS* exceptionPointers)
{
    WriteCrashArtifacts(exceptionPointers, "unhandled");
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

namespace mod
{
void InstallCrashHandlers(HMODULE moduleHandle, bool injectedTakeoverMode)
{
    std::lock_guard<std::mutex> lock(g_crashMutex);

    g_injectedMode = injectedTakeoverMode;
    g_dumpWritten = false;
    ResolveModuleDirectory(moduleHandle);

    if (g_vectoredHandle == nullptr)
    {
        g_vectoredHandle = AddVectoredExceptionHandler(1, VectoredExceptionThunk);
    }

    g_previousUnhandledFilter = SetUnhandledExceptionFilter(UnhandledExceptionThunk);
    mod::Log(
        "CrashHandler: installed (mode=%s dir='%s' vectored=%p)",
        g_injectedMode ? "injected_takeover" : "host",
        g_moduleDirectory,
        g_vectoredHandle);
}

void UninstallCrashHandlers()
{
    std::lock_guard<std::mutex> lock(g_crashMutex);

    if (g_vectoredHandle != nullptr)
    {
        RemoveVectoredExceptionHandler(g_vectoredHandle);
        g_vectoredHandle = nullptr;
    }

    SetUnhandledExceptionFilter(g_previousUnhandledFilter);
    g_previousUnhandledFilter = nullptr;

    if (g_dbgHelpModule != nullptr)
    {
        FreeLibrary(g_dbgHelpModule);
        g_dbgHelpModule = nullptr;
        g_miniDumpWriteDump = nullptr;
    }

    mod::Log("CrashHandler: uninstalled");
}

void ResetCrashRecoveryState()
{
    g_toctouRecoveryFired.store(false);
    mod::Log("CrashHandler: TOCTOU recovery guard reset");
}
} // namespace mod
