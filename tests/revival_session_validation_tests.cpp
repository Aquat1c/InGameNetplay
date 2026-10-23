#include "netplay/bridge/takeover_internal.h"

#include <cstdio>
#include <cstdlib>

namespace netplay::bridge::takeover {
const RevivalAddressProfile* g_activeRevival = &kRevival_1_02h;
bool SafeReadPtr(const void* address, uintptr_t* value)
{
    __try { *value = *static_cast<const uintptr_t*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SafeReadInt(const void* address, int* value)
{
    __try { *value = *static_cast<const int*>(address); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

namespace {
void Expect(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}

int main()
{
    using namespace netplay::bridge::takeover;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    auto* memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, 2 * info.dwPageSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Expect(memory != nullptr, "session fixture allocation");
    DWORD old;
    Expect(VirtualProtect(memory + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old) != FALSE,
           "session fixture guard page");
    const uintptr_t base = 0x10000000u, end = base + 0x200000u;
    const std::size_t localSizes[] = {0x2B0, 0x310};
    for (const auto* profile : kAllRevivalProfiles)
    {
        g_activeRevival = profile;
        const uintptr_t localVtables[] = {profile->practiceSessionVtableRva,
                                        profile->tournamentSessionVtableRva};
        for (int i = 0; i < 2; ++i)
        {
            const auto object = reinterpret_cast<uintptr_t>(memory + info.dwPageSize - localSizes[i]);
            *reinterpret_cast<uintptr_t*>(object) = base + localVtables[i];
            Expect(IsLikelySessionPointer(object, base, end),
                   "known local session validates without online-only fields");
        }
    }
    g_activeRevival = &kRevival_1_02h;
    const auto localSession = reinterpret_cast<uintptr_t>(memory + info.dwPageSize - 0x2B0);
    Expect(!IsLikelySessionPointer(0x100u, base, end), "low pointer rejected");
    Expect(!IsLikelySessionPointer(reinterpret_cast<uintptr_t>(memory + info.dwPageSize), base, end),
           "unreadable pointer rejected");
    const uintptr_t invalidVtables[] = {base - 1, end, base + 0x100};
    for (auto table : invalidVtables)
    {
        *reinterpret_cast<uintptr_t*>(localSession) = table;
        Expect(!IsLikelySessionPointer(localSession, base, end), "invalid local session identity rejected");
    }
    const auto online = reinterpret_cast<uintptr_t>(memory);
    *reinterpret_cast<uintptr_t*>(online) = base + kRevival_1_02h.onlineSessionVtableRva;
    *reinterpret_cast<int*>(online + kRevival_1_02h.sessionOffsetInputDelay) = 2;
    *reinterpret_cast<int*>(online + kRevival_1_02h.sessionOffsetPingMs) = 25;
    Expect(IsLikelySessionPointer(online, base, end), "valid online fields retain prior acceptance");
    *reinterpret_cast<int*>(online + kRevival_1_02h.sessionOffsetInputDelay) = -1;
    *reinterpret_cast<int*>(online + kRevival_1_02h.sessionOffsetPingMs) = -1;
    Expect(!IsLikelySessionPointer(online, base, end), "invalid online fields retain prior rejection");
    g_activeRevival = &kRevival_1_02j;
    *reinterpret_cast<uintptr_t*>(localSession) = base + g_activeRevival->onlineSessionVtableRva;
    Expect(IsLikelySessionPointer(localSession, base, end), "1.02j exact online vtable remains sufficient");
    *reinterpret_cast<uintptr_t*>(localSession) = base + 0x100;
    Expect(!IsLikelySessionPointer(localSession, base, end), "1.02j unknown vtable rejected");
    VirtualFree(memory, 0, MEM_RELEASE);
    std::puts("session validation tests passed");
}
