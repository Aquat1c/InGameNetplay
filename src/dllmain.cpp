#include <windows.h>

#include "logger.h"
#include "netplay_menu_hooks.h"

namespace
{
DWORD WINAPI InitializeModThread(LPVOID moduleHandleRaw)
{
    const auto moduleHandle = static_cast<HMODULE>(moduleHandleRaw);
    mod::InitializeLogger(moduleHandle);
    mod::Log("Module attached at %p", moduleHandle);

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
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID lpReserved)
{
    switch (ulReasonForCall)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE thread = CreateThread(nullptr, 0, InitializeModThread, hModule, 0, nullptr);
        if (thread != nullptr)
        {
            CloseHandle(thread);
        }
        break;
    }
    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr)
        {
            netplay::RemoveHooks();
        }
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
