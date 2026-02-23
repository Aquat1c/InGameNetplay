#include <windows.h>

#include <cstdint>

#include "crash_handler.h"
#include "logger.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/hooks/menu_hooks.h"

namespace
{
DWORD WINAPI InitializeModThread(LPVOID moduleHandleRaw)
{
    const auto moduleHandle = static_cast<HMODULE>(moduleHandleRaw);
    mod::InitializeLogger(moduleHandle);
    mod::InstallCrashHandlers(moduleHandle, false);
    mod::Log("Module attached at %p", moduleHandle);

    netplay::bridge::Initialize();

    if (!netplay::InstallHooks())
    {
        mod::Log("InstallHooks failed");
    }
    else
    {
        mod::Log("InstallHooks succeeded");
    }

    return 0;
}
DWORD WINAPI InitializeInjectedThread(LPVOID moduleHandleRaw)
{
    const auto moduleHandle = static_cast<HMODULE>(moduleHandleRaw);
    mod::InitializeLogger(moduleHandle, false);
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

        if (netplay::bridge::IsCurrentProcessRevival())
        {
            HANDLE injectedThread = CreateThread(nullptr, 0, InitializeInjectedThread, hModule, 0, nullptr);
            if (injectedThread != nullptr)
            {
                CloseHandle(injectedThread);
            }
            break;
        }

        HANDLE thread = CreateThread(nullptr, 0, InitializeModThread, hModule, 0, nullptr);
        if (thread != nullptr)
        {
            CloseHandle(thread);
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        if (netplay::bridge::IsCurrentProcessRevival())
        {
            netplay::bridge::ShutdownInjectedProcess();
            mod::UninstallCrashHandlers();
            mod::ShutdownLogger();
            break;
        }

        if (lpReserved == nullptr)
        {
            netplay::RemoveHooks();
            netplay::bridge::Shutdown();
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


