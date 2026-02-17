#include "netplay/core/patch_utils.h"

#include "logger.h"

#include <cstring>
#include <windows.h>

namespace netplay::patch
{
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
    const char* label,
    std::vector<PatchRecord>* patchRecords)
{
    if (patchRecords == nullptr)
    {
        return false;
    }

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

    patchRecords->push_back(PatchRecord{address, std::move(currentBytes)});
    mod::Log("Patched '%s' at 0x%08X", label, static_cast<unsigned>(address));
    return true;
}

void RestorePatches(std::vector<PatchRecord>* patchRecords)
{
    if (patchRecords == nullptr)
    {
        return;
    }

    for (auto it = patchRecords->rbegin(); it != patchRecords->rend(); ++it)
    {
        (void)WriteProcessMemoryLocal(it->address, it->originalBytes.data(), it->originalBytes.size());
    }
    patchRecords->clear();
}
}


