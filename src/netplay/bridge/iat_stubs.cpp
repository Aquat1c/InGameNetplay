// IAT stub implementations and extern "C" nb_stub_* wrappers.

#include "netplay/bridge/takeover_internal.h"
#include "crash_handler.h"

#include <array>
#include <cctype>
#include <cstring>
#include <intrin.h>
#include <string>
#include <cwchar>

#include <windows.h>

namespace netplay::bridge::takeover
{

static std::string GetTakeoverModuleDirectoryA()
{
    char modulePath[MAX_PATH] = {};
    HMODULE selfModule = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetTakeoverModuleDirectoryA),
        &selfModule);
    if (selfModule == nullptr || GetModuleFileNameA(selfModule, modulePath, MAX_PATH) == 0)
    {
        return {};
    }

    std::string dir(modulePath);
    const size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos)
    {
        return {};
    }
    dir.resize(slash);
    return dir;
}

static std::wstring GetTakeoverModuleDirectoryW()
{
    wchar_t modulePath[MAX_PATH] = {};
    HMODULE selfModule = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetTakeoverModuleDirectoryW),
        &selfModule);
    if (selfModule == nullptr || GetModuleFileNameW(selfModule, modulePath, MAX_PATH) == 0)
    {
        return {};
    }

    std::wstring dir(modulePath);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
    {
        return {};
    }
    dir.resize(slash);
    return dir;
}

static bool EnsureDirectoryExistsA(const std::string& path)
{
    if (path.empty())
    {
        return false;
    }

    if (CreateDirectoryA(path.c_str(), nullptr) != FALSE)
    {
        return true;
    }

    const DWORD error = GetLastError();
    return error == ERROR_ALREADY_EXISTS;
}

static bool EnsureDirectoryExistsW(const std::wstring& path)
{
    if (path.empty())
    {
        return false;
    }

    if (CreateDirectoryW(path.c_str(), nullptr) != FALSE)
    {
        return true;
    }

    const DWORD error = GetLastError();
    return error == ERROR_ALREADY_EXISTS;
}

static bool HasLogEfzBaseNameA(LPCSTR path)
{
    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    const char* baseName = path;
    for (const char* p = path; *p != '\0'; ++p)
    {
        if (*p == '\\' || *p == '/')
        {
            baseName = p + 1;
        }
    }

    return _stricmp(baseName, "logEfz.txt") == 0;
}

static bool HasLogEfzBaseNameW(LPCWSTR path)
{
    if (path == nullptr || path[0] == L'\0')
    {
        return false;
    }

    const wchar_t* baseName = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p)
    {
        if (*p == L'\\' || *p == L'/')
        {
            baseName = p + 1;
        }
    }

    return _wcsicmp(baseName, L"logEfz.txt") == 0;
}

static std::string GetNativeShadowLogEfzPathA()
{
    const std::string dir = GetTakeoverModuleDirectoryA();
    if (dir.empty())
    {
        return {};
    }

    const char* const subdir = IsCurrentProcessRevival() ? "native_revival" : "native_host";
    const std::string shadowDir = dir + "\\" + subdir;
    if (!EnsureDirectoryExistsA(shadowDir))
    {
        return {};
    }

    return shadowDir + "\\logEfz.txt";
}

static std::wstring GetNativeShadowLogEfzPathW()
{
    const std::wstring dir = GetTakeoverModuleDirectoryW();
    if (dir.empty())
    {
        return {};
    }

    const wchar_t* const subdir = IsCurrentProcessRevival() ? L"native_revival" : L"native_host";
    const std::wstring shadowDir = dir + L"\\" + subdir;
    if (!EnsureDirectoryExistsW(shadowDir))
    {
        return {};
    }

    return shadowDir + L"\\logEfz.txt";
}

// ---------------------------------------------------------------------------
// Dummy vtable for neutralized Revival session objects.
//
// When the mod intercepts ExitProcess from the Revival DLL, it overwrites the
// session object's vtable pointer with g_revivalDummyVtable.  Every slot
// must match the calling convention of the original vtable method so that the
// stack is correctly cleaned on return.
//
// Slot layout (from decompiled Revival DLL 1.02e):
//   [0] destructor  (__thiscall, 1 extra stack param)  → ret 4
//   [1] init        (__thiscall, 0 extra)              → ret
//   [2] tick        (__cdecl,   1 stack param)         → ret
//   [3] hotkey      (__thiscall, 1 extra)              → ret 4
//   [4] input       (__thiscall, 2 extra)              → ret 8
//   [5] net data    (__thiscall, 1 extra)              → ret 4
//   [6] character   (__thiscall, 2 extra)              → ret 8
//   [7] action      (__thiscall, 2 extra)              → ret 8
//   [8] action      (__thiscall, 1 extra)              → ret 4
// ---------------------------------------------------------------------------

#if defined(_MSC_VER) && defined(_M_IX86)

__declspec(naked) static void DummyVtableRet()
{
    __asm {
        xor eax, eax
        ret
    }
}

__declspec(naked) static void DummyVtableRet4()
{
    __asm {
        xor eax, eax
        ret 4
    }
}

__declspec(naked) static void DummyVtableRet8()
{
    __asm {
        xor eax, eax
        ret 8
    }
}

#else
// Fallback for non-MSVC or x64 — should never be reached in practice.
static int  __cdecl DummyVtableRet()    { return 0; }
static int  __cdecl DummyVtableRet4()   { return 0; }
static int  __cdecl DummyVtableRet8()   { return 0; }
#endif

static uintptr_t g_revivalDummyVtable[9] = { 0 };
static bool g_dummyVtableInitialized = false;

static void EnsureDummyVtable()
{
    if (g_dummyVtableInitialized)
    {
        return;
    }

    const uintptr_t ret  = reinterpret_cast<uintptr_t>(DummyVtableRet);
    const uintptr_t ret4 = reinterpret_cast<uintptr_t>(DummyVtableRet4);
    const uintptr_t ret8 = reinterpret_cast<uintptr_t>(DummyVtableRet8);

    g_revivalDummyVtable[0] = ret4; // [0] destructor
    g_revivalDummyVtable[1] = ret;  // [1] init
    g_revivalDummyVtable[2] = ret;  // [2] tick
    g_revivalDummyVtable[3] = ret4; // [3] hotkey
    g_revivalDummyVtable[4] = ret8; // [4] input
    g_revivalDummyVtable[5] = ret4; // [5] net data
    g_revivalDummyVtable[6] = ret8; // [6] character
    g_revivalDummyVtable[7] = ret8; // [7] action
    g_revivalDummyVtable[8] = ret4; // [8] action

    g_dummyVtableInitialized = true;
}

// ---------------------------------------------------------------------------
// NeutralizeRevivalSessionVtable — overwrite the Revival DLL's active session
// object vtable pointer with the dummy vtable.  This makes every per-frame
// dispatch a harmless no-op, preventing further ExitProcess triggers.
// ---------------------------------------------------------------------------
void NeutralizeRevivalSessionVtable()
{
    EnsureDummyVtable();

    HMODULE revival = g_localRevivalModule;
    if (revival == nullptr)
    {
        return;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    if (g_activeRevival == nullptr || g_activeRevival->sessionPtrOffsetCount == 0)
    {
        return;
    }

    const uintptr_t sessionGlobalAddr = base + g_activeRevival->sessionPtrOffsets[0];
    uintptr_t sessionPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionGlobalAddr), &sessionPtr) || sessionPtr == 0)
    {
        return;
    }

    // The first DWORD of the session object is the vtable pointer.
    // A single aligned 4-byte write is atomic on x86.
    auto* vtableSlot = reinterpret_cast<uintptr_t*>(sessionPtr);
    *vtableSlot = reinterpret_cast<uintptr_t>(&g_revivalDummyVtable[0]);
}

// ---------------------------------------------------------------------------
// ExitProcess interception — installed into EfzRevival.dll's IAT.
//
// ExitProcess is __noreturn — the compiler emits no valid code past the call.
// The primary defense against ExitProcess is patching the conditional-jump
// bytes guarding each call site in the DLL binary (see
// SaveAndApplyDllExitProcessPatches).  This makes the calls unreachable.
//
// The IAT hook remains as a safety net: if an unpatched code path somehow
// calls ExitProcess, we do cleanup and suspend the thread forever.
// ---------------------------------------------------------------------------

typedef VOID (WINAPI *ExitProcessFn)(UINT uExitCode);
static ExitProcessFn g_realExitProcess = nullptr;

bool SignalGracefulQuitRing(const char* contextTag, uintptr_t callerRva)
{
    HANDLE hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "Quit");
    if (hMap == nullptr)
    {
        mod::Log(
            "GracefulQuitRing: skipped (%s) callerRva=0x%lX "
            "OpenFileMappingA('Quit') failed err=%lu",
            contextTag != nullptr ? contextTag : "",
            static_cast<unsigned long>(callerRva),
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    auto* view = static_cast<volatile uint8_t*>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 9));
    if (view == nullptr)
    {
        const DWORD mapErr = GetLastError();
        CloseHandle(hMap);
        mod::Log(
            "GracefulQuitRing: skipped (%s) callerRva=0x%lX "
            "MapViewOfFile('Quit') failed err=%lu",
            contextTag != nullptr ? contextTag : "",
            static_cast<unsigned long>(callerRva),
            static_cast<unsigned long>(mapErr));
        return false;
    }

    auto* header = reinterpret_cast<volatile LONG*>(const_cast<uint8_t*>(view));
    const LONG headBefore = header[0];
    const LONG tailBefore = header[1];
    const bool alreadyQueued = headBefore != tailBefore;

    if (!alreadyQueued)
    {
        view[8] = 1;
        MemoryBarrier();
        InterlockedExchange(&header[1], tailBefore + 1);
    }

    const LONG headAfter = header[0];
    const LONG tailAfter = header[1];

    UnmapViewOfFile(const_cast<uint8_t*>(view));
    CloseHandle(hMap);

    mod::Log(
        "GracefulQuitRing: %s (%s) callerRva=0x%lX "
        "head=%ld->%ld tail=%ld->%ld",
        alreadyQueued ? "already pending" : "queued",
        contextTag != nullptr ? contextTag : "",
        static_cast<unsigned long>(callerRva),
        static_cast<long>(headBefore),
        static_cast<long>(headAfter),
        static_cast<long>(tailBefore),
        static_cast<long>(tailAfter));
    return true;
}

