#include "netplay/hooks/internal/shared.h"

#include "logger.h"
#include "netplay/assets/assets.h"
#include "netplay/core/battle_log_menu.h"
#include "netplay/hooks/debug_overlay.h"

namespace netplay
{
bool InstallHooks()
{
    using namespace netplay::constants;
    using namespace netplay::hooks::internal;

    std::lock_guard<std::mutex> lock(g_patchMutex);

    if (g_hooksInstalled.load())
    {
        mod::Log("InstallHooks: already installed");
        return true;
    }

    if (sizeof(void*) != 4)
    {
        mod::Log("InstallHooks: this mod requires x86 process");
        return false;
    }

#if !defined(_M_IX86)
    mod::Log("InstallHooks: this build target is not x86");
    return false;
#else
    g_exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (g_exeBase == 0)
    {
        mod::Log("InstallHooks: failed to resolve exe base");
        return false;
    }

    g_moduleDirectory = netplay::assets::BuildModuleDirectory(ResolveCurrentModule());
    mod::Log("InstallHooks: module directory '%s'", g_moduleDirectory.c_str());

    // ----- Early asset availability check ----------------------------------
    // Probe for the netplay background .dat using all fallback tiers.
    // If it cannot be found anywhere, skip the title menu expansion patches
    // so the original 7-entry menu is preserved (no "Netplay" button).
    // This handles Wine/Proton scenarios where the modloader can't resolve
    // the DLL's directory and assets are unreachable, as well as
    // installations that are simply missing the asset files.
    const std::string probeBg = netplay::assets::ResolveNetplayBackgroundPath(g_moduleDirectory);
    g_netplayAssetsAvailable = !probeBg.empty();
    if (!g_netplayAssetsAvailable)
    {
        mod::Log("InstallHooks: WARNING - netplay assets not found, netplay menu will be disabled");
        mod::Log("InstallHooks: looked in DLL dir '%s', mods\\efz_netplay_mod\\, working dir, system\\", g_moduleDirectory.c_str());
    }
    else
    {
        mod::Log("InstallHooks: netplay assets verified at '%s'", probeBg.c_str());
    }

    if (!netplay::menu::ValidateMenuSpecs())
    {
        mod::Log("InstallHooks: menu spec validation failed");
        return false;
    }

    g_titleCaseReturnAddress = static_cast<uint32_t>(RuntimeAddress(kVaTitleCaseEpilogue));

    g_customDispatchTable[0] = static_cast<uint32_t>(RuntimeAddress(kVaCaseArcade));
    g_customDispatchTable[1] = static_cast<uint32_t>(RuntimeAddress(kVaCaseVsCpu));
    g_customDispatchTable[2] = static_cast<uint32_t>(RuntimeAddress(kVaCaseVsHuman));
    g_customDispatchTable[3] = static_cast<uint32_t>(RuntimeAddress(kVaCasePractice));
    g_replayCaseDispatchAddress = static_cast<uint32_t>(RuntimeAddress(kVaCaseReplay));
    g_customDispatchTable[4] = g_replayCaseDispatchAddress;
    g_customDispatchTable[5] = reinterpret_cast<uint32_t>(&NetplayCaseThunk);
    g_customDispatchTable[6] = static_cast<uint32_t>(RuntimeAddress(kVaCaseOptions));
    g_customDispatchTable[7] = static_cast<uint32_t>(RuntimeAddress(kVaCaseExit));

    auto dwordToBytes = [](uint32_t value) -> std::vector<uint8_t>
    {
        return {
            static_cast<uint8_t>(value & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 24) & 0xFF),
        };
    };

    const uintptr_t addrWrapCmpMax = RuntimeAddress(kVaWrapCmpMax);
    const uintptr_t addrWrapClampNegative = RuntimeAddress(kVaWrapClampNegative);
    const uintptr_t addrSwitchCmpMax = RuntimeAddress(kVaSwitchCmpMax);
    const uintptr_t addrSwitchTableDisp = RuntimeAddress(kVaSwitchTableDisp);
    const uintptr_t addrRenderPanelDestH = RuntimeAddress(kVaRenderPanelDestH);
    const uintptr_t addrRenderPanelSourceH = RuntimeAddress(kVaRenderPanelSourceH);
    const uintptr_t addrRenderPanelDestTop = RuntimeAddress(kVaRenderPanelDestTop);
    const uintptr_t addrRenderHighlightDestBase = RuntimeAddress(kVaRenderHighlightDestBase);
    const uintptr_t addrRenderHighlightScreenBase = RuntimeAddress(kVaRenderHighlightScreenBase);
    const uintptr_t addrTitleVtableRender = RuntimeAddress(kVaTitleVtableRender);
    const uintptr_t addrTitleVtableUpdate = RuntimeAddress(kVaTitleVtableUpdate);


    bool ok = true;

