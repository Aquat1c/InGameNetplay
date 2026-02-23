#include "crash_handler.h"

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
std::atomic<bool> g_dumpWritten{false};
HMODULE g_dbgHelpModule = nullptr;
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

    WriteCrashArtifacts(exceptionPointers, "vectored");
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
} // namespace mod