static VOID WINAPI NeutralizeExitProcess(UINT uExitCode)
{
    // Capture caller context for diagnostics before any side-effects.
    const void* callerAddr = _ReturnAddress();
    uintptr_t callerRva = 0;
    const char* callerModule = "unknown";
    {
        HMODULE revival = g_localRevivalModule;
        if (revival != nullptr)
        {
            const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
            const uintptr_t caller = reinterpret_cast<uintptr_t>(callerAddr);
            if (caller >= base && caller < base + 0x200000u)
            {
                callerRva = caller - base;
                callerModule = "EfzRevival.dll";
            }
        }
    }
    int currentScreenIndex = -1;
    if (g_activeRevival != nullptr && g_activeRevival->addrGameModeCurrentIndex != 0)
    {
        SafeReadInt(
            reinterpret_cast<const void*>(g_activeRevival->addrGameModeCurrentIndex),
            &currentScreenIndex);
    }

    // First interception: neutralize the session vtable and capture the role.
    if (InterlockedExchange(&g_revivalExitIntercepted, 1) == 0)
    {
        const int role = g_localRoleFlag;
        InterlockedExchange(&g_revivalExitMode, static_cast<LONG>(role));

        mod::Log(
            "NeutralizeExitProcess: intercepted code=%u role=%d screen=%d "
            "caller=%s+0x%lX (%p) frameJmpActive=%d uiJmpActive=%d",
            uExitCode, role, currentScreenIndex,
            callerModule, static_cast<unsigned long>(callerRva), callerAddr,
            g_netplayFrameJmpActive ? 1 : 0,
            g_netplayUiJmpActive ? 1 : 0);

        if (ConsumeOnlineMatchEscGracefulQuit())
        {
            // Match-local ESC was armed on the live battle screen. Ask the
            // injected helper to run its native "send MessageQuit to every
            // peer" path before we tear the helper process down from the host.
            mod::Log(
                "NeutralizeExitProcess: online match ESC broadcast request "
                "role=%d screen=%d helperPid=%lu caller=%s+0x%lX",
                role,
                currentScreenIndex,
                static_cast<unsigned long>(g_revivalProcessId),
                callerModule,
                static_cast<unsigned long>(callerRva));
            const bool peerQuitSent =
                RequestInjectedPeerQuitBroadcast("online_match_esc", 300u);
            mod::Log(
                "NeutralizeExitProcess: online match ESC peer-quit broadcast=%d "
                "caller=%s+0x%lX",
                peerQuitSent ? 1 : 0,
                callerModule,
                static_cast<unsigned long>(callerRva));
            if (!peerQuitSent)
            {
                mod::Log(
                    "NeutralizeExitProcess: online match ESC broadcast failed; "
                    "continuing with local cleanup and helper teardown");
            }
        }

        if (role == kLocalRoleTournament)
        {
            ClearRevivalText();
        }

        NeutralizeRevivalSessionVtable();
        mod::Log(
            "NeutralizeExitProcess: vtable neutralised code=%u role=%d",
            uExitCode, role);
    }

    // For netplay (online/spectate) sessions, ExitProcess is called on the
    // EFZ.exe main game thread when the peer process terminates.  Unlike
    // tournament (where Jcc patches make ExitProcess unreachable), netplay has
    // no such patches.  If OurFrameDispatch (which wraps sub_1006E590) has
    // set a setjmp recovery point, use longjmp to escape without freezing the
    // main thread.  The title-screen hook will then consume the interception
    // flag and re-enter the netplay menu on the next mode-0 frame.
    if (g_netplayFrameJmpActive)
    {
        mod::Log(
            "NeutralizeExitProcess: longjmp — returning control to game "
            "thread (role=%d)",
            g_localRoleFlag);
        g_netplayFrameJmpActive = false;
        longjmp(g_netplayFrameJmpBuf, 1);
        // longjmp does not return.
    }

    // Fallback: during title/menu/charselect update flow the frame hook's
    // setjmp is not active. If ExitProcess fires there (common in spectate/join
    // error paths, or early desync during charselect), use the UI-update
    // recovery context instead of returning into unknown compiler-generated
    // post-call code.
    if (g_netplayUiJmpActive)
    {
        mod::Log(
            "NeutralizeExitProcess: ui longjmp — escaping title/menu path "
            "(role=%d)",
            g_localRoleFlag);
        g_netplayUiJmpActive = false;
        longjmp(g_netplayUiJmpBuf, 1);
        // longjmp does not return.
    }

    // For online/spectate: if neither longjmp context is active, ExitProcess
    // was called from a DLL code path that isn't covered by any setjmp
    // (e.g. a direct vtable call from the EXE game loop during a state
    // transition, BEFORE HookedCharSelectUpdateImpl runs for the first time).
    //
    // All ExitProcess call sites in the DLL should be made unreachable by
    // SaveAndApplyDllExitProcessPatches.  If we reach here, there is an
    // unpatched site.  Do full cleanup to keep the game alive, then force
    // the game mode back to title screen and suspend this thread forever
    // (ExitProcess is __noreturn; the compiler emits no valid code after the
    // call site, so we MUST NOT return).
    const int currentRole = g_localRoleFlag;
    if (currentRole == kLocalRoleOnline || currentRole == kLocalRoleSpectate)
    {
        mod::Log(
            "NeutralizeExitProcess: no jmp recovery for online role=%d screen=%d "
            "caller=%s+0x%lX (%p) — performing inline cleanup (TOCTOU last resort)",
            currentRole, currentScreenIndex,
            callerModule, static_cast<unsigned long>(callerRva), callerAddr);

        // Step 1: Reinstate a live local-play session so the game loop has
        // a valid vtable for subsequent frame dispatches.
        const bool initOk = ForceLocalPlayInit();
        mod::Log(
            "NeutralizeExitProcess: TOCTOU step 1 ForceLocalPlayInit result=%d",
            initOk ? 1 : 0);

        // Step 2: Terminate the dead helper process and close its handle.
        if (g_revivalProcess != nullptr)
        {
            const BOOL termOk = TerminateProcess(g_revivalProcess, 0);
            CloseHandle(g_revivalProcess);
            g_revivalProcess = nullptr;
            g_revivalProcessId = 0;
            mod::Log(
                "NeutralizeExitProcess: TOCTOU step 2 helper terminated=%d",
                termOk ? 1 : 0);
        }

        // Step 3: Restore DLL Jcc patches.
        const bool patchOk = RestoreDllExitProcessPatches();
        mod::Log(
            "NeutralizeExitProcess: TOCTOU step 3 RestoreDllExitProcessPatches=%d",
            patchOk ? 1 : 0);

        // Step 4: Disable stale text overlays.
        DisableRevivalTextRendering();

        // Step 5: Reset VEH one-shot guard.
        mod::ResetCrashRecoveryState();

        // Step 6: Force game mode to title screen.
        const bool modeOk = ForceGameModeToTitle();
        mod::Log(
            "NeutralizeExitProcess: TOCTOU step 6 ForceGameModeToTitle=%d",
            modeOk ? 1 : 0);

        // ExitProcess is __noreturn.  The DLL code after `call ExitProcess`
        // is a compiler-emitted unreachable marker (HLT / privileged insn).
        // We MUST NOT return.  Suspend this thread forever; the VEH handler
        // will catch the resulting StillActive when ForceGameModeToTitle
        // transitions back to the title screen on the next game-loop tick.
        //
        // NOTE: If the ExitProcess call happened on the main game thread,
        // suspending it here will freeze the game.  The VEH handler should
        // detect this and TOCTOU-recover normally since we've already done
        // full cleanup.
        mod::Log(
            "NeutralizeExitProcess: TOCTOU cleanup complete, suspending thread "
            "(role=%d)",
            currentRole);
        SuspendThread(GetCurrentThread());
        // If resumed, just sleep forever.
        while (true) { Sleep(INFINITE); }
    }
    // No recovery point active.  For tournament mode, Jcc patches normally
    // prevent ExitProcess from being reached.  If we land here, the patches
    // failed (e.g. wrong version profile) or an unexpected DLL code path
    // fired.  Do full cleanup so the game can continue from the title screen.
    if (currentRole == kLocalRoleTournament)
    {
        mod::Log(
            "NeutralizeExitProcess: tournament fallback cleanup — no jmp "
            "recovery role=%d screen=%d caller=%s+0x%lX (%p)",
            currentRole, currentScreenIndex,
            callerModule, static_cast<unsigned long>(callerRva), callerAddr);

        // Step 1: Restore DLL Jcc patches (prevents recursive ExitProcess
        // during the ForceLocalPlayInit below).
        const bool patchOk = RestoreDllExitProcessPatches();
        mod::Log(
            "NeutralizeExitProcess: tournament step 1 RestoreDllExitProcessPatches=%d",
            patchOk ? 1 : 0);

        // Step 2: Restore tournament-specific EXE patches.
        const bool exeOk = RestoreTournamentExePatches();
        mod::Log(
            "NeutralizeExitProcess: tournament step 2 RestoreTournamentExePatches=%d",
            exeOk ? 1 : 0);

        // Step 3: Reinstate a live local-play session.
        const bool initOk = ForceLocalPlayInit();
        mod::Log(
            "NeutralizeExitProcess: tournament step 3 ForceLocalPlayInit=%d",
            initOk ? 1 : 0);

        // Step 4: Disable stale text overlays.
        DisableRevivalTextRendering();

        // Step 5: Reset VEH one-shot guard and game-mode validation.
        mod::ResetCrashRecoveryState();
        ResetGameModeValidation();

        // Step 6: Force game mode to title screen.
        const bool modeOk = ForceGameModeToTitle();
        mod::Log(
            "NeutralizeExitProcess: tournament step 6 ForceGameModeToTitle=%d",
            modeOk ? 1 : 0);

        // Reset tournament role so the title-screen code doesn't think
        // we're still in tournament mode.
        g_localRoleFlag = kLocalRoleLocalPlay;

        mod::Log(
            "NeutralizeExitProcess: tournament cleanup complete, "
            "suspending thread (role=%d)",
            currentRole);
        SuspendThread(GetCurrentThread());
        while (true) { Sleep(INFINITE); }
    }

    // Truly unguarded path — unknown role or unexpected state.
    mod::Log(
        "NeutralizeExitProcess: no longjmp recovery point active — "
        "suspending thread (role=%d screen=%d caller=%s+0x%lX, safety fallback)",
        currentRole, currentScreenIndex,
        callerModule, static_cast<unsigned long>(callerRva));
    while (true) { Sleep(INFINITE); }
}