    // ---- Netplay menu expansion patches -----------------------------------
    // These patches add the 8th menu entry ("Netplay") to the title screen.
    // Only applied when netplay assets were found; otherwise the original
    // 7-entry title menu is preserved unchanged.
    if (g_netplayAssetsAvailable)
    {
        ok = ok && ApplyPatch(addrWrapCmpMax, {0x83, 0xF8, 0x06}, {0x83, 0xF8, 0x07}, "wrap compare 6->7");
        ok = ok && ApplyPatch(
            addrWrapClampNegative,
            {0x8B, 0x4D, 0xF8, 0xC6, 0x81, 0x3C, 0x04, 0x00, 0x00, 0x06},
            {0x8B, 0x4D, 0xF8, 0xC6, 0x81, 0x3C, 0x04, 0x00, 0x00, 0x07},
            "wrap clamp 6->7");
        ok = ok && ApplyPatch(addrSwitchCmpMax, {0x83, 0x7D, 0xF4, 0x06}, {0x83, 0x7D, 0xF4, 0x07}, "switch compare 6->7");
        ok = ok && ApplyPatch(addrRenderPanelDestH, {0x6A, 0x62}, {0x6A, 0x70}, "panel dest height 98->112");
        ok = ok && ApplyPatch(addrRenderPanelSourceH, {0x6A, 0x62}, {0x6A, 0x70}, "panel src height 98->112");
        ok = ok && ApplyPatch(
            addrRenderPanelDestTop,
            {0x68, 0x82, 0x00, 0x00, 0x00},
            {0x68,
             static_cast<uint8_t>(kMenuDestTopPatched & 0xFF),
             static_cast<uint8_t>((kMenuDestTopPatched >> 8) & 0xFF),
             static_cast<uint8_t>((kMenuDestTopPatched >> 16) & 0xFF),
             static_cast<uint8_t>((kMenuDestTopPatched >> 24) & 0xFF)},
            "panel dest top 130->123");
        ok = ok && ApplyPatch(
            addrRenderHighlightDestBase,
            {0x83, 0xC0, 0x62},
            {0x83, 0xC0, 0x70},
            "highlight src base 98->112");
        ok = ok && ApplyPatch(
            addrRenderHighlightScreenBase,
            {0x81, 0xC2, 0x82, 0x00, 0x00, 0x00},
            {0x81, 0xC2,
             static_cast<uint8_t>(kMenuDestTopPatched & 0xFF),
             static_cast<uint8_t>((kMenuDestTopPatched >> 8) & 0xFF),
             static_cast<uint8_t>((kMenuDestTopPatched >> 16) & 0xFF),
             static_cast<uint8_t>((kMenuDestTopPatched >> 24) & 0xFF)},
            "highlight screen base 130->123");

        const uint32_t dispatchAddress = reinterpret_cast<uint32_t>(&g_customDispatchTable[0]);
        ok = ok && ApplyPatch(
            addrSwitchTableDisp,
            {0x87, 0x64, 0x77, 0x00},
            dwordToBytes(dispatchAddress),
            "switch dispatch table -> custom");
    }
    else
    {
        mod::Log("InstallHooks: skipping netplay menu patches (assets unavailable)");
    }
    ok = ok && ApplyPatch(
        addrTitleVtableRender,
        dwordToBytes(static_cast<uint32_t>(RuntimeAddress(kVaTitleRender))),
        dwordToBytes(reinterpret_cast<uint32_t>(&HookedTitleRenderThunk)),
        "title vtable render -> hook");
    ok = ok && ApplyPatch(
        addrTitleVtableUpdate,
        dwordToBytes(static_cast<uint32_t>(RuntimeAddress(kVaUpdateTitleScreenLogic))),
        dwordToBytes(reinterpret_cast<uint32_t>(&HookedTitleUpdateThunk)),
        "title vtable update -> hook");


    if (!ok)
    {
        RestorePatches();
        mod::Log("InstallHooks: patch application failed");
        return false;
    }

    g_titleUpdateCallCount = 0;
    g_netplayUpdateCallCount = 0;
    g_hooksInstalled.store(true);
    mod::Log(
        "InstallHooks: success (title update=0x%08X netplayAssets=%d)",
        static_cast<unsigned>(RuntimeAddress(kVaUpdateTitleScreenLogic)),
        g_netplayAssetsAvailable ? 1 : 0);
    return true;
#endif
}

void RemoveHooks()
{
    using namespace netplay::hooks::internal;

    std::lock_guard<std::mutex> lock(g_patchMutex);

    if (!g_hooksInstalled.load())
    {
        return;
    }

    g_netplayMenuState = {};
    g_netplayAssetsAvailable = false;
    g_spriteFont = {};
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = false;
    g_restoreReplaySelectionOnNextTitleUpdate = false;
    g_replaySelectionGuardFramesRemaining = 0;
    g_replaySelectionRestoreTarget = -1;
    g_replayCaseDispatchAddress = 0;
    const bool windowOk = RemoveNetplayWindowHook();
    const bool imguiOk = netplay::debug_overlay::SuspendForOnlineSimulation();
    const bool overlayOk = netplay::battle_log::ShutdownRenderOverlay();
    if (!windowOk || !imguiOk || !overlayOk)
    {
        mod::Log(
            "RemoveHooks: UI teardown incomplete window=%d imgui=%d overlay=%d",
            windowOk ? 1 : 0,
            imguiOk ? 1 : 0,
            overlayOk ? 1 : 0);
    }
    RestorePatches();
    g_hooksInstalled.store(false);
    mod::Log("RemoveHooks: restored original bytes");
}
} // namespace netplay
