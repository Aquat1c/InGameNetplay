#include <windows.h>

#include <cstdint>

#include "crash_handler.h"
#include "logger.h"
#include "netplay/core/mod_settings.h"
#include "netplay/bridge/external_launcher_guard.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/bridge/revival_takeover.h"
#include "netplay/bridge/netplay_state_export.h"
#include "netplay/hooks/menu_hooks.h"

namespace
{
volatile LONG g_passiveInitialization = 0;

DWORD WINAPI InitializeModThread(LPVOID moduleHandleRaw)
{
    const auto moduleHandle = static_cast<HMODULE>(moduleHandleRaw);
    bool startupCompleted = false;
    __try
    {
        netplay::mod_settings::Reload();
        mod::InitializeLogger(
            moduleHandle,
            netplay::mod_settings::IsConsoleEnabled(),
            netplay::mod_settings::IsFileLoggingEnabled());
        mod::InstallCrashHandlers(moduleHandle, false);
        mod::Log("Module attached at %p", moduleHandle);

        if (!netplay::bridge::Initialize())
        {
            mod::Log(
                "Bridge initialization stayed passive; skipping all game/UI hooks");
            InterlockedExchange(&g_passiveInitialization, 1);
            mod::UninstallCrashHandlers();
            mod::ShutdownLogger();
            return 0;
        }

        const bool hooksInstalled = netplay::InstallHooks();
        if (!hooksInstalled)
        {
            mod::Log("InstallHooks failed");
        }
        else
        {
            mod::Log("InstallHooks succeeded");
        }
        if (!netplay::bridge::CompleteLauncherUiAttachment(hooksInstalled))
        {
            mod::Log(
                "Launcher UI attachment boundary completion failed");
        }
        startupCompleted = true;
    }
    __finally
    {
        if (!startupCompleted)
        {
            // A blind timeout is unsafe while title code may be mid-patch.
            // The worker's SEH termination handler is the only asynchronous
            // release authority: it publishes managed recovery first, then
            // atomically quarantines a still-pending adopted tick.
            netplay::bridge::EmergencyQuarantineLauncherUiAttachment();
        }
    }

    return 0;
}
DWORD WINAPI InitializeInjectedThread(LPVOID moduleHandleRaw)
{
    const auto moduleHandle = static_cast<HMODULE>(moduleHandleRaw);
    netplay::mod_settings::Reload();
    mod::InitializeLogger(
        moduleHandle,
        false,
        netplay::mod_settings::IsFileLoggingEnabled());
    mod::InstallCrashHandlers(moduleHandle, true);
    mod::Log("Module attached in EfzRevival.exe (injected takeover mode)");
    netplay::bridge::InitializeInjectedProcess();
    return 0;
}

netplay::bridge::NetbridgeRole ToBridgeRole(int role)
{
    switch (role)
    {
    case 0:
        return netplay::bridge::NetbridgeRole::Host;
    case 2:
        return netplay::bridge::NetbridgeRole::Spectate;
    default:
        return netplay::bridge::NetbridgeRole::Join;
    }
}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID lpReserved)
{
    switch (ulReasonForCall)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);

        // An externally launched, mod-owned Revival session injects this DLL
        // back into its verified parent solely to neutralize one exact cleanup
        // TerminateProcess call. Detect that marker before Wine's broad IAT
        // patch or the ordinary injected-helper bootstrap can run.
        if (netplay::bridge::takeover::TryStartExternalLauncherGuardProcess())
        {
            break;
        }

        if (netplay::bridge::IsCurrentProcessRevival())
        {
            // Under Wine/Proton the helper process is NOT created suspended,
            // so main() will start as soon as the loader lock is released.
            // Patch the EXE's IAT right here - inside DllMain - so all
            // import entries point to our stubs BEFORE main() can call
            // ReadConsoleA, CreateProcessA, etc. through the original IAT.
            // This eliminates the race between main() and the host-side
            // remote PatchIat() call.
            if (netplay::bridge::IsRunningUnderWine())
            {
                netplay::bridge::SelfPatchIat();
            }

            HANDLE injectedThread = CreateThread(nullptr, 0, InitializeInjectedThread, hModule, 0, nullptr);
            if (injectedThread != nullptr)
            {
                CloseHandle(injectedThread);
            }
            break;
        }

        // In launcher-first mode, native Revival installs its Tournament EXE
        // hooks immediately after this DllMain returns. Preserve the actual
        // preimage now so later managed cleanup restores prior mod ownership
        // instead of guessing stock bytes.
        netplay::bridge::takeover::
            CaptureTournamentExePreimageAtProcessAttach();

        HANDLE thread = CreateThread(nullptr, 0, InitializeModThread, hModule, 0, nullptr);
        if (thread != nullptr)
        {
            CloseHandle(thread);
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        if (netplay::bridge::takeover::IsExternalLauncherGuardProcess())
        {
            netplay::bridge::takeover::ShutdownExternalLauncherGuardProcess();
            break;
        }

        if (netplay::bridge::IsCurrentProcessRevival())
        {
            netplay::bridge::ShutdownInjectedProcess();
            mod::UninstallCrashHandlers();
            mod::ShutdownLogger();
            break;
        }

        // A fail-closed launcher admission already unwound the transient
        // logger/crash-handler setup and installed no game/UI hooks.
        if (InterlockedCompareExchange(
                &g_passiveInitialization, 0, 0) != 0)
        {
            break;
        }

        if (lpReserved == nullptr)
        {
            netplay::RemoveHooks();
            netplay::bridge::Shutdown();
        }
        else
        {
            netplay::bridge::EmergencyShutdown();
        }
        mod::UninstallCrashHandlers();
        mod::ShutdownLogger();
        break;
    default:
        break;
    }

    return TRUE;
}

extern "C" __declspec(dllexport) void EFZNetplayShowStubMessageBox(HWND owner)
{
    netplay::ShowInProgressMessage(owner);
}

extern "C" __declspec(dllexport) void __cdecl
netbridge_StartNetplaySession(int role, std::uint16_t port, const char* address, const char* nickname)
{
    mod::Log(
        "netbridge_StartNetplaySession: role=%d port=%u address='%s' nickname='%s'",
        role,
        static_cast<unsigned>(port),
        (address != nullptr) ? address : "",
        (nickname != nullptr) ? nickname : "");
    (void)netplay::bridge::StartSession(ToBridgeRole(role), port, address, nickname);
}

extern "C" __declspec(dllexport) netplay::bridge::NetbridgeStatus __cdecl
netbridge_GetStatus(void)
{
    return netplay::bridge::GetStatus();
}

extern "C" __declspec(dllexport) void __cdecl
netbridge_CancelSession(void)
{
    mod::Log("netbridge_CancelSession called");
    netplay::bridge::CancelSession("external_cancel");
}

extern "C" __declspec(dllexport) const EFZNetplayState* __cdecl
EFZNetplay_GetState(void)
{
    return netplay::bridge::state_export::GetExportedState();
}