// ---------------------------------------------------------------------------
// PatchRevivalDllExitProcess — walk the Revival DLL's PE import table and
// redirect its ExitProcess IAT entry to NeutralizeExitProcess.
// ---------------------------------------------------------------------------
bool PatchRevivalDllExitProcess()
{
    HMODULE module = g_localRevivalModule;
    if (module == nullptr)
    {
        return false;
    }

    auto* base = reinterpret_cast<uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        mod::Log("PatchRevivalDllExitProcess: bad DOS signature");
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        mod::Log("PatchRevivalDllExitProcess: bad NT signature");
        return false;
    }

    const DWORD importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    const DWORD importSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if (importDir == 0 || importSize == 0)
    {
        mod::Log("PatchRevivalDllExitProcess: no import directory");
        return false;
    }

    auto* importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir);
    for (; importDesc->Name != 0; ++importDesc)
    {
        const char* dllName = reinterpret_cast<const char*>(base + importDesc->Name);
        if (_stricmp(dllName, "kernel32.dll") != 0)
        {
            continue;
        }

        if (importDesc->OriginalFirstThunk == 0)
        {
            continue;
        }

        auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->OriginalFirstThunk);
        auto* iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA*>(base + importDesc->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal))
            {
                continue;
            }

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(importByName->Name), "ExitProcess") != 0)
            {
                continue;
            }

            // Save the original resolved address.
            g_realExitProcess = reinterpret_cast<ExitProcessFn>(iatThunk->u1.Function);

            // Overwrite the IAT entry with our interceptor.
            DWORD oldProtect = 0;
            if (!VirtualProtect(&iatThunk->u1.Function, sizeof(uintptr_t), PAGE_READWRITE, &oldProtect))
            {
                mod::Log("PatchRevivalDllExitProcess: VirtualProtect failed err=%lu", GetLastError());
                return false;
            }
            iatThunk->u1.Function = reinterpret_cast<ULONG_PTR>(NeutralizeExitProcess);
            VirtualProtect(&iatThunk->u1.Function, sizeof(uintptr_t), oldProtect, &oldProtect);

            mod::Log(
                "PatchRevivalDllExitProcess: patched IAT entry orig=0x%p stub=0x%p",
                reinterpret_cast<void*>(g_realExitProcess),
                reinterpret_cast<void*>(NeutralizeExitProcess));
            return true;
        }
    }

    mod::Log("PatchRevivalDllExitProcess: ExitProcess import not found in kernel32 descriptor");
    return false;
}

constexpr DWORD kRedirectProcessAccess =
    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_CREATE_THREAD | SYNCHRONIZE;

static bool ContainsInsensitiveAscii(const char* haystack, const char* needle)
{
    if (haystack == nullptr || needle == nullptr || needle[0] == '\0')
    {
        return false;
    }

    const size_t needleLen = std::strlen(needle);
    for (size_t i = 0; haystack[i] != '\0'; ++i)
    {
        size_t j = 0;
        while (j < needleLen && haystack[i + j] != '\0')
        {
            const unsigned char lhs = static_cast<unsigned char>(haystack[i + j]);
            const unsigned char rhs = static_cast<unsigned char>(needle[j]);
            if (std::tolower(lhs) != std::tolower(rhs))
            {
                break;
            }
            ++j;
        }
        if (j == needleLen)
        {
            return true;
        }
    }

    return false;
}

static bool ShouldRedirectCreateProcessA(LPCSTR lpApplicationName, LPCSTR lpCommandLine)
{
    return ContainsInsensitiveAscii(lpApplicationName, "efz.exe")
        || ContainsInsensitiveAscii(lpCommandLine, "efz.exe");
}

enum class RedirectHostSource
{
    InjectedBlock,
    TempIpc,
};

static bool TryOpenHostProcessByPid(uint32_t hostPid, HANDLE* outProcessHandle)
{
    if (hostPid == 0 || outProcessHandle == nullptr)
    {
        return false;
    }

    const HANDLE processHandle = OpenProcess(kRedirectProcessAccess, FALSE, hostPid);
    if (processHandle == nullptr)
    {
        return false;
    }

    *outProcessHandle = processHandle;
    return true;
}

static bool ResolveHostProcessForRedirect(HANDLE* outProcessHandle, uint32_t* outHostPid, RedirectHostSource* outSource)
{
    if (outProcessHandle == nullptr || outHostPid == nullptr)
    {
        return false;
    }

    *outProcessHandle = nullptr;
    *outHostPid = 0;

    if (g_injectedBlock != nullptr)
    {
        if (TryOpenHostProcessByPid(g_injectedBlock->hostPid, outProcessHandle))
        {
            *outHostPid = g_injectedBlock->hostPid;
            if (outSource != nullptr)
            {
                *outSource = RedirectHostSource::InjectedBlock;
            }
            return true;
        }
    }

    TempIpcContext temp = {};
    if (!OpenTempIpcContext(&temp, false, false) || temp.block == nullptr)
    {
        CloseTempIpcContext(&temp);
        return false;
    }

    InterlockedIncrement(&temp.block->dbgCreateProcessHits);
    const uint32_t tempHostPid = temp.block->hostPid;
    const bool opened = TryOpenHostProcessByPid(tempHostPid, outProcessHandle);
    CloseTempIpcContext(&temp);
    if (!opened)
    {
        return false;
    }

    if (g_injectedBlock != nullptr && g_injectedBlock->hostPid != tempHostPid)
    {
        g_injectedBlock->hostPid = tempHostPid;
    }

    *outHostPid = tempHostPid;
    if (outSource != nullptr)
    {
        *outSource = RedirectHostSource::TempIpc;
    }
    return true;
}

static uintptr_t ResolveInjectedInitAddress()
{
    if (g_injectedInitAddress != 0)
    {
        return g_injectedInitAddress;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return 0;
    }

    const FARPROC initProc = GetProcAddress(revival, "init");
    g_injectedInitAddress = reinterpret_cast<uintptr_t>(initProc);
    if (g_injectedInitAddress != 0)
    {
        mod::Log("Takeover: resolved injected init address=0x%p", reinterpret_cast<void*>(g_injectedInitAddress));
    }
    return g_injectedInitAddress;
}

static bool IsLikelyInitThreadCall(LPTHREAD_START_ROUTINE lpStartAddress, LONG callIndex)
{
    const uintptr_t start = reinterpret_cast<uintptr_t>(lpStartAddress);
    const uintptr_t initAddress = ResolveInjectedInitAddress();
    if (initAddress != 0 && start == initAddress)
    {
        return true;
    }

    if (initAddress != 0)
    {
        const uintptr_t localBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("EfzRevival.dll"));
        if (localBase != 0 && initAddress >= localBase)
        {
            const uintptr_t initRva = initAddress - localBase;
            uintptr_t remoteBase = ResolveInjectedExpectedRevivalBase();
            if (remoteBase == 0)
            {
                remoteBase = static_cast<uintptr_t>(g_activeRevival->defaultImageBase);
            }
            const uintptr_t expectedRemoteInit = remoteBase + initRva;
            if (start == expectedRemoteInit)
            {
                return true;
            }
        }
    }

    // Fallback for builds where init cannot be resolved yet.
    return callIndex >= 2;
}

static bool TryReadRemoteInitParams(LPVOID lpParameter, int* outMode, int* outMagic)
{
    if (lpParameter == nullptr || outMode == nullptr || outMagic == nullptr)
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(lpParameter, &mbi, sizeof(mbi)) == 0)
    {
        return false;
    }
    if (mbi.State != MEM_COMMIT || mbi.Protect == PAGE_NOACCESS)
    {
        return false;
    }

    const DWORD protect = mbi.Protect & 0xFF;
    const bool readable =
        protect == PAGE_READONLY ||
        protect == PAGE_READWRITE ||
        protect == PAGE_WRITECOPY ||
        protect == PAGE_EXECUTE_READ ||
        protect == PAGE_EXECUTE_READWRITE ||
        protect == PAGE_EXECUTE_WRITECOPY;
    if (!readable)
    {
        return false;
    }

    int params[2] = {0, 0};
    std::memcpy(params, lpParameter, sizeof(params));
    *outMode = params[0];
    *outMagic = params[1];
    return true;
}

// ---------------------------------------------------------------------------
// Stub implementations called from nb_stub_* exports.
// ---------------------------------------------------------------------------

BOOL StubCreateProcessA(
    LPCSTR lpApplicationName,
    LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation)
{
    if (g_injectedBlock != nullptr)
    {
        InterlockedIncrement(&g_injectedBlock->dbgCreateProcessHits);
    }

    if (!ShouldRedirectCreateProcessA(lpApplicationName, lpCommandLine))
    {
        return CreateProcessA(
            lpApplicationName,
            lpCommandLine,
            lpProcessAttributes,
            lpThreadAttributes,
            bInheritHandles,
            dwCreationFlags,
            lpEnvironment,
            lpCurrentDirectory,
            lpStartupInfo,
            lpProcessInformation);
    }

    if (lpProcessInformation == nullptr)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        mod::Log("nb_stub_CreateProcessA: redirect rejected (null PROCESS_INFORMATION)");
        return FALSE;
    }

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }

    HANDLE processHandle = nullptr;
    uint32_t hostPid = 0;
    RedirectHostSource source = RedirectHostSource::InjectedBlock;
    if (!ResolveHostProcessForRedirect(&processHandle, &hostPid, &source))
    {
        const uint32_t injectedHostPid = (g_injectedBlock != nullptr) ? g_injectedBlock->hostPid : 0u;
        mod::Log(
            "nb_stub_CreateProcessA: OpenProcess failed injectedHostPid=%lu err=%s",
            static_cast<unsigned long>(injectedHostPid),
            ErrorString(GetLastError()).c_str());
        return FALSE;
    }

    HANDLE threadHandle = CreateEventA(nullptr, TRUE, TRUE, nullptr);
    if (threadHandle == nullptr)
    {
        mod::Log("nb_stub_CreateProcessA: failed to create fake process thread handle: %s", ErrorString(GetLastError()).c_str());
        CloseHandle(processHandle);
        return FALSE;
    }

    g_fakeProcessThreadHandle = threadHandle;

    std::memset(lpProcessInformation, 0, sizeof(PROCESS_INFORMATION));
    lpProcessInformation->hProcess = processHandle;
    lpProcessInformation->hThread = threadHandle;
    lpProcessInformation->dwProcessId = hostPid;
    lpProcessInformation->dwThreadId = GetCurrentThreadId();
    mod::Log(
        "nb_stub_CreateProcessA: redirected to host pid=%lu source=%s",
        static_cast<unsigned long>(lpProcessInformation->dwProcessId),
        (source == RedirectHostSource::TempIpc) ? "temp_ipc" : "injected");
    return TRUE;
}

