#include "netplay/hooks/internal/shared.h"

namespace netplay::hooks::internal
{
bool ApplyPatch(
    uintptr_t address,
    const std::vector<uint8_t>& expectedBytes,
    const std::vector<uint8_t>& patchedBytes,
    const char* label)
{
    return netplay::patch::ApplyPatch(address, expectedBytes, patchedBytes, label, &g_appliedPatches);
}

void RestorePatches()
{
    netplay::patch::RestorePatches(&g_appliedPatches);
}
} // namespace netplay::hooks::internal

