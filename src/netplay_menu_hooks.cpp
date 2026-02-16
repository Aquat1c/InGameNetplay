#include "netplay_menu_hooks.h"

#include "logger.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
constexpr uintptr_t kEfzImageBase = 0x00400000;

constexpr uintptr_t kVaPlaySoundEffect = 0x00406860;

constexpr uintptr_t kVaUpdateTitleScreenLogic = 0x00775FB0;
constexpr uintptr_t kVaTitleCaseEpilogue = 0x00776483;

constexpr uintptr_t kVaWrapCmpMax = 0x0077612E;       // cmp eax, 6
constexpr uintptr_t kVaWrapClampNegative = 0x0077614B; // mov ... , 6
constexpr uintptr_t kVaSwitchCmpMax = 0x007761D5;      // cmp [ebp-0Ch], 6
constexpr uintptr_t kVaSwitchTableDisp = 0x007761E5;   // dword disp in jmp [ecx*4 + disp32]

constexpr uintptr_t kVaRenderPanelDestH = 0x00776578;     // push 62h
constexpr uintptr_t kVaRenderPanelSourceH = 0x007765A3;   // push 62h
constexpr uintptr_t kVaRenderHighlightDestBase = 0x0077664A; // add eax, 62h

constexpr uintptr_t kVaCaseArcade = 0x007761E9;
constexpr uintptr_t kVaCaseVsCpu = 0x00776268;
constexpr uintptr_t kVaCaseVsHuman = 0x007762E3;
constexpr uintptr_t kVaCasePractice = 0x00776352;
constexpr uintptr_t kVaCaseReplay = 0x007763D1;
constexpr uintptr_t kVaCaseOptions = 0x00776432;
constexpr uintptr_t kVaCaseExit = 0x0077645F;

struct PatchRecord
{
    uintptr_t address;
    std::vector<uint8_t> originalBytes;
};

std::mutex g_patchMutex;
std::atomic<bool> g_hooksInstalled{false};
uintptr_t g_exeBase = 0;

std::vector<PatchRecord> g_appliedPatches;

uint32_t g_customDispatchTable[8] = {};
extern "C" uint32_t g_titleCaseReturnAddress = 0;

uintptr_t RuntimeAddress(uintptr_t va)
{
    return g_exeBase + (va - kEfzImageBase);
}

bool WriteProcessMemoryLocal(uintptr_t address, const uint8_t* data, size_t size)
{
    DWORD oldProtect = 0;
    if (VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect) == FALSE)
    {
        mod::Log("VirtualProtect failed at 0x%08X", static_cast<unsigned>(address));
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(address), data, size);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);

    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &tmp);
    return true;
}

bool ApplyPatch(
    uintptr_t address,
    const std::vector<uint8_t>& expectedBytes,
    const std::vector<uint8_t>& patchedBytes,
    const char* label)
{
    if (patchedBytes.empty())
    {
        return true;
    }

    std::vector<uint8_t> currentBytes(patchedBytes.size());
    std::memcpy(currentBytes.data(), reinterpret_cast<void*>(address), currentBytes.size());

    if (!expectedBytes.empty() && expectedBytes != currentBytes)
    {
        mod::Log("Patch '%s' mismatch at 0x%08X", label, static_cast<unsigned>(address));
        return false;
    }

    if (!WriteProcessMemoryLocal(address, patchedBytes.data(), patchedBytes.size()))
    {
        mod::Log("Patch '%s' write failed at 0x%08X", label, static_cast<unsigned>(address));
        return false;
    }

    g_appliedPatches.push_back(PatchRecord{address, std::move(currentBytes)});
    mod::Log("Patched '%s' at 0x%08X", label, static_cast<unsigned>(address));
    return true;
}

void RestorePatches()
{
    for (auto it = g_appliedPatches.rbegin(); it != g_appliedPatches.rend(); ++it)
    {
        (void)WriteProcessMemoryLocal(it->address, it->originalBytes.data(), it->originalBytes.size());
    }
    g_appliedPatches.clear();
}

using PlaySoundEffectFn = int(__thiscall*)(void* gameSystem, unsigned short soundIndex);

void TriggerNetplayStub(uint32_t screenContext)
{
    auto* const gameSystem = *reinterpret_cast<void**>(screenContext + 0x1C);
    auto const playSoundEffect = reinterpret_cast<PlaySoundEffectFn>(RuntimeAddress(kVaPlaySoundEffect));

    if (gameSystem != nullptr)
    {
        playSoundEffect(gameSystem, 6);
    }

    const HWND owner = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + 0x08));
    netplay::ShowInProgressMessage(owner);
}

#if defined(_M_IX86)
extern "C" void __cdecl NetplayCaseImpl(uint32_t screenContext)
{
    TriggerNetplayStub(screenContext);
}

extern "C" __declspec(naked) void NetplayCaseThunk()
{
    __asm
    {
        mov eax, dword ptr [ebp-8]
        push eax
        call NetplayCaseImpl
        add esp, 4
        mov al, 0
        mov eax, dword ptr [g_titleCaseReturnAddress]
        jmp eax
    }
}
#endif
}

namespace netplay
{
bool InstallHooks()
{
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

    g_titleCaseReturnAddress = static_cast<uint32_t>(RuntimeAddress(kVaTitleCaseEpilogue));

    g_customDispatchTable[0] = static_cast<uint32_t>(RuntimeAddress(kVaCaseArcade));
    g_customDispatchTable[1] = static_cast<uint32_t>(RuntimeAddress(kVaCaseVsCpu));
    g_customDispatchTable[2] = static_cast<uint32_t>(RuntimeAddress(kVaCaseVsHuman));
    g_customDispatchTable[3] = static_cast<uint32_t>(RuntimeAddress(kVaCasePractice));
    g_customDispatchTable[4] = reinterpret_cast<uint32_t>(&NetplayCaseThunk);
    g_customDispatchTable[5] = static_cast<uint32_t>(RuntimeAddress(kVaCaseReplay));
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
    const uintptr_t addrRenderHighlightDestBase = RuntimeAddress(kVaRenderHighlightDestBase);

    bool ok = true;
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
        addrRenderHighlightDestBase,
        {0x83, 0xC0, 0x62},
        {0x83, 0xC0, 0x70},
        "highlight src base 98->112");

    const uint32_t dispatchAddress = reinterpret_cast<uint32_t>(&g_customDispatchTable[0]);
    ok = ok && ApplyPatch(
        addrSwitchTableDisp,
        {0x87, 0x64, 0x77, 0x00},
        dwordToBytes(dispatchAddress),
        "switch dispatch table -> custom");

    if (!ok)
    {
        RestorePatches();
        mod::Log("InstallHooks: patch application failed");
        return false;
    }

    g_hooksInstalled.store(true);
    mod::Log(
        "InstallHooks: success (title update=0x%08X custom table=0x%08X)",
        static_cast<unsigned>(RuntimeAddress(kVaUpdateTitleScreenLogic)),
        dispatchAddress);
    return true;
#endif
}

void RemoveHooks()
{
    std::lock_guard<std::mutex> lock(g_patchMutex);

    if (!g_hooksInstalled.load())
    {
        return;
    }

    RestorePatches();
    g_hooksInstalled.store(false);
    mod::Log("RemoveHooks: restored original bytes");
}

bool AreHooksInstalled()
{
    return g_hooksInstalled.load();
}

void ShowInProgressMessage(HWND owner)
{
    MessageBoxA(owner, "In progress", "Netplay", MB_OK | MB_ICONINFORMATION);
}
}