BOOL StubReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead)
{
    const uintptr_t address = reinterpret_cast<uintptr_t>(lpBaseAddress);
    if (address == 0x787237)
    {
        if (lpBuffer != nullptr && nSize > 0)
        {
            std::memset(lpBuffer, 0, nSize);
        }
        if (lpNumberOfBytesRead != nullptr)
        {
            *lpNumberOfBytesRead = nSize;
        }
        return TRUE;
    }

    if (address == 0x7871F4)
    {
        std::array<uint8_t, 4> bytes = {
            static_cast<uint8_t>(g_activeRevival->efzFingerprint & 0xFF),
            static_cast<uint8_t>((g_activeRevival->efzFingerprint >> 8) & 0xFF),
            static_cast<uint8_t>((g_activeRevival->efzFingerprint >> 16) & 0xFF),
            static_cast<uint8_t>((g_activeRevival->efzFingerprint >> 24) & 0xFF),
        };
        const SIZE_T copy = (std::min)(nSize, static_cast<SIZE_T>(bytes.size()));
        if (lpBuffer != nullptr && copy > 0)
        {
            std::memcpy(lpBuffer, bytes.data(), copy);
        }
        if (lpNumberOfBytesRead != nullptr)
        {
            *lpNumberOfBytesRead = copy;
        }
        const LONG hits = InterlockedIncrement(&g_injectedFingerprintReadHits);
        if (hits <= 8 || (hits % 64) == 0)
        {
            mod::Log(
                "nb_stub_ReadProcessMemory: spoof addr=0x%08lX size=%zu value=0x%08lX count=%ld",
                static_cast<unsigned long>(address),
                static_cast<size_t>(nSize),
                static_cast<unsigned long>(g_activeRevival->efzFingerprint),
                static_cast<long>(hits));
        }
        return TRUE;
    }

    return ReadProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead);
}

LPVOID StubVirtualAllocEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (!HasInjectedContext())
    {
        LPVOID local = VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
        if (local == nullptr)
        {
            mod::Log("nb_stub_VirtualAllocEx: fallback VirtualAlloc failed size=%zu err=%s", static_cast<size_t>(dwSize), ErrorString(GetLastError()).c_str());
        }
        else
        {
            RegisterRedirectAllocation(local, dwSize);
            mod::Log("nb_stub_VirtualAllocEx: fallback local alloc size=%zu addr=0x%p", static_cast<size_t>(dwSize), local);
        }
        return local;
    }

    const LPVOID local = VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect);
    if (local == nullptr)
    {
        mod::Log("nb_stub_VirtualAllocEx: VirtualAlloc failed size=%zu err=%s", static_cast<size_t>(dwSize), ErrorString(GetLastError()).c_str());
    }
    else
    {
        RegisterRedirectAllocation(local, dwSize);
    }
    return local;
}

BOOL StubVirtualFreeEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (!HasInjectedContext())
    {
        if (lpAddress == nullptr)
        {
            return TRUE;
        }
        DWORD freeType = dwFreeType == 0 ? MEM_RELEASE : dwFreeType;
        SIZE_T freeSize = (freeType == MEM_RELEASE) ? 0 : dwSize;
        BOOL ok = VirtualFree(lpAddress, freeSize, freeType);
        if (ok == FALSE && freeType == MEM_RELEASE)
        {
            ok = VirtualFree(lpAddress, 0, MEM_RELEASE);
        }
        if (ok != FALSE && freeType == MEM_RELEASE)
        {
            ForgetRedirectAllocation(lpAddress);
        }
        return ok;
    }

    if (lpAddress == nullptr)
    {
        return TRUE;
    }

    DWORD freeType = dwFreeType;
    SIZE_T freeSize = dwSize;
    if (freeType == 0)
    {
        freeType = MEM_RELEASE;
        freeSize = 0;
    }

    BOOL ok = VirtualFree(lpAddress, freeSize, freeType);
    if (ok == FALSE && freeType == MEM_RELEASE && freeSize != 0)
    {
        ok = VirtualFree(lpAddress, 0, MEM_RELEASE);
    }
    if (ok != FALSE && freeType == MEM_RELEASE)
    {
        ForgetRedirectAllocation(lpAddress);
    }
    return ok;
}

BOOL StubWriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesWritten)
{
    if (g_injectedBlock != nullptr)
    {
        InterlockedIncrement(&g_injectedBlock->dbgWriteProcessHits);
    }

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (!HasInjectedContext())
    {
        bool copied = true;
        if (lpBaseAddress != nullptr && lpBuffer != nullptr && nSize > 0)
        {
            if (IsWithinRedirectAllocation(lpBaseAddress, nSize) && IsWritableRange(lpBaseAddress, nSize))
            {
                std::memcpy(lpBaseAddress, lpBuffer, nSize);
            }
            else
            {
                copied = false;
                SetLastError(ERROR_NOACCESS);
                const LONG blockedHits = InterlockedIncrement(&g_redirectWriteBlockedHits);
                if (blockedHits <= 8 || (blockedHits % 64) == 0)
                {
                    mod::Log(
                        "nb_stub_WriteProcessMemory: fallback write blocked addr=0x%p size=%zu tracked=%d count=%ld",
                        lpBaseAddress,
                        static_cast<size_t>(nSize),
                        IsWithinRedirectAllocation(lpBaseAddress, nSize) ? 1 : 0,
                        static_cast<long>(blockedHits));
                }
            }
        }
        if (lpNumberOfBytesWritten != nullptr)
        {
            *lpNumberOfBytesWritten = copied ? nSize : 0;
        }

        if (nSize == sizeof(int) * 2 && lpBuffer != nullptr)
        {
            const int* vals = reinterpret_cast<const int*>(lpBuffer);
            TempIpcContext temp = {};
            if (OpenTempIpcContext(&temp, true, false) && temp.block != nullptr)
            {
                InterlockedIncrement(&temp.block->dbgWriteProcessHits);
                if (vals[1] == 102 && vals[0] >= 0 && vals[0] <= 3)
                {
                    temp.block->initParams[0] = vals[0];
                    temp.block->initParams[1] = vals[1];
                    InterlockedIncrement(&temp.block->initSerial);
                    if (temp.initEvent != nullptr)
                    {
                        SetEvent(temp.initEvent);
                    }
                    mod::Log(
                        "nb_stub_WriteProcessMemory: fallback captured init params mode=%d magic=%d",
                        vals[0],
                        vals[1]);
                }
            }
            CloseTempIpcContext(&temp);
        }
        else
        {
            TempIpcContext temp = {};
            if (OpenTempIpcContext(&temp, false, false) && temp.block != nullptr)
            {
                InterlockedIncrement(&temp.block->dbgWriteProcessHits);
            }
            CloseTempIpcContext(&temp);
        }
        return copied ? TRUE : FALSE;
    }

    bool copied = true;
    if (lpBaseAddress != nullptr && lpBuffer != nullptr && nSize > 0)
    {
        if (IsWithinRedirectAllocation(lpBaseAddress, nSize) && IsWritableRange(lpBaseAddress, nSize))
        {
            std::memcpy(lpBaseAddress, lpBuffer, nSize);
        }
        else
        {
            copied = false;
            SetLastError(ERROR_NOACCESS);
            const LONG blockedHits = InterlockedIncrement(&g_redirectWriteBlockedHits);
            if (blockedHits <= 8 || (blockedHits % 64) == 0)
            {
                mod::Log(
                    "nb_stub_WriteProcessMemory: write blocked addr=0x%p size=%zu tracked=%d count=%ld",
                    lpBaseAddress,
                    static_cast<size_t>(nSize),
                    IsWithinRedirectAllocation(lpBaseAddress, nSize) ? 1 : 0,
                    static_cast<long>(blockedHits));
            }
        }
    }

    if (lpNumberOfBytesWritten != nullptr)
    {
        *lpNumberOfBytesWritten = copied ? nSize : 0;
    }

    if (nSize == sizeof(int) * 2)
    {
        const int* vals = reinterpret_cast<const int*>(lpBuffer);
        mod::Log("nb_stub_WriteProcessMemory: observed init write mode=%d magic=%d dst=0x%p", vals[0], vals[1], lpBaseAddress);

        if (!g_initCapturedFromWrite
            && vals[1] == 102
            && vals[0] >= 0
            && vals[0] <= 3
            && g_injectedBlock != nullptr
            && g_injectedInitEvent != nullptr)
        {
            g_injectedBlock->initParams[0] = vals[0];
            g_injectedBlock->initParams[1] = vals[1];
            InterlockedIncrement(&g_injectedBlock->initSerial);
            SetEvent(g_injectedInitEvent);
            g_initCapturedFromWrite = true;
            mod::Log(
                "nb_stub_WriteProcessMemory: captured init params via write mode=%d magic=%d",
                vals[0],
                vals[1]);
        }
    }

    return copied ? TRUE : FALSE;
}

