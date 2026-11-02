#pragma once

#include <cstdint>
#include <vector>

namespace netplay::patch
{
struct PatchRecord
{
    uintptr_t address;
    std::vector<uint8_t> originalBytes;
};

bool WriteProcessMemoryLocal(uintptr_t address, const uint8_t* data, size_t size);
bool ApplyPatch(
    uintptr_t address,
    const std::vector<uint8_t>& expectedBytes,
    const std::vector<uint8_t>& patchedBytes,
    const char* label,
    std::vector<PatchRecord>* patchRecords);
void RestorePatches(std::vector<PatchRecord>* patchRecords);
}

