#include "netplay/bridge/helper_delay_disconnect.h"

#include <cstdint>
#include <cstring>

namespace netplay::bridge::takeover
{
namespace
{
struct HelperProfile
{
    const char* versionTag;
    DWORD timestamp;
    DWORD imageSize;
    DWORD quitSlotRva;
    DWORD deathSlotRva;
    DWORD nullHandlerRva;
    DWORD deathHandlerRva;
    bool mingw;
};

constexpr HelperProfile profiles[] = {
    {"1.02e", 0x5EA87633, 0xFD000, 0x9A010, 0x9A030, 0x44330, 0x496C0, false},
    {"1.02f", 0x5F8C58B0, 0xFD000, 0x9A010, 0x9A030, 0x44370, 0x496E0, false},
    {"1.02f-framestepping", 0x5F8C58B0, 0xFD000, 0x9A010, 0x9A030, 0x44370, 0x496E0, false},
    {"1.02g", 0x623F85D8, 0xFE000, 0x9B010, 0x9B030, 0x446A0, 0x49A50, false},
    {"1.02h", 0x6292935E, 0xFE000, 0x9B038, 0x9B058, 0x44630, 0x49C00, false},
    {"1.02i", 0x63BF27B6, 0xFE000, 0x99FD8, 0x99FF8, 0x45780, 0x4AEA0, false},
    {"1.02j", 0x6A3696C8, 0x294000, 0x1ECE30, 0x1ECE50, 0x82490, 0x94AA0, true},
};

bool ValidateHelperImage(unsigned char* base, const HelperProfile& profile)
{
    __try
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER)
            || dos->e_lfanew > 0x1000)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386
            || nt->FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER32)
            || nt->FileHeader.TimeDateStamp != profile.timestamp
            || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC
            || nt->OptionalHeader.ImageBase != 0x400000
            || nt->OptionalHeader.SizeOfImage != profile.imageSize)
            return false;
        const auto original = reinterpret_cast<uintptr_t>(base + profile.nullHandlerRva);
        const auto replacement = reinterpret_cast<uintptr_t>(base + profile.deathHandlerRva);
        const auto current = *reinterpret_cast<const uintptr_t*>(base + profile.quitSlotRva);
        if ((current != original && current != replacement)
            || *reinterpret_cast<const uintptr_t*>(base + profile.deathSlotRva) != replacement)
            return false;
        const unsigned char* nullHandler = base + profile.nullHandlerRva;
        const unsigned char* deathHandler = base + profile.deathHandlerRva;
        return nullHandler[0] == 0xC2 && nullHandler[1] == 0x08 && nullHandler[2] == 0x00
            && deathHandler[0] == 0x55
            && deathHandler[1] == (profile.mingw ? 0x89 : 0x8B)
            && deathHandler[2] == (profile.mingw ? 0xE5 : 0xEC);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
}

bool InstallHelperDelayDisconnectHandler(HMODULE helperModule, const char* versionTag)
{
    static_assert(sizeof(void*) == 4, "Revival helpers use 32-bit vtables");
    if (helperModule == nullptr || versionTag == nullptr) return false;
    const HelperProfile* profile = nullptr;
    for (const auto& candidate : profiles)
    {
        if (std::strcmp(candidate.versionTag, versionTag) == 0)
        {
            profile = &candidate;
            break;
        }
    }
    auto* base = reinterpret_cast<unsigned char*>(helperModule);
    if (profile == nullptr || !ValidateHelperImage(base, *profile)) return false;
    auto* slot = reinterpret_cast<PVOID volatile*>(base + profile->quitSlotRva);
    PVOID original = base + profile->nullHandlerRva;
    PVOID replacement = base + profile->deathHandlerRva;
    if (*slot == replacement) return true;
    DWORD previous = 0;
    if (!VirtualProtect(base + profile->quitSlotRva, sizeof(PVOID), PAGE_READWRITE, &previous)) return false;
    // Both native callbacks use __thiscall(state, manager, endpoint), with callee cleanup.
    const PVOID observed = InterlockedCompareExchangePointer(slot, replacement, original);
    DWORD unused = 0;
    const bool restored = VirtualProtect(base + profile->quitSlotRva, sizeof(PVOID), previous, &unused) != FALSE;
    return restored && (observed == original || observed == replacement);
}
}