HANDLE StubCreateRemoteThread(HANDLE hProcess, LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId)
{
    (void)hProcess;
    (void)lpThreadAttributes;
    (void)dwStackSize;
    (void)dwCreationFlags;

    if (lpThreadId != nullptr)
    {
        *lpThreadId = GetCurrentThreadId();
    }

    if (g_injectedBlock != nullptr)
    {
        InterlockedIncrement(&g_injectedBlock->dbgCreateRemoteThreadHits);
    }

    const uintptr_t expectedRemoteBase = [&]() -> uintptr_t {
        uintptr_t base = ResolveInjectedExpectedRevivalBase();
        if (base == 0)
        {
            base = ResolveHostRevivalBase();
        }
        if (base == 0)
        {
            base = static_cast<uintptr_t>(g_activeRevival->defaultImageBase);
        }
        return base;
    }();

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (!HasInjectedContext())
    {
        TempIpcContext temp = {};
        const LONG callIndexFallback = InterlockedIncrement(&g_remoteThreadCallIndex);
        int paramMode = 0;
        int paramMagic = 0;
        const bool hasParamHint = TryReadRemoteInitParams(lpParameter, &paramMode, &paramMagic);
        const bool paramLooksLikeInit = hasParamHint && paramMagic == 102 && paramMode >= 0 && paramMode <= 3;

        if (OpenTempIpcContext(&temp, true, false) && temp.block != nullptr)
        {
            InterlockedIncrement(&temp.block->dbgCreateRemoteThreadHits);
            if (paramLooksLikeInit && temp.initEvent != nullptr)
            {
                temp.block->initParams[0] = paramMode;
                temp.block->initParams[1] = paramMagic;
                InterlockedIncrement(&temp.block->initSerial);
                SetEvent(temp.initEvent);
                mod::Log(
                    "nb_stub_CreateRemoteThread: fallback captured init params mode=%d magic=%d start=0x%p call=%ld",
                    paramMode,
                    paramMagic,
                    reinterpret_cast<void*>(lpStartAddress),
                    static_cast<long>(callIndexFallback));
                CloseTempIpcContext(&temp);
                return CreateFakeThread(1);
            }
        }
        CloseTempIpcContext(&temp);

        mod::Log("nb_stub_CreateRemoteThread: passthrough (ready=%d start=0x%p)", HasInjectedContext() ? 1 : 0, reinterpret_cast<void*>(lpStartAddress));
        return CreateRemoteThread(hProcess, lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
    }

    const LONG callIndex = InterlockedIncrement(&g_remoteThreadCallIndex);
    int paramMode = 0;
    int paramMagic = 0;
    const bool hasParamHint = TryReadRemoteInitParams(lpParameter, &paramMode, &paramMagic);
    const bool paramLooksLikeInit = hasParamHint && paramMagic == 102 && paramMode >= 0 && paramMode <= 3;
    const bool initCall = paramLooksLikeInit || IsLikelyInitThreadCall(lpStartAddress, callIndex);
    DWORD fakeExitCode = static_cast<DWORD>(expectedRemoteBase);

    if (initCall)
    {
        int params[2] = {0, 102};
        if (hasParamHint)
        {
            params[0] = paramMode;
            params[1] = paramMagic;
        }
        else if (lpParameter != nullptr)
        {
            std::memcpy(params, lpParameter, sizeof(params));
        }
        g_injectedBlock->initParams[0] = params[0];
        g_injectedBlock->initParams[1] = params[1];
        InterlockedIncrement(&g_injectedBlock->initSerial);
        SetEvent(g_injectedInitEvent);
        fakeExitCode = 1;
        mod::Log(
            "nb_stub_CreateRemoteThread: captured init params mode=%d magic=%d start=0x%p call=%ld",
            params[0],
            params[1],
            reinterpret_cast<void*>(lpStartAddress),
            static_cast<long>(callIndex));
    }
    else
    {
        mod::Log(
            "nb_stub_CreateRemoteThread: bypass call start=0x%p call=%ld base=0x%08lX",
            reinterpret_cast<void*>(lpStartAddress),
            static_cast<long>(callIndex),
            static_cast<unsigned long>(fakeExitCode));
    }

    return CreateFakeThread(fakeExitCode);
}

BOOL StubTerminateProcess(HANDLE hProcess, UINT uExitCode)
{
    if (hProcess == nullptr)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }

    uint32_t hostPid = 0;
    if (g_injectedBlock != nullptr)
    {
        hostPid = g_injectedBlock->hostPid;
    }

    if (hostPid != 0)
    {
        const DWORD targetPid = GetProcessId(hProcess);
        if (targetPid == 0)
        {
            const LONG hits = InterlockedIncrement(&g_injectedTerminateUnknownPidHits);
            if (hits <= 8 || (hits % 64) == 0)
            {
                const DWORD pidError = GetLastError();
                mod::Log(
                    "nb_stub_TerminateProcess: unresolved target pid hostPid=%lu handle=0x%p err=%s (blocked) count=%ld",
                    static_cast<unsigned long>(hostPid),
                    hProcess,
                    ErrorString(pidError).c_str(),
                    static_cast<long>(hits));
            }
            return TRUE;
        }
        if (targetPid == hostPid)
        {
            mod::Log(
                "nb_stub_TerminateProcess: blocked host termination pid=%lu exit=%u",
                static_cast<unsigned long>(targetPid),
                static_cast<unsigned>(uExitCode));
            return TRUE;
        }
    }

    return TerminateProcess(hProcess, uExitCode);
}

HANDLE StubOpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId)
{
    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }

    uint32_t hostPid = 0;
    if (g_injectedBlock != nullptr)
    {
        hostPid = g_injectedBlock->hostPid;
    }

    DWORD requestedAccess = dwDesiredAccess;
    if (hostPid != 0 && dwProcessId == hostPid && (requestedAccess & PROCESS_TERMINATE) != 0)
    {
        requestedAccess &= ~PROCESS_TERMINATE;
        mod::Log(
            "nb_stub_OpenProcess: stripped PROCESS_TERMINATE for host pid=%lu requested=0x%08lX effective=0x%08lX",
            static_cast<unsigned long>(dwProcessId),
            static_cast<unsigned long>(dwDesiredAccess),
            static_cast<unsigned long>(requestedAccess));
    }

    return OpenProcess(requestedAccess, bInheritHandle, dwProcessId);
}

BOOL StubReadConsoleA(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl)
{
    (void)hConsoleInput;
    (void)pInputControl;
    FlushPendingConsoleOutput("before_ReadConsoleA");

    auto copyInputOut = [&](const char* input) -> BOOL {
        if (input == nullptr || input[0] == '\0')
        {
            input = "2\n";
        }
        const size_t len = std::strlen(input);
        const DWORD copy = static_cast<DWORD>((std::min)(static_cast<size_t>(nNumberOfCharsToRead), len));
        if (lpBuffer != nullptr && copy > 0)
        {
            std::memcpy(lpBuffer, input, copy);
            if (copy < nNumberOfCharsToRead)
            {
                reinterpret_cast<char*>(lpBuffer)[copy] = '\0';
            }
        }
        if (lpNumberOfCharsRead != nullptr)
        {
            *lpNumberOfCharsRead = copy;
        }
        return TRUE;
    };

    auto serveScriptedInput = [&](SharedBlock* block, const char* input, const char* sourceTag, const char* reason, LONG serial, bool autoInput) -> BOOL {
        if (block != nullptr)
        {
            InterlockedIncrement(&block->dbgReadConsoleHits);
            if (autoInput)
            {
                InterlockedIncrement(&block->dbgReadConsoleAutoHits);
            }
        }

        const BOOL ok = copyInputOut(input);
        if (autoInput)
        {
            const LONG fallbackCount = InterlockedIncrement(&g_injectedAutoConsoleFallbackCount);
            if (fallbackCount <= 8 || (fallbackCount % 64) == 0)
            {
                mod::Log(
                    "nb_stub_ReadConsoleA: auto input serial=%ld source=%s reason=%s read=%lu value='%s' count=%ld",
                    static_cast<long>(serial),
                    sourceTag != nullptr ? sourceTag : "unknown",
                    reason != nullptr ? reason : "",
                    static_cast<unsigned long>(nNumberOfCharsToRead),
                    (input != nullptr) ? input : "",
                    static_cast<long>(fallbackCount));
            }
        }
        else
        {
            mod::Log(
                "nb_stub_ReadConsoleA: served input serial=%ld source=%s value='%s'",
                static_cast<long>(serial),
                sourceTag != nullptr ? sourceTag : "unknown",
                (input != nullptr) ? input : "");
        }
        return ok;
    };

    auto waitAndServe = [&](SharedBlock* block, HANDLE consoleEvent, const char* sourceTag) -> BOOL {
        if (block == nullptr)
        {
            return FALSE;
        }

        for (;;)
        {
            const LONG serial = block->consoleSerial;
            const LONG servedSerial = InterlockedCompareExchange(&g_injectedLastConsoleSerialServed, 0, 0);
            if (serial > 0 && serial != servedSerial)
            {
                InterlockedExchange(&g_injectedLastConsoleSerialServed, serial);
                return serveScriptedInput(block, block->consoleInput, sourceTag, "primary", serial, false);
            }

            const LONG auxSerial = block->consoleAuxSerial;
            if (auxSerial > 0 && serial > 0 && serial == servedSerial && block->consoleInputAux[0] != '\0')
            {
                const int roleMode = block->initParams[0];
                const LONG promptSerial = InterlockedCompareExchange(&g_injectedDelayPromptSerial, 0, 0);
                const LONG promptServed = InterlockedCompareExchange(&g_injectedDelayPromptServedSerial, 0, 0);
                const bool delayPromptPending = promptSerial > 0 && promptSerial != promptServed;
                const LONG servedAuxSerial = InterlockedCompareExchange(&g_injectedLastConsoleAuxSerialServed, 0, 0);
                const bool auxNotServedYet = servedAuxSerial != auxSerial;
                bool allowAuxNow = (roleMode == kLocalRoleLocalPlay || roleMode == kLocalRoleTournament) || delayPromptPending;
                if (!allowAuxNow && auxNotServedYet && (roleMode == kLocalRoleOnline || roleMode == kLocalRoleSpectate))
                {
                    allowAuxNow = true;
                }
                if (!allowAuxNow)
                {
                    goto maybe_default_input;
                }

                if (auxSerial != g_injectedActiveConsoleAuxSerial)
                {
                    InterlockedExchange(&g_injectedActiveConsoleAuxSerial, auxSerial);
                    InterlockedExchange(&g_injectedConsoleAuxScriptOffset, 0);
                    InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, 0);
                }

                if (servedAuxSerial != auxSerial)
                {
                    char auxLine[128] = {};
                    LONG auxOffset = InterlockedCompareExchange(&g_injectedConsoleAuxScriptOffset, 0, 0);
                    bool auxHasMore = false;
                    if (ExtractConsoleScriptLine(
                            block->consoleInputAux,
                            &auxOffset,
                            auxLine,
                            sizeof(auxLine),
                            &auxHasMore))
                    {
                        InterlockedExchange(&g_injectedConsoleAuxScriptOffset, auxOffset);
                        if (!auxHasMore)
                        {
                            InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, auxSerial);
                        }

                        // When aux auto-answers the spectate confirm prompt
                        // (JoinSpectate flow), sync the served serial so that
                        // the title-screen overlay knows the prompt is resolved
                        // and does not loop the confirmation window.
                        if (roleMode == kLocalRoleSpectate)
                        {
                            const LONG scPS = InterlockedCompareExchange(&g_injectedSpectateConfirmPromptSerial, 0, 0);
                            if (scPS > 0)
                            {
                                InterlockedExchange(&g_injectedSpectateConfirmPromptServedSerial, scPS);
                                InterlockedExchange(&block->spectateConfirmPromptServedSerial, scPS);
                                mod::Log("Takeover: aux auto-answered spectate confirm — synced servedSerial=%ld", static_cast<long>(scPS));
                            }
                        }

                        return serveScriptedInput(block, auxLine, sourceTag, "aux_line", auxSerial, true);
                    }

                    InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, auxSerial);
                }
            }

        maybe_default_input:
            if (serial > 0 && serial == servedSerial && nNumberOfCharsToRead <= 0x80u)
            {
                // --- Spectate confirm prompt handling ---
                const LONG scPromptSerial = InterlockedCompareExchange(&g_injectedSpectateConfirmPromptSerial, 0, 0);
                const LONG scPromptServed = InterlockedCompareExchange(&g_injectedSpectateConfirmPromptServedSerial, 0, 0);
                if (scPromptSerial > 0 && scPromptSerial != scPromptServed)
                {
                    const LONG scInputSerial = InterlockedCompareExchange(&block->spectateConfirmInputSerial, 0, 0);
                    const LONG scInputServedSerial = InterlockedCompareExchange(&block->spectateConfirmInputServedSerial, 0, 0);
                    if (scInputSerial > scInputServedSerial)
                    {
                        const int scValue = block->spectateConfirmInputValue;
                        char scLine[16] = {};
                        std::snprintf(scLine, sizeof(scLine), "%d\r\n", scValue);
                        InterlockedExchange(&block->spectateConfirmInputServedSerial, scInputSerial);
                        InterlockedExchange(&g_injectedSpectateConfirmPromptServedSerial, scPromptSerial);
                        InterlockedExchange(&block->spectateConfirmPromptServedSerial, scPromptSerial);
                        g_injectedSpectateConfirmPromptWaitStartTick = 0;
                        return serveScriptedInput(block, scLine, sourceTag, "prompt_spectate_confirm_selected", scInputSerial, true);
                    }

                    const DWORD nowTickSc = GetTickCount();
                    if (g_injectedSpectateConfirmPromptWaitStartTick == 0)
                    {
                        g_injectedSpectateConfirmPromptWaitStartTick = nowTickSc;
                        mod::Log(
                            "Takeover: spectate confirm prompt waiting for overlay selection promptSerial=%ld",
                            static_cast<long>(scPromptSerial));
                    }
                    if (nowTickSc - g_injectedSpectateConfirmPromptWaitStartTick < kPromptSpectateConfirmWaitTimeoutMs)
                    {
                        if (consoleEvent != nullptr)
                        {
                            const DWORD wait = WaitForSingleObject(consoleEvent, 200);
                            if (wait == WAIT_FAILED)
                            {
                                break;
                            }
                        }
                        else
                        {
                            Sleep(10);
                        }
                        continue;
                    }

                    int defaultChoice = 2;
                    const int promptKind = block->spectateConfirmPromptKind;
                    if (promptKind == static_cast<int>(NetbridgeSpectatePromptKind::HostNotYetPlaying))
                    {
                        defaultChoice = 3;
                    }
                    char defaultLine[16] = {};
                    std::snprintf(defaultLine, sizeof(defaultLine), "%d\r\n", defaultChoice);
                    InterlockedExchange(&g_injectedSpectateConfirmPromptServedSerial, scPromptSerial);
                    InterlockedExchange(&block->spectateConfirmPromptServedSerial, scPromptSerial);
                    g_injectedSpectateConfirmPromptWaitStartTick = 0;
                    mod::Log(
                        "Takeover: spectate confirm prompt timed out; falling back to choice=%d kind=%d promptSerial=%ld",
                        defaultChoice,
                        promptKind,
                        static_cast<long>(scPromptSerial));
                    return serveScriptedInput(block, defaultLine, sourceTag, "prompt_spectate_confirm_default", scPromptSerial, true);
                }

                // --- Delay prompt handling ---
                const LONG promptSerial = InterlockedCompareExchange(&g_injectedDelayPromptSerial, 0, 0);
                const LONG promptServed = InterlockedCompareExchange(&g_injectedDelayPromptServedSerial, 0, 0);
                if (promptSerial > 0 && promptSerial != promptServed)
                {
                    const LONG delayInputSerial = InterlockedCompareExchange(&block->delayInputSerial, 0, 0);
                    const LONG delayInputServedSerial = InterlockedCompareExchange(&block->delayInputServedSerial, 0, 0);
                    if (delayInputSerial > delayInputServedSerial)
                    {
                        const int delayValue = block->delayInputValue;
                        char delayLine[16] = {};
                        std::snprintf(delayLine, sizeof(delayLine), "%d\r\n", delayValue);
                        InterlockedExchange(&block->delayInputServedSerial, delayInputSerial);
                        InterlockedExchange(&g_injectedDelayPromptServedSerial, promptSerial);
                        InterlockedExchange(&block->delayPromptServedSerial, promptSerial);
                        g_injectedDelayPromptWaitStartTick = 0;
                        return serveScriptedInput(block, delayLine, sourceTag, "prompt_delay_selected", delayInputSerial, true);
                    }

                    const DWORD nowTick = GetTickCount();
                    if (g_injectedDelayPromptWaitStartTick == 0)
                    {
                        g_injectedDelayPromptWaitStartTick = nowTick;
                        mod::Log(
                            "Takeover: delay prompt waiting for overlay selection promptSerial=%ld",
                            static_cast<long>(promptSerial));
                    }
                    if (nowTick - g_injectedDelayPromptWaitStartTick < kPromptDelayInputWaitTimeoutMs)
                    {
                        if (consoleEvent != nullptr)
                        {
                            const DWORD wait = WaitForSingleObject(consoleEvent, 200);
                            if (wait == WAIT_FAILED)
                            {
                                break;
                            }
                        }
                        else
                        {
                            Sleep(10);
                        }
                        continue;
                    }

                    InterlockedExchange(&g_injectedDelayPromptServedSerial, promptSerial);
                    InterlockedExchange(&block->delayPromptServedSerial, promptSerial);
                    g_injectedDelayPromptWaitStartTick = 0;
                    mod::Log(
                        "Takeover: delay prompt timed out; falling back to default promptSerial=%ld",
                        static_cast<long>(promptSerial));
                    return serveScriptedInput(block, "\r\n", sourceTag, "prompt_delay_default", promptSerial, true);
                }
                return FALSE;
            }

            if (consoleEvent != nullptr)
            {
                const DWORD wait = WaitForSingleObject(consoleEvent, 200);
                if (wait == WAIT_FAILED)
                {
                    break;
                }
                continue;
            }

            Sleep(10);
        }

        return FALSE;
    };

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (HasInjectedContext() && waitAndServe(g_injectedBlock, g_injectedConsoleEvent, "injected"))
    {
        return TRUE;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, true))
    {
        const BOOL ok = waitAndServe(temp.block, temp.consoleEvent, "fallback");
        CloseTempIpcContext(&temp);
        if (ok)
        {
            return TRUE;
        }
    }

    SetLastError(NO_ERROR);
    const BOOL nativeResult = ReadConsoleA(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, pInputControl);
    const DWORD nativeError = GetLastError();
    const DWORD nativeRead = (lpNumberOfCharsRead != nullptr) ? *lpNumberOfCharsRead : 0;
    mod::Log(
        "nb_stub_ReadConsoleA: passthrough (ready=%d result=%d read=%lu err=%lu)",
        HasInjectedContext() ? 1 : 0,
        nativeResult ? 1 : 0,
        static_cast<unsigned long>(nativeRead),
        static_cast<unsigned long>(nativeError));
    return nativeResult;
}

BOOL StubReadConsoleW(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl)
{
    (void)hConsoleInput;
    (void)pInputControl;
    FlushPendingConsoleOutput("before_ReadConsoleW");

    auto copyInputOut = [&](const char* input) -> BOOL {
        if (input == nullptr || input[0] == '\0')
        {
            input = "2\n";
        }

        wchar_t wide[256] = {};
        const int converted =
            MultiByteToWideChar(CP_ACP, 0, input, -1, wide, static_cast<int>(sizeof(wide) / sizeof(wide[0])));
        if (converted <= 0)
        {
            if (lpNumberOfCharsRead != nullptr)
            {
                *lpNumberOfCharsRead = 0;
            }
            return FALSE;
        }

        const DWORD wideLen = static_cast<DWORD>((converted > 0) ? (converted - 1) : 0);
        const DWORD copy = (std::min)(nNumberOfCharsToRead, wideLen);
        if (lpBuffer != nullptr && copy > 0)
        {
            std::memcpy(lpBuffer, wide, static_cast<size_t>(copy) * sizeof(wchar_t));
            if (copy < nNumberOfCharsToRead)
            {
                reinterpret_cast<wchar_t*>(lpBuffer)[copy] = L'\0';
            }
        }
        if (lpNumberOfCharsRead != nullptr)
        {
            *lpNumberOfCharsRead = copy;
        }
        return TRUE;
    };

    auto serveScriptedInput = [&](SharedBlock* block, const char* input, const char* sourceTag, const char* reason, LONG serial, bool autoInput) -> BOOL {
        if (block != nullptr)
        {
            InterlockedIncrement(&block->dbgReadConsoleHits);
            if (autoInput)
            {
                InterlockedIncrement(&block->dbgReadConsoleAutoHits);
            }
        }

        const BOOL ok = copyInputOut(input);
        if (autoInput)
        {
            const LONG fallbackCount = InterlockedIncrement(&g_injectedAutoConsoleFallbackCount);
            if (fallbackCount <= 8 || (fallbackCount % 64) == 0)
            {
                mod::Log(
                    "nb_stub_ReadConsoleW: auto input serial=%ld source=%s reason=%s read=%lu value='%s' count=%ld",
                    static_cast<long>(serial),
                    sourceTag != nullptr ? sourceTag : "unknown",
                    reason != nullptr ? reason : "",
                    static_cast<unsigned long>(nNumberOfCharsToRead),
                    (input != nullptr) ? input : "",
                    static_cast<long>(fallbackCount));
            }
        }
        else
        {
            mod::Log(
                "nb_stub_ReadConsoleW: served input serial=%ld source=%s value='%s'",
                static_cast<long>(serial),
                sourceTag != nullptr ? sourceTag : "unknown",
                (input != nullptr) ? input : "");
        }
        return ok;
    };

    auto waitAndServe = [&](SharedBlock* block, HANDLE consoleEvent, const char* sourceTag) -> BOOL {
        if (block == nullptr)
        {
            return FALSE;
        }

        for (;;)
        {
            const LONG serial = block->consoleSerial;
            const LONG servedSerial = InterlockedCompareExchange(&g_injectedLastConsoleSerialServed, 0, 0);
            if (serial > 0 && serial != servedSerial)
            {
                InterlockedExchange(&g_injectedLastConsoleSerialServed, serial);
                return serveScriptedInput(block, block->consoleInput, sourceTag, "primary", serial, false);
            }

            const LONG auxSerial = block->consoleAuxSerial;
            if (auxSerial > 0 && serial > 0 && serial == servedSerial && block->consoleInputAux[0] != '\0')
            {
                const int roleMode = block->initParams[0];
                const LONG promptSerial = InterlockedCompareExchange(&g_injectedDelayPromptSerial, 0, 0);
                const LONG promptServed = InterlockedCompareExchange(&g_injectedDelayPromptServedSerial, 0, 0);
                const bool delayPromptPending = promptSerial > 0 && promptSerial != promptServed;
                const LONG servedAuxSerial = InterlockedCompareExchange(&g_injectedLastConsoleAuxSerialServed, 0, 0);
                const bool auxNotServedYet = servedAuxSerial != auxSerial;
                bool allowAuxNow = (roleMode == kLocalRoleLocalPlay || roleMode == kLocalRoleTournament) || delayPromptPending;
                if (!allowAuxNow && auxNotServedYet && (roleMode == kLocalRoleOnline || roleMode == kLocalRoleSpectate))
                {
                    allowAuxNow = true;
                }
                if (!allowAuxNow)
                {
                    goto maybe_default_input_w;
                }

                if (auxSerial != g_injectedActiveConsoleAuxSerial)
                {
                    InterlockedExchange(&g_injectedActiveConsoleAuxSerial, auxSerial);
                    InterlockedExchange(&g_injectedConsoleAuxScriptOffset, 0);
                    InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, 0);
                }

                if (servedAuxSerial != auxSerial)
                {
                    char auxLine[128] = {};
                    LONG auxOffset = InterlockedCompareExchange(&g_injectedConsoleAuxScriptOffset, 0, 0);
                    bool auxHasMore = false;
                    if (ExtractConsoleScriptLine(
                            block->consoleInputAux,
                            &auxOffset,
                            auxLine,
                            sizeof(auxLine),
                            &auxHasMore))
                    {
                        InterlockedExchange(&g_injectedConsoleAuxScriptOffset, auxOffset);
                        if (!auxHasMore)
                        {
                            InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, auxSerial);
                        }
                        return serveScriptedInput(block, auxLine, sourceTag, "aux_line", auxSerial, true);
                    }

                    InterlockedExchange(&g_injectedLastConsoleAuxSerialServed, auxSerial);
                }
            }

        maybe_default_input_w:
            if (serial > 0 && serial == servedSerial && nNumberOfCharsToRead <= 0x80u)
            {
                // --- Spectate confirm prompt handling (W) ---
                const LONG scPromptSerialW = InterlockedCompareExchange(&g_injectedSpectateConfirmPromptSerial, 0, 0);
                const LONG scPromptServedW = InterlockedCompareExchange(&g_injectedSpectateConfirmPromptServedSerial, 0, 0);
                if (scPromptSerialW > 0 && scPromptSerialW != scPromptServedW)
                {
                    const LONG scInputSerialW = InterlockedCompareExchange(&block->spectateConfirmInputSerial, 0, 0);
                    const LONG scInputServedSerialW = InterlockedCompareExchange(&block->spectateConfirmInputServedSerial, 0, 0);
                    if (scInputSerialW > scInputServedSerialW)
                    {
                        const int scValueW = block->spectateConfirmInputValue;
                        char scLineW[16] = {};
                        std::snprintf(scLineW, sizeof(scLineW), "%d\r\n", scValueW);
                        InterlockedExchange(&block->spectateConfirmInputServedSerial, scInputSerialW);
                        InterlockedExchange(&g_injectedSpectateConfirmPromptServedSerial, scPromptSerialW);
                        InterlockedExchange(&block->spectateConfirmPromptServedSerial, scPromptSerialW);
                        g_injectedSpectateConfirmPromptWaitStartTick = 0;
                        return serveScriptedInput(block, scLineW, sourceTag, "prompt_spectate_confirm_selected", scInputSerialW, true);
                    }

                    const DWORD nowTickScW = GetTickCount();
                    if (g_injectedSpectateConfirmPromptWaitStartTick == 0)
                    {
                        g_injectedSpectateConfirmPromptWaitStartTick = nowTickScW;
                        mod::Log(
                            "Takeover: spectate confirm prompt waiting for overlay selection promptSerial=%ld",
                            static_cast<long>(scPromptSerialW));
                    }
                    if (nowTickScW - g_injectedSpectateConfirmPromptWaitStartTick < kPromptSpectateConfirmWaitTimeoutMs)
                    {
                        if (consoleEvent != nullptr)
                        {
                            const DWORD wait = WaitForSingleObject(consoleEvent, 200);
                            if (wait == WAIT_FAILED)
                            {
                                break;
                            }
                        }
                        else
                        {
                            Sleep(10);
                        }
                        continue;
                    }

                    int defaultChoiceW = 2;
                    const int promptKindW = block->spectateConfirmPromptKind;
                    if (promptKindW == static_cast<int>(NetbridgeSpectatePromptKind::HostNotYetPlaying))
                    {
                        defaultChoiceW = 3;
                    }
                    char defaultLineW[16] = {};
                    std::snprintf(defaultLineW, sizeof(defaultLineW), "%d\r\n", defaultChoiceW);
                    InterlockedExchange(&g_injectedSpectateConfirmPromptServedSerial, scPromptSerialW);
                    InterlockedExchange(&block->spectateConfirmPromptServedSerial, scPromptSerialW);
                    g_injectedSpectateConfirmPromptWaitStartTick = 0;
                    mod::Log(
                        "Takeover: spectate confirm prompt timed out; falling back to choice=%d kind=%d promptSerial=%ld",
                        defaultChoiceW,
                        promptKindW,
                        static_cast<long>(scPromptSerialW));
                    return serveScriptedInput(block, defaultLineW, sourceTag, "prompt_spectate_confirm_default", scPromptSerialW, true);
                }

                // --- Delay prompt handling (W) ---
                const LONG promptSerial = InterlockedCompareExchange(&g_injectedDelayPromptSerial, 0, 0);
                const LONG promptServed = InterlockedCompareExchange(&g_injectedDelayPromptServedSerial, 0, 0);
                if (promptSerial > 0 && promptSerial != promptServed)
                {
                    const LONG delayInputSerial = InterlockedCompareExchange(&block->delayInputSerial, 0, 0);
                    const LONG delayInputServedSerial = InterlockedCompareExchange(&block->delayInputServedSerial, 0, 0);
                    if (delayInputSerial > delayInputServedSerial)
                    {
                        const int delayValue = block->delayInputValue;
                        char delayLine[16] = {};
                        std::snprintf(delayLine, sizeof(delayLine), "%d\r\n", delayValue);
                        InterlockedExchange(&block->delayInputServedSerial, delayInputSerial);
                        InterlockedExchange(&g_injectedDelayPromptServedSerial, promptSerial);
                        InterlockedExchange(&block->delayPromptServedSerial, promptSerial);
                        g_injectedDelayPromptWaitStartTick = 0;
                        return serveScriptedInput(block, delayLine, sourceTag, "prompt_delay_selected", delayInputSerial, true);
                    }

                    const DWORD nowTick = GetTickCount();
                    if (g_injectedDelayPromptWaitStartTick == 0)
                    {
                        g_injectedDelayPromptWaitStartTick = nowTick;
                        mod::Log(
                            "Takeover: delay prompt waiting for overlay selection promptSerial=%ld",
                            static_cast<long>(promptSerial));
                    }
                    if (nowTick - g_injectedDelayPromptWaitStartTick < kPromptDelayInputWaitTimeoutMs)
                    {
                        if (consoleEvent != nullptr)
                        {
                            const DWORD wait = WaitForSingleObject(consoleEvent, 200);
                            if (wait == WAIT_FAILED)
                            {
                                break;
                            }
                        }
                        else
                        {
                            Sleep(10);
                        }
                        continue;
                    }

                    InterlockedExchange(&g_injectedDelayPromptServedSerial, promptSerial);
                    InterlockedExchange(&block->delayPromptServedSerial, promptSerial);
                    g_injectedDelayPromptWaitStartTick = 0;
                    mod::Log(
                        "Takeover: delay prompt timed out; falling back to default promptSerial=%ld",
                        static_cast<long>(promptSerial));
                    return serveScriptedInput(block, "\r\n", sourceTag, "prompt_delay_default", promptSerial, true);
                }
                return FALSE;
            }

            if (consoleEvent != nullptr)
            {
                const DWORD wait = WaitForSingleObject(consoleEvent, 200);
                if (wait == WAIT_FAILED)
                {
                    break;
                }
                continue;
            }

            Sleep(10);
        }

        return FALSE;
    };

    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (HasInjectedContext() && waitAndServe(g_injectedBlock, g_injectedConsoleEvent, "injected"))
    {
        return TRUE;
    }

    TempIpcContext temp = {};
    if (OpenTempIpcContext(&temp, false, true))
    {
        const BOOL ok = waitAndServe(temp.block, temp.consoleEvent, "fallback");
        CloseTempIpcContext(&temp);
        if (ok)
        {
            return TRUE;
        }
    }

    SetLastError(NO_ERROR);
    const BOOL nativeResult = ReadConsoleW(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, pInputControl);
    const DWORD nativeError = GetLastError();
    const DWORD nativeRead = (lpNumberOfCharsRead != nullptr) ? *lpNumberOfCharsRead : 0;
    mod::Log(
        "nb_stub_ReadConsoleW: passthrough (ready=%d result=%d read=%lu err=%lu)",
        HasInjectedContext() ? 1 : 0,
        nativeResult ? 1 : 0,
        static_cast<unsigned long>(nativeRead),
        static_cast<unsigned long>(nativeError));
    return nativeResult;
}

BOOL StubWriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
    const BOOL result = WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
    MaybeLogConsoleOutputChunk(hFile, lpBuffer, nNumberOfBytesToWrite);
    return result;
}

HANDLE StubCreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    if (HasLogEfzBaseNameA(lpFileName))
    {
        const std::string redirectPath = GetNativeShadowLogEfzPathA();
        if (!redirectPath.empty())
        {
            mod::Log(
                "CAPTURE_LOG: redirected native logEfz CreateFileA original='%s' redirect='%s'",
                lpFileName,
                redirectPath.c_str());
            return CreateFileA(
                redirectPath.c_str(),
                dwDesiredAccess,
                dwShareMode,
                lpSecurityAttributes,
                dwCreationDisposition,
                dwFlagsAndAttributes,
                hTemplateFile);
        }
    }

    return CreateFileA(
        lpFileName,
        dwDesiredAccess,
        dwShareMode,
        lpSecurityAttributes,
        dwCreationDisposition,
        dwFlagsAndAttributes,
        hTemplateFile);
}

HANDLE StubCreateFileW(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    if (HasLogEfzBaseNameW(lpFileName))
    {
        const std::wstring redirectPath = GetNativeShadowLogEfzPathW();
        if (!redirectPath.empty())
        {
            char originalUtf8[MAX_PATH * 2] = {};
            char redirectUtf8[MAX_PATH * 2] = {};
            WideCharToMultiByte(CP_UTF8, 0, lpFileName, -1, originalUtf8, static_cast<int>(sizeof(originalUtf8)), nullptr, nullptr);
            WideCharToMultiByte(CP_UTF8, 0, redirectPath.c_str(), -1, redirectUtf8, static_cast<int>(sizeof(redirectUtf8)), nullptr, nullptr);
            mod::Log(
                "CAPTURE_LOG: redirected native logEfz CreateFileW original='%s' redirect='%s'",
                originalUtf8,
                redirectUtf8);
            return CreateFileW(
                redirectPath.c_str(),
                dwDesiredAccess,
                dwShareMode,
                lpSecurityAttributes,
                dwCreationDisposition,
                dwFlagsAndAttributes,
                hTemplateFile);
        }
    }

    return CreateFileW(
        lpFileName,
        dwDesiredAccess,
        dwShareMode,
        lpSecurityAttributes,
        dwCreationDisposition,
        dwFlagsAndAttributes,
        hTemplateFile);
}

BOOL StubWriteConsoleA(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved)
{
    const BOOL result = WriteConsoleA(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved);
    MaybeLogConsoleWriteAChunk(lpBuffer, nNumberOfCharsToWrite);
    return result;
}

BOOL StubWriteConsoleW(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved)
{
    const BOOL result = WriteConsoleW(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved);
    MaybeLogConsoleWriteWChunk(lpBuffer, nNumberOfCharsToWrite);
    return result;
}

BOOL StubWriteConsoleOutputCharacterA(HANDLE hConsoleOutput, LPCSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten)
{
    const BOOL result = WriteConsoleOutputCharacterA(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
    MaybeLogConsoleOutputCharacterAChunk(lpCharacter, nLength, dwWriteCoord);
    return result;
}

BOOL StubWriteConsoleOutputCharacterW(HANDLE hConsoleOutput, LPCWSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten)
{
    const BOOL result = WriteConsoleOutputCharacterW(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
    MaybeLogConsoleOutputCharacterWChunk(lpCharacter, nLength, dwWriteCoord);
    return result;
}

VOID StubOutputDebugStringA(LPCSTR lpOutputString)
{
    OutputDebugStringA(lpOutputString);
    MaybeLogOutputDebugStringA(lpOutputString);
}

VOID StubOutputDebugStringW(LPCWSTR lpOutputString)
{
    OutputDebugStringW(lpOutputString);
    MaybeLogOutputDebugStringW(lpOutputString);
}

DWORD StubWaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    DWORD fakeExit = 0;
    if (LookupFakeThread(hHandle, &fakeExit))
    {
        (void)fakeExit;
        return WAIT_OBJECT_0;
    }
    return WaitForSingleObject(hHandle, dwMilliseconds);
}

BOOL StubGetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode)
{
    DWORD fakeExit = 0;
    if (LookupFakeThread(hThread, &fakeExit))
    {
        if (lpExitCode != nullptr)
        {
            *lpExitCode = fakeExit;
        }
        return TRUE;
    }
    return GetExitCodeThread(hThread, lpExitCode);
}

DWORD StubResumeThread(HANDLE hThread)
{
    if (!HasInjectedContext())
    {
        (void)EnsureInjectedContextFast();
    }
    if (HasInjectedContext())
    {
        DWORD fakeExit = 0;
        if (hThread == g_fakeProcessThreadHandle || LookupFakeThread(hThread, &fakeExit))
        {
            return 1;
        }
    }
    return ResumeThread(hThread);
}

} // namespace netplay::bridge::takeover

// ---------------------------------------------------------------------------
// extern "C" wrappers that forward to the namespaced Stub* implementations.
// ---------------------------------------------------------------------------

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_CreateProcessA(
    LPCSTR lpApplicationName,
    LPSTR lpCommandLine,
    LPSECURITY_ATTRIBUTES lpProcessAttributes,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    BOOL bInheritHandles,
    DWORD dwCreationFlags,
    LPVOID lpEnvironment,
    LPCSTR lpCurrentDirectory,
    LPSTARTUPINFOA lpStartupInfo,
    LPPROCESS_INFORMATION lpProcessInformation)
{
    return netplay::bridge::takeover::StubCreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes, bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo, lpProcessInformation);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadProcessMemory(HANDLE hProcess, LPCVOID lpBaseAddress, LPVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesRead)
{
    return netplay::bridge::takeover::StubReadProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead);
}

extern "C" __declspec(dllexport) LPVOID WINAPI nb_stub_VirtualAllocEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect)
{
    return netplay::bridge::takeover::StubVirtualAllocEx(hProcess, lpAddress, dwSize, flAllocationType, flProtect);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_VirtualFreeEx(HANDLE hProcess, LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType)
{
    return netplay::bridge::takeover::StubVirtualFreeEx(hProcess, lpAddress, dwSize, dwFreeType);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteProcessMemory(HANDLE hProcess, LPVOID lpBaseAddress, LPCVOID lpBuffer, SIZE_T nSize, SIZE_T* lpNumberOfBytesWritten)
{
    return netplay::bridge::takeover::StubWriteProcessMemory(hProcess, lpBaseAddress, lpBuffer, nSize, lpNumberOfBytesWritten);
}

extern "C" __declspec(dllexport) HANDLE WINAPI nb_stub_CreateRemoteThread(HANDLE hProcess, LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize, LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags, LPDWORD lpThreadId)
{
    return netplay::bridge::takeover::StubCreateRemoteThread(hProcess, lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_TerminateProcess(HANDLE hProcess, UINT uExitCode)
{
    return netplay::bridge::takeover::StubTerminateProcess(hProcess, uExitCode);
}

extern "C" __declspec(dllexport) HANDLE WINAPI nb_stub_OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId)
{
    return netplay::bridge::takeover::StubOpenProcess(dwDesiredAccess, bInheritHandle, dwProcessId);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadConsoleA(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl)
{
    return netplay::bridge::takeover::StubReadConsoleA(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, pInputControl);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_ReadConsoleW(HANDLE hConsoleInput, LPVOID lpBuffer, DWORD nNumberOfCharsToRead, LPDWORD lpNumberOfCharsRead, PCONSOLE_READCONSOLE_CONTROL pInputControl)
{
    return netplay::bridge::takeover::StubReadConsoleW(hConsoleInput, lpBuffer, nNumberOfCharsToRead, lpNumberOfCharsRead, pInputControl);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped)
{
    return netplay::bridge::takeover::StubWriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
}

extern "C" __declspec(dllexport) HANDLE WINAPI nb_stub_CreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    return netplay::bridge::takeover::StubCreateFileA(
        lpFileName,
        dwDesiredAccess,
        dwShareMode,
        lpSecurityAttributes,
        dwCreationDisposition,
        dwFlagsAndAttributes,
        hTemplateFile);
}

extern "C" __declspec(dllexport) HANDLE WINAPI nb_stub_CreateFileW(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    return netplay::bridge::takeover::StubCreateFileW(
        lpFileName,
        dwDesiredAccess,
        dwShareMode,
        lpSecurityAttributes,
        dwCreationDisposition,
        dwFlagsAndAttributes,
        hTemplateFile);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleA(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved)
{
    return netplay::bridge::takeover::StubWriteConsoleA(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleW(HANDLE hConsoleOutput, const VOID* lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved)
{
    return netplay::bridge::takeover::StubWriteConsoleW(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleOutputCharacterA(HANDLE hConsoleOutput, LPCSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten)
{
    return netplay::bridge::takeover::StubWriteConsoleOutputCharacterA(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_WriteConsoleOutputCharacterW(HANDLE hConsoleOutput, LPCWSTR lpCharacter, DWORD nLength, COORD dwWriteCoord, LPDWORD lpNumberOfCharsWritten)
{
    return netplay::bridge::takeover::StubWriteConsoleOutputCharacterW(hConsoleOutput, lpCharacter, nLength, dwWriteCoord, lpNumberOfCharsWritten);
}

extern "C" __declspec(dllexport) VOID WINAPI nb_stub_OutputDebugStringA(LPCSTR lpOutputString)
{
    netplay::bridge::takeover::StubOutputDebugStringA(lpOutputString);
}

extern "C" __declspec(dllexport) VOID WINAPI nb_stub_OutputDebugStringW(LPCWSTR lpOutputString)
{
    netplay::bridge::takeover::StubOutputDebugStringW(lpOutputString);
}

extern "C" __declspec(dllexport) DWORD WINAPI nb_stub_WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
    return netplay::bridge::takeover::StubWaitForSingleObject(hHandle, dwMilliseconds);
}

extern "C" __declspec(dllexport) BOOL WINAPI nb_stub_GetExitCodeThread(HANDLE hThread, LPDWORD lpExitCode)
{
    return netplay::bridge::takeover::StubGetExitCodeThread(hThread, lpExitCode);
}

extern "C" __declspec(dllexport) DWORD WINAPI nb_stub_ResumeThread(HANDLE hThread)
{
    return netplay::bridge::takeover::StubResumeThread(hThread);
}

extern "C" __declspec(dllexport) DWORD WINAPI nb_stub_RequestPeerQuitBroadcast(LPVOID)
{
    return netplay::bridge::takeover::RunInjectedPeerQuitBroadcast();
}
