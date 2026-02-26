// Revival DLL memory introspection and session field manipulation.

#include "netplay/bridge/takeover_internal.h"

#include <cstring>
#include <cwchar>

#include <windows.h>

namespace netplay::bridge::takeover
{

bool IsReadableRange(const void* address, size_t size)
{
    if (address == nullptr || size == 0)
    {
        return false;
    }

    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = cursor + size;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) == 0)
        {
            return false;
        }

        if (mbi.State != MEM_COMMIT)
        {
            return false;
        }

        const DWORD rawProt = mbi.Protect;
        if ((rawProt & PAGE_GUARD) != 0 || (rawProt & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        const DWORD prot = rawProt & 0xFFu;
        const bool readable =
            prot == PAGE_READONLY ||
            prot == PAGE_READWRITE ||
            prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_READ ||
            prot == PAGE_EXECUTE_READWRITE ||
            prot == PAGE_EXECUTE_WRITECOPY;
        if (!readable)
        {
            return false;
        }

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor)
        {
            return false;
        }
        cursor = regionEnd;
    }

    return true;
}

bool IsWritableRange(void* address, size_t size)
{
    if (address == nullptr || size == 0)
    {
        return false;
    }

    uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = cursor + size;
    while (cursor < end)
    {
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &mbi, sizeof(mbi)) == 0)
        {
            return false;
        }

        if (mbi.State != MEM_COMMIT)
        {
            return false;
        }

        const DWORD rawProt = mbi.Protect;
        if ((rawProt & PAGE_GUARD) != 0 || (rawProt & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        const DWORD prot = rawProt & 0xFFu;
        const bool writable =
            prot == PAGE_READWRITE ||
            prot == PAGE_WRITECOPY ||
            prot == PAGE_EXECUTE_READWRITE ||
            prot == PAGE_EXECUTE_WRITECOPY;
        if (!writable)
        {
            return false;
        }

        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (regionEnd <= cursor)
        {
            return false;
        }
        cursor = regionEnd;
    }

    return true;
}

bool SafeReadInt(const void* address, int* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(int)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const int*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

bool SafeReadPtr(const void* address, uintptr_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uintptr_t)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const uintptr_t*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

bool SafeReadByte(const void* address, uint8_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uint8_t)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const uint8_t*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

bool ReadModuleImageRange(HMODULE module, uintptr_t* outBase, uintptr_t* outEnd)
{
    if (module == nullptr || outBase == nullptr || outEnd == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    IMAGE_DOS_HEADER dos = {};
    if (!IsReadableRange(reinterpret_cast<const void*>(base), sizeof(dos)))
    {
        return false;
    }

    __try
    {
        dos = *reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
    {
        return false;
    }

    const uintptr_t ntAddress = base + static_cast<uintptr_t>(dos.e_lfanew);
    IMAGE_NT_HEADERS32 nt = {};
    if (!IsReadableRange(reinterpret_cast<const void*>(ntAddress), sizeof(nt)))
    {
        return false;
    }

    __try
    {
        nt = *reinterpret_cast<const IMAGE_NT_HEADERS32*>(ntAddress);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }

    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt.OptionalHeader.SizeOfImage == 0)
    {
        return false;
    }

    const uintptr_t end = base + static_cast<uintptr_t>(nt.OptionalHeader.SizeOfImage);
    if (end <= base)
    {
        return false;
    }

    *outBase = base;
    *outEnd = end;
    return true;
}

int ReadRoleFlagFromRevival()
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return -1;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    for (size_t i = 0; i < g_activeRevival->roleFlagOffsetCount; ++i)
    {
        const uintptr_t offset = g_activeRevival->roleFlagOffsets[i];
        int role = -1;
        if (SafeReadInt(reinterpret_cast<const void*>(base + offset), &role)
            && role >= 0 && role <= 3)
        {
            return role;
        }
    }
    return -1;
}

bool IsSessionPointerByVtable(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd)
{
    // Reject null / low-memory sentinels that can appear at fallback offsets.
    if (sessionPtr < 0x00100000u)
    {
        return false;
    }

    // Session objects are C++ classes; first word must be a vtable inside EfzRevival.dll.
    uintptr_t vtable = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable))
    {
        return false;
    }
    if (revivalImageBase == 0 || revivalImageEnd <= revivalImageBase || vtable < revivalImageBase || vtable >= revivalImageEnd)
    {
        return false;
    }

    return true;
}

bool IsLikelySessionPointer(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd)
{
    if (!IsSessionPointerByVtable(sessionPtr, revivalImageBase, revivalImageEnd))
    {
        return false;
    }

    int delayFrames = -1;
    int pingMs = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay), &delayFrames))
    {
        return false;
    }
    if (!SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetPingMs), &pingMs))
    {
        return false;
    }

    const bool delayLooksValid = delayFrames >= 0 && delayFrames < 128;
    const bool pingLooksValid = pingMs >= 0 && pingMs < 60000;
    return delayLooksValid || pingLooksValid;
}

uintptr_t ReadSessionPointerFromRevival()
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        g_lastSessionPtrOffset = 0;
        return 0;
    }

    g_lastSessionPtrOffset = 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t revivalImageBase = 0;
    uintptr_t revivalImageEnd = 0;
    if (!ReadModuleImageRange(revival, &revivalImageBase, &revivalImageEnd))
    {
        revivalImageBase = base;
        revivalImageEnd = base + 0x02000000u;
    }

    for (size_t i = 0; i < g_activeRevival->sessionPtrOffsetCount; ++i)
    {
        const uintptr_t offset = g_activeRevival->sessionPtrOffsets[i];
        uintptr_t sessionPtr = 0;
        if (SafeReadPtr(reinterpret_cast<const void*>(base + offset), &sessionPtr)
            && IsLikelySessionPointer(sessionPtr, revivalImageBase, revivalImageEnd))
        {
            g_lastSessionPtrOffset = offset;
            g_lastValidatedSessionPtr = sessionPtr;
            return sessionPtr;
        }
    }
    return 0;
}

uintptr_t ReadSessionPointerFromRevivalLoose()
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return 0;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t revivalImageBase = 0;
    uintptr_t revivalImageEnd = 0;
    if (!ReadModuleImageRange(revival, &revivalImageBase, &revivalImageEnd))
    {
        revivalImageBase = base;
        revivalImageEnd = base + 0x02000000u;
    }

    for (size_t i = 0; i < g_activeRevival->sessionPtrOffsetCount; ++i)
    {
        const uintptr_t offset = g_activeRevival->sessionPtrOffsets[i];
        uintptr_t sessionPtr = 0;
        if (SafeReadPtr(reinterpret_cast<const void*>(base + offset), &sessionPtr)
            && IsSessionPointerByVtable(sessionPtr, revivalImageBase, revivalImageEnd))
        {
            return sessionPtr;
        }
    }

    return 0;
}

uintptr_t ReadSessionPointerForMutation(bool* outUsedCached)
{
    if (outUsedCached != nullptr)
    {
        *outUsedCached = false;
    }

    const uintptr_t strictSessionPtr = ReadSessionPointerFromRevival();
    if (strictSessionPtr != 0)
    {
        return strictSessionPtr;
    }

    const uintptr_t cachedSessionPtr = g_lastValidatedSessionPtr;
    if (cachedSessionPtr == 0)
    {
        return 0;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return 0;
    }

    uintptr_t revivalImageBase = 0;
    uintptr_t revivalImageEnd = 0;
    if (!ReadModuleImageRange(revival, &revivalImageBase, &revivalImageEnd))
    {
        revivalImageBase = reinterpret_cast<uintptr_t>(revival);
        revivalImageEnd = revivalImageBase + 0x02000000u;
    }

    if (!IsSessionPointerByVtable(cachedSessionPtr, revivalImageBase, revivalImageEnd))
    {
        return 0;
    }

    int activePlayer = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(cachedSessionPtr + g_activeRevival->sessionOffsetActivePlayer), &activePlayer))
    {
        return 0;
    }
    if (activePlayer != 0 && activePlayer != 1)
    {
        return 0;
    }

    if (outUsedCached != nullptr)
    {
        *outUsedCached = true;
    }
    return cachedSessionPtr;
}

bool ReadRevivalSyncFlags(RevivalSyncFlags* outFlags)
{
    if (outFlags == nullptr)
    {
        return false;
    }

    *outFlags = RevivalSyncFlags{};
    bool hasAny = false;

    int gameMode = -1;
    if (SafeReadInt(reinterpret_cast<const void*>(g_activeRevival->addrGameModeCurrentIndex), &gameMode))
    {
        outFlags->gameMode = gameMode;
        hasAny = true;
    }

    uintptr_t mode0Struct = 0;
    if (SafeReadPtr(reinterpret_cast<const void*>(g_activeRevival->addrGameModeStructTable), &mode0Struct) && mode0Struct != 0)
    {
        uint8_t mode0Flag = 0;
        if (SafeReadByte(reinterpret_cast<const void*>(mode0Struct + 1084u), &mode0Flag))
        {
            outFlags->mode0Flag1084 = static_cast<int>(mode0Flag);
            hasAny = true;
        }
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival != nullptr)
    {
        uintptr_t globalStatePtr = 0;
        const uintptr_t globalStatePtrAddr = reinterpret_cast<uintptr_t>(revival) + g_activeRevival->globalStatePtrOffset;
        if (SafeReadPtr(reinterpret_cast<const void*>(globalStatePtrAddr), &globalStatePtr) && globalStatePtr != 0)
        {
            uint8_t sessionByte = 0;
            if (SafeReadByte(reinterpret_cast<const void*>(globalStatePtr + g_activeRevival->globalStateOffsetSessionByte), &sessionByte))
            {
                outFlags->sessionByte = static_cast<int>(sessionByte);
                hasAny = true;
            }

            uint8_t globalFlag4964 = 0;
            if (SafeReadByte(reinterpret_cast<const void*>(globalStatePtr + g_activeRevival->globalStateOffsetFlag4964), &globalFlag4964))
            {
                outFlags->globalFlag4964 = static_cast<int>(globalFlag4964);
                hasAny = true;
            }

            uint8_t globalFlag4965 = 0;
            if (SafeReadByte(reinterpret_cast<const void*>(globalStatePtr + g_activeRevival->globalStateOffsetFlag4965), &globalFlag4965))
            {
                outFlags->globalFlag4965 = static_cast<int>(globalFlag4965);
                hasAny = true;
            }
        }
    }

    const bool sessionLooksRollback = outFlags->sessionByte == 0 || outFlags->sessionByte == 1 || outFlags->sessionByte == 2;
    const bool isPlayerSync = (outFlags->gameMode == 3 && outFlags->mode0Flag1084 == 4 && sessionLooksRollback);
    const bool isSpectateSync = (outFlags->gameMode == 8 && outFlags->mode0Flag1084 == 4);
    outFlags->inRollbackSyncState = isPlayerSync || isSpectateSync;
    outFlags->inRollbackActiveState = (outFlags->gameMode == 3 && outFlags->mode0Flag1084 == 4 && outFlags->sessionByte == 2)
        || isSpectateSync;
    return hasAny;
}

void RefreshRuntimeStatus(NetbridgeStatus* ioStatus)
{
    if (ioStatus == nullptr)
    {
        return;
    }

    ioStatus->syncGameMode = -1;
    ioStatus->syncMode0Flag1084 = -1;
    ioStatus->syncSessionByte = -1;
    ioStatus->syncGlobalFlag4964 = -1;
    ioStatus->syncGlobalFlag4965 = -1;
    ioStatus->pingMs = -1;
    ioStatus->rollbackFrames = -1;
    ioStatus->delayPromptSerial = 0;
    ioStatus->delayPromptServedSerial = 0;
    ioStatus->spectateConfirmPromptSerial = 0;
    ioStatus->spectateConfirmPromptServedSerial = 0;
    ioStatus->localInitApplied = g_localInitAppliedForSession ? 1 : 0;
    ioStatus->delaySetupReady = 0;
    ioStatus->vsHumanSyncReady = 0;
    ioStatus->p1Name[0] = '\0';
    ioStatus->p2Name[0] = '\0';

    LONG delayPromptSerial = 0;
    LONG delayPromptServedSerial = 0;
    ReadDelayPromptSignal(&delayPromptSerial, &delayPromptServedSerial);
    ioStatus->delayPromptSerial = static_cast<int>(delayPromptSerial);
    ioStatus->delayPromptServedSerial = static_cast<int>(delayPromptServedSerial);

    LONG spectateConfirmSerial = 0;
    LONG spectateConfirmServedSerial = 0;
    ReadSpectateConfirmPromptSignal(&spectateConfirmSerial, &spectateConfirmServedSerial);
    ioStatus->spectateConfirmPromptSerial = static_cast<int>(spectateConfirmSerial);
    ioStatus->spectateConfirmPromptServedSerial = static_cast<int>(spectateConfirmServedSerial);

    DelayPromptMetrics promptMetrics = g_delayPromptMetrics;
    if (g_hostBlock != nullptr)
    {
        const LONG metricsSerial = InterlockedCompareExchange(&g_hostBlock->delayMetricsSerial, 0, 0);
        if (metricsSerial > 0)
        {
            promptMetrics.serial = static_cast<int>(metricsSerial);
            promptMetrics.averagePingMs = g_hostBlock->delayAveragePingMs;
            promptMetrics.minPingMs = g_hostBlock->delayMinPingMs;
            promptMetrics.maxPingMs = g_hostBlock->delayMaxPingMs;
            promptMetrics.recommendedDelay = g_hostBlock->delayRecommended;
            promptMetrics.minDelay = g_hostBlock->delayRangeMin;
            promptMetrics.maxDelay = g_hostBlock->delayRangeMax;
        }
        const LONG inputSerial = InterlockedCompareExchange(&g_hostBlock->delayInputSerial, 0, 0);
        promptMetrics.inputSerial = static_cast<int>(inputSerial);
        promptMetrics.inputValue = g_hostBlock->delayInputValue;
    }
    g_delayPromptMetrics = promptMetrics;

    RevivalSyncFlags syncFlags = {};
    if (ReadRevivalSyncFlags(&syncFlags))
    {
        ioStatus->syncGameMode = syncFlags.gameMode;
        ioStatus->syncMode0Flag1084 = syncFlags.mode0Flag1084;
        ioStatus->syncSessionByte = syncFlags.sessionByte;
        ioStatus->syncGlobalFlag4964 = syncFlags.globalFlag4964;
        ioStatus->syncGlobalFlag4965 = syncFlags.globalFlag4965;
        ioStatus->vsHumanSyncReady = syncFlags.inRollbackSyncState ? 1 : 0;
    }

    int roleFlag = ReadRoleFlagFromRevival();
    if (roleFlag < 0)
    {
        roleFlag = g_localRoleFlag;
    }
    ioStatus->roleFlag = roleFlag;

    const uintptr_t sessionPtr = ReadSessionPointerFromRevival();
    if (sessionPtr == 0)
    {
        if (promptMetrics.averagePingMs >= 0)
        {
            ioStatus->pingMs = promptMetrics.averagePingMs;
        }
        if (promptMetrics.recommendedDelay >= 0)
        {
            ioStatus->rollbackFrames = promptMetrics.recommendedDelay;
        }
        ioStatus->delaySetupReady =
            (ioStatus->delayPromptSerial > 0 || ioStatus->delayPromptServedSerial > 0)
                ? 1
                : 0;
        return;
    }

    int delayFrames = -1;
    int pingMs = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay), &delayFrames);
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetPingMs), &pingMs);

    if (delayFrames >= 0 && delayFrames < 128)
    {
        ioStatus->rollbackFrames = delayFrames;
    }
    if (pingMs >= 0 && pingMs < 60000)
    {
        ioStatus->pingMs = pingMs;
    }
    else if (promptMetrics.averagePingMs >= 0)
    {
        ioStatus->pingMs = promptMetrics.averagePingMs;
    }

    if (ioStatus->rollbackFrames < 0 && promptMetrics.recommendedDelay >= 0)
    {
        ioStatus->rollbackFrames = promptMetrics.recommendedDelay;
    }

    auto tryReadInlineName = [](uintptr_t baseAddress, char* outText, size_t outSize) -> void {
        if (outText == nullptr || outSize == 0)
        {
            return;
        }
        outText[0] = '\0';

        constexpr size_t kMaxChars = 24;
        wchar_t wide[kMaxChars + 1] = {};
        if (!IsReadableRange(reinterpret_cast<const void*>(baseAddress), kMaxChars * sizeof(wchar_t)))
        {
            return;
        }

        __try
        {
            std::memcpy(wide, reinterpret_cast<const void*>(baseAddress), kMaxChars * sizeof(wchar_t));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        wide[kMaxChars] = L'\0';

        size_t n = 0;
        while (n < kMaxChars && wide[n] != L'\0')
        {
            if (wide[n] < 0x20)
            {
                return;
            }
            ++n;
        }
        if (n == 0 || n >= kMaxChars)
        {
            return;
        }

        const int converted = WideCharToMultiByte(CP_ACP, 0, wide, static_cast<int>(n), outText, static_cast<int>(outSize - 1), nullptr, nullptr);
        if (converted > 0)
        {
            outText[converted] = '\0';
        }
    };

    auto sanitizeInlineName = [](char* text, size_t textSize) -> void {
        if (text == nullptr || textSize == 0 || text[0] == '\0')
        {
            return;
        }

        size_t len = 0;
        int questionMarks = 0;
        for (; len + 1 < textSize && text[len] != '\0'; ++len)
        {
            const unsigned char c = static_cast<unsigned char>(text[len]);
            if (c < 0x20 || c == 0x7F)
            {
                text[0] = '\0';
                return;
            }
            if (c == '?')
            {
                ++questionMarks;
            }
        }
        if (len == 0 || len + 1 >= textSize)
        {
            text[0] = '\0';
            return;
        }

        // Reject mojibake-like names (mostly '?' from failed conversion).
        if (questionMarks >= 3 && questionMarks * 2 >= static_cast<int>(len))
        {
            text[0] = '\0';
        }
    };

    const bool allowSessionInlineNames =
        syncFlags.inRollbackSyncState
        || syncFlags.inRollbackActiveState
        || static_cast<NetbridgePhase>(ioStatus->phase) == NetbridgePhase::Connected;

    if (allowSessionInlineNames)
    {
        tryReadInlineName(sessionPtr + 740u, ioStatus->p1Name, sizeof(ioStatus->p1Name));
        tryReadInlineName(sessionPtr + 764u, ioStatus->p2Name, sizeof(ioStatus->p2Name));
        sanitizeInlineName(ioStatus->p1Name, sizeof(ioStatus->p1Name));
        sanitizeInlineName(ioStatus->p2Name, sizeof(ioStatus->p2Name));
    }

    // Only expose player names when we have both sides; showing a single local
    // nickname during delay setup looks misleading.
    if (ioStatus->p1Name[0] == '\0' || ioStatus->p2Name[0] == '\0')
    {
        ioStatus->p1Name[0] = '\0';
        ioStatus->p2Name[0] = '\0';
    }

    ioStatus->delaySetupReady =
        (ioStatus->delayPromptSerial > 0 || ioStatus->delayPromptServedSerial > 0)
            ? 1
            : 0;
}

bool SetLocalRoleFlag(int roleFlag, const char* reason)
{
    if (g_localInitFn == nullptr)
    {
        return false;
    }

    if (roleFlag < 0 || roleFlag > 3)
    {
        return false;
    }

    if (g_localRoleFlag == roleFlag)
    {
        return true;
    }

    int localParams[2] = {roleFlag, 102};
    const int result = g_localInitFn(localParams);
    g_localRoleFlag = roleFlag;
    mod::Log("Takeover: local role switch mode=%d result=%d reason=%s", roleFlag, result, reason != nullptr ? reason : "");
    return true;
}

bool SetRoleFlagDirect(int roleFlag, const char* reason)
{
    if (roleFlag < 0 || roleFlag > 3)
    {
        return false;
    }

    if (g_localRoleFlag == roleFlag)
    {
        return true;
    }

    // Write roleFlag directly to every Revival DLL global location
    // WITHOUT calling init(). This changes how other mods (training
    // mode, rich presence) see the current mode without creating a
    // new session object or applying session-specific EXE patches.
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("Takeover: SetRoleFlagDirect failed (Revival DLL not loaded) reason=%s",
            reason != nullptr ? reason : "");
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    int written = 0;
    for (size_t i = 0; i < g_activeRevival->roleFlagOffsetCount; ++i)
    {
        const uintptr_t offset = g_activeRevival->roleFlagOffsets[i];
        if (offset == 0)
        {
            continue;
        }
        int* const ptr = reinterpret_cast<int*>(base + offset);
        if (IsWritableRange(ptr, sizeof(int)))
        {
            *ptr = roleFlag;
            ++written;
        }
    }

    const int oldRole = g_localRoleFlag;
    g_localRoleFlag = roleFlag;
    mod::Log("Takeover: direct role flag %d -> %d (wrote %d globals) reason=%s",
        oldRole, roleFlag, written, reason != nullptr ? reason : "");
    return written > 0;
}

// ---------------------------------------------------------------------------
// NeutralizeTournamentAutoNav — zero all entries in the tournament session's
// auto-navigation input queue.
//
// After init(3,102), the tournament constructor populates a deque with 22
// byte-pair entries that simulate controller presses to auto-navigate from
// the title screen through character select and into a fight.  This is
// designed for Concerto's unattended bracket flow.
//
// For our interactive use we zero every entry (making them idle/no-input)
// while keeping the queue non-empty (element count stays at 22).  This
// prevents phantom button presses during character select AND avoids the
// mode-0 ExitProcess trigger that fires when the queue is empty.
//
// The queue is naturally consumed over 22 frames as idle inputs.  When the
// match ends and returns to mode 0, the queue is empty → ExitProcess → our
// IAT hook intercepts and routes to the netplay menu.
//
// MSVC deque<uint16_t> layout (byte offsets from deque start):
//   +0  proxy / allocator
//   +4  pointer to block-pointer array
//   +8  block slot count (power of 2)
//   +12 start offset
//   +16 element count
//
// Each block holds 8 elements of 2 bytes (16 bytes per block).
// Block index = (abs_index >> 3) & (block_count - 1).
// ---------------------------------------------------------------------------
bool NeutralizeTournamentAutoNav()
{
    const uintptr_t sessionPtr = ReadSessionPointerFromRevivalLoose();
    if (sessionPtr == 0)
    {
        mod::Log("NeutralizeTournamentAutoNav: no session pointer");
        return false;
    }

    if (g_activeRevival == nullptr || g_activeRevival->tournamentInputQueueOffset == 0)
    {
        mod::Log("NeutralizeTournamentAutoNav: no queue offset in profile");
        return false;
    }

    const uintptr_t dequeAddr = sessionPtr + g_activeRevival->tournamentInputQueueOffset;

    // Read deque metadata.
    uintptr_t blockArrayPtr = 0;
    int blockCount = 0;
    int elementCount = 0;

    if (!SafeReadPtr(reinterpret_cast<const void*>(dequeAddr + 4), &blockArrayPtr) || blockArrayPtr == 0)
    {
        mod::Log("NeutralizeTournamentAutoNav: failed to read block-array pointer");
        return false;
    }
    if (!SafeReadInt(reinterpret_cast<const void*>(dequeAddr + 8), &blockCount) || blockCount <= 0)
    {
        mod::Log("NeutralizeTournamentAutoNav: failed to read block count");
        return false;
    }
    if (!SafeReadInt(reinterpret_cast<const void*>(dequeAddr + 16), &elementCount) || elementCount <= 0)
    {
        mod::Log("NeutralizeTournamentAutoNav: failed to read element count (got %d)", elementCount);
        return false;
    }

    // Number of blocks that contain data.
    const int usedBlocks = (elementCount + 7) / 8;
    int zeroed = 0;

    for (int i = 0; i < usedBlocks && i < blockCount; ++i)
    {
        uintptr_t blockPtr = 0;
        if (SafeReadPtr(reinterpret_cast<const void*>(blockArrayPtr + 4 * static_cast<uintptr_t>(i)), &blockPtr)
            && blockPtr != 0)
        {
            // Zero all 8 element slots (16 bytes) in this block, making
            // every entry the idle byte pair (0, 0).
            std::memset(reinterpret_cast<void*>(blockPtr), 0, 16);
            ++zeroed;
        }
    }

    mod::Log(
        "NeutralizeTournamentAutoNav: zeroed %d/%d blocks, %d elements remain (idle)",
        zeroed, usedBlocks, elementCount);
    return zeroed > 0;
}

// ---------------------------------------------------------------------------
// Tournament EXE patch save / restore.
//
// The tournament constructor patches 4 locations in the EFZ executable:
//   0x763F04 (7 bytes) — inline hook → sub_1006E260
//   0x763E50 (7 bytes) — inline hook → sub_1006E260
//   0x754C1A (1 byte)  — byte set to 0
//   0x7599ED (20 bytes) — NOP pad
//
// In Concerto the process exits after every match, so these are never
// reverted.  For our in-process mode switching we save the original bytes
// before init(3,102) and restore them after intercepting ExitProcess.
// ---------------------------------------------------------------------------

static uint8_t g_savedTournamentPatches[kMaxTournamentExePatches][kMaxTournamentExePatchBytes];
static bool g_tournamentPatchesSaved = false;

bool SaveTournamentExePatches()
{
    if (g_activeRevival == nullptr || g_activeRevival->tournamentExePatchCount == 0)
    {
        return false;
    }

    for (size_t i = 0; i < g_activeRevival->tournamentExePatchCount; ++i)
    {
        const uintptr_t addr = g_activeRevival->tournamentExePatchAddr[i];
        const size_t size = g_activeRevival->tournamentExePatchSize[i];
        if (size == 0 || size > kMaxTournamentExePatchBytes)
        {
            continue;
        }
        std::memcpy(g_savedTournamentPatches[i],
                    reinterpret_cast<const void*>(addr), size);
    }

    g_tournamentPatchesSaved = true;
    mod::Log("SaveTournamentExePatches: saved %zu patch regions",
             g_activeRevival->tournamentExePatchCount);
    return true;
}

bool RestoreTournamentExePatches()
{
    if (!g_tournamentPatchesSaved || g_activeRevival == nullptr)
    {
        mod::Log("RestoreTournamentExePatches: nothing to restore");
        return false;
    }

    int restored = 0;
    for (size_t i = 0; i < g_activeRevival->tournamentExePatchCount; ++i)
    {
        const uintptr_t addr = g_activeRevival->tournamentExePatchAddr[i];
        const size_t size = g_activeRevival->tournamentExePatchSize[i];
        if (size == 0 || size > kMaxTournamentExePatchBytes)
        {
            continue;
        }

        DWORD oldProtect = 0;
        if (VirtualProtect(reinterpret_cast<void*>(addr), size,
                           PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            std::memcpy(reinterpret_cast<void*>(addr),
                        g_savedTournamentPatches[i], size);
            VirtualProtect(reinterpret_cast<void*>(addr), size,
                           oldProtect, &oldProtect);
            ++restored;
        }
    }

    g_tournamentPatchesSaved = false;
    mod::Log("RestoreTournamentExePatches: restored %d/%zu patches",
             restored, g_activeRevival->tournamentExePatchCount);
    return restored > 0;
}

// ---------------------------------------------------------------------------
// DLL ExitProcess call-site patches
//
// The Revival DLL's tournament tick calls ExitProcess(0) when the game
// mode returns to 0 (title screen).  Each call is guarded by a Jcc
// instruction (jz or jnz).  By patching the Jcc byte to 0xEB (jmp short),
// the conditional becomes unconditional, making ExitProcess unreachable.
//
// These patches are applied before init(3,102) and restored after the
// tournament session is cleaned up.
// ---------------------------------------------------------------------------

static uint8_t g_savedDllExitProcessBytes[RevivalAddressProfile::kMaxExitProcessPatches];
static bool g_dllExitProcessPatchesSaved = false;

bool SaveAndApplyDllExitProcessPatches()
{
    if (g_dllExitProcessPatchesSaved)
    {
        return true; // Already applied — don't overwrite saved originals.
    }
    if (g_activeRevival == nullptr || g_activeRevival->exitProcessPatchCount == 0)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    int applied = 0;

    for (size_t i = 0; i < g_activeRevival->exitProcessPatchCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessPatchRva[i];
        if (rva == 0)
        {
            continue;
        }

        auto* ptr = reinterpret_cast<uint8_t*>(base + rva);

        // Save original byte.
        g_savedDllExitProcessBytes[i] = *ptr;

        // Verify the original byte matches the expected Jcc opcode.
        const uint8_t expected = g_activeRevival->exitProcessPatchOriginal[i];
        if (*ptr != expected)
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: site %zu at RVA 0x%lX: "
                     "expected 0x%02X, found 0x%02X — skipping",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(expected),
                     static_cast<unsigned>(*ptr));
            continue;
        }

        DWORD oldProtect = 0;
        if (VirtualProtect(ptr, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *ptr = 0xEB; // jmp short (unconditional)
            VirtualProtect(ptr, 1, oldProtect, &oldProtect);
            ++applied;
        }
    }

    g_dllExitProcessPatchesSaved = true;
    mod::Log("SaveAndApplyDllExitProcessPatches: applied %d/%zu patches",
             applied, g_activeRevival->exitProcessPatchCount);
    return applied > 0;
}

bool RestoreDllExitProcessPatches()
{
    if (!g_dllExitProcessPatchesSaved || g_activeRevival == nullptr)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    int restored = 0;

    for (size_t i = 0; i < g_activeRevival->exitProcessPatchCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessPatchRva[i];
        if (rva == 0)
        {
            continue;
        }

        auto* ptr = reinterpret_cast<uint8_t*>(base + rva);

        DWORD oldProtect = 0;
        if (VirtualProtect(ptr, 1, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            *ptr = g_savedDllExitProcessBytes[i];
            VirtualProtect(ptr, 1, oldProtect, &oldProtect);
            ++restored;
        }
    }

    g_dllExitProcessPatchesSaved = false;
    mod::Log("RestoreDllExitProcessPatches: restored %d/%zu patches",
             restored, g_activeRevival->exitProcessPatchCount);
    return restored > 0;
}

// ---------------------------------------------------------------------------
// ForceLocalPlayInit — unconditionally call init(2,102) to create a fresh
// local play session, then invoke the session's vtable[1] init method to
// fully initialize it (audio, BGM, etc.) before any other hooks dispatch
// to the new session.
//
// Why vtable[1]?  During normal startup, init() creates the session and
// installs the frame hook (sub_1006E590) at 0x401582.  The frame hook
// calls vtable[1] every tick, which initializes fields like offset 668
// (BGM audio pointer).  But at runtime the hook was already installed from
// the previous init() call, and other inline hooks (sub_1006E290 etc.)
// dispatch to the session via dword_100A02CC.  If those fire before the
// frame hook's next vtable[1] call, they hit uninitialized fields → crash.
//
// The DLL's own mode-change detector (sub_1006D810) avoids this by
// calling vtable[1] immediately after storing the session pointer.  We do
// the same here.
// ---------------------------------------------------------------------------
bool ForceLocalPlayInit()
{
    if (g_localInitFn == nullptr)
    {
        mod::Log("ForceLocalPlayInit: init function not available");
        return false;
    }

    int localParams[2] = {kLocalRoleLocalPlay, 102};
    const int result = g_localInitFn(localParams);
    g_localRoleFlag = kLocalRoleLocalPlay;
    mod::Log("ForceLocalPlayInit: init(2,102) result=%d", result);

    // --- Immediately call vtable[1] on the new session -----------------------
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival != nullptr
        && g_activeRevival != nullptr
        && g_activeRevival->sessionPtrOffsetCount > 0)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
        const uintptr_t sessionGlobalAddr =
            base + g_activeRevival->sessionPtrOffsets[0];
        uintptr_t sessionPtr = 0;
        if (SafeReadPtr(reinterpret_cast<const void*>(sessionGlobalAddr),
                        &sessionPtr)
            && sessionPtr != 0)
        {
            uintptr_t vtablePtr = 0;
            if (SafeReadPtr(reinterpret_cast<const void*>(sessionPtr),
                            &vtablePtr)
                && vtablePtr != 0)
            {
                uintptr_t vtableSlot1 = 0;
                if (SafeReadPtr(
                        reinterpret_cast<const void*>(
                            vtablePtr + sizeof(uintptr_t)),
                        &vtableSlot1)
                    && vtableSlot1 != 0)
                {
                    // __thiscall: this in ECX, no extra args.
                    typedef void(__thiscall* SessionInitFn)(void*);
                    auto initFn =
                        reinterpret_cast<SessionInitFn>(vtableSlot1);
                    initFn(reinterpret_cast<void*>(sessionPtr));
                    mod::Log(
                        "ForceLocalPlayInit: vtable[1] 0x%08lX called on "
                        "session 0x%08lX",
                        static_cast<unsigned long>(vtableSlot1),
                        static_cast<unsigned long>(sessionPtr));
                }
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// SaveRenderContext / RestoreRenderContext — saves the EfzRender* pointer
// (dword_100A0778) before entering tournament mode, and writes it back when
// needed.  Tournament cleanup code (sub_1006CC30) zeros this global before
// calling ExitProcess, and init(2,102) can zero it again.  The EfzRender
// object itself lives in EFZ.exe and its pointer never changes, so
// restoring the saved value is always safe.
// ---------------------------------------------------------------------------
static uintptr_t g_savedRenderContext = 0;
static bool      g_renderContextSaved = false;

bool SaveRenderContext()
{
    if (g_activeRevival == nullptr
        || g_activeRevival->renderContextGlobalOffset == 0)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t addr = base + g_activeRevival->renderContextGlobalOffset;
    uintptr_t value = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(addr), &value)
        || value == 0)
    {
        mod::Log("SaveRenderContext: dword_100A0778 is NULL or unreadable");
        return false;
    }

    g_savedRenderContext = value;
    g_renderContextSaved = true;
    mod::Log("SaveRenderContext: saved EfzRender* 0x%08lX from offset 0x%lX",
             static_cast<unsigned long>(value),
             static_cast<unsigned long>(g_activeRevival->renderContextGlobalOffset));
    return true;
}

bool RestoreRenderContext()
{
    if (!g_renderContextSaved || g_savedRenderContext == 0)
    {
        return false;
    }

    if (g_activeRevival == nullptr
        || g_activeRevival->renderContextGlobalOffset == 0)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    auto* ptr = reinterpret_cast<uintptr_t*>(
        base + g_activeRevival->renderContextGlobalOffset);
    *ptr = g_savedRenderContext;
    mod::Log("RestoreRenderContext: restored EfzRender* 0x%08lX",
             static_cast<unsigned long>(g_savedRenderContext));
    return true;
}

// ---------------------------------------------------------------------------
// ClearRevivalText — clear the Revival text overlay buffer.
//
// The EfzRender object lives in EFZ.exe memory. Revival's wrapper function
// EFZ_Render_ClearText (RVA clearTextRva = 0x6C070) reads the global
// dword_100A0778 and issues a direct IAT call to EfzRender::clearTextRender.
//
// Tournament cleanup code (sub_1006CC30) zeroes dword_100A0778 before calling
// ExitProcess, and init(2,102) may zero it again.  We restore it from the
// saved value each time before calling clearTextRender so it always sees a
// valid pointer.
//
// The call is wrapped in SEH as a safety net — if the EfzRender object is
// in a bad state, we log and return false instead of crashing.
// ---------------------------------------------------------------------------
bool ClearRevivalText()
{
    if (g_activeRevival == nullptr
        || g_activeRevival->clearTextRva == 0)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);

    // Restore the EfzRender* global if it was zeroed by cleanup code.
    if (!RestoreRenderContext())
    {
        mod::Log("ClearRevivalText: failed to restore render context");
        return false;
    }

    const uintptr_t fnAddr = base + g_activeRevival->clearTextRva;

    __try
    {
        typedef void(__cdecl* ClearTextFn)();
        auto clearFn = reinterpret_cast<ClearTextFn>(fnAddr);
        clearFn();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log("ClearRevivalText: SEH exception 0x%08lX at 0x%08lX",
                 static_cast<unsigned long>(GetExceptionCode()),
                 static_cast<unsigned long>(fnAddr));
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// DisableRevivalTextRendering — disable the EFZ.exe text overlay.
//
// This mirrors the logic that the character-select mode transition
// (sub_10078600) uses to hide tournament nicknames / win counts:
//
//     EFZ_Render_SetTextEnabled(&dword_100A0760, false);
//
// The DLL wrapper at setTextEnabledRva (sub_1006C030) is __thiscall:
//     void __thiscall SetTextEnabled(void* contextBase, bool enable)
// where contextBase is &dword_100A0760 (renderContextGlobalOffset - 0x18).
// It internally calls EfzRender::setRenderText(contextBase[6], enable)
// via the IAT, so dword_100A0778 must be valid (call RestoreRenderContext
// first).
// ---------------------------------------------------------------------------
bool DisableRevivalTextRendering()
{
    if (g_activeRevival == nullptr
        || g_activeRevival->setTextEnabledRva == 0
        || g_activeRevival->renderContextGlobalOffset < 0x18)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    // Ensure the EfzRender* global is valid (init(2,102) may have zeroed it).
    if (!RestoreRenderContext())
    {
        mod::Log("DisableRevivalTextRendering: failed to restore render context");
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t fnAddr = base + g_activeRevival->setTextEnabledRva;
    void* contextBase = reinterpret_cast<void*>(
        base + g_activeRevival->renderContextGlobalOffset - 0x18);

    __try
    {
        // __thiscall: this in ECX, bool arg on stack.
        // Use __fastcall with a dummy EDX parameter.
        typedef void(__fastcall* SetTextEnabledFn)(void* thisPtr, void* edx, int enable);
        auto setFn = reinterpret_cast<SetTextEnabledFn>(fnAddr);
        setFn(contextBase, nullptr, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log("DisableRevivalTextRendering: SEH exception 0x%08lX at 0x%08lX",
                 static_cast<unsigned long>(GetExceptionCode()),
                 static_cast<unsigned long>(fnAddr));
        return false;
    }

    mod::Log("DisableRevivalTextRendering: text rendering disabled");
    return true;
}

void ResetDebugCounters(SharedBlock* block)
{
    if (block == nullptr)
    {
        return;
    }
    block->delayPromptSerial = 0;
    block->delayPromptServedSerial = 0;
    block->delayMetricsSerial = 0;
    block->delayAveragePingMs = -1;
    block->delayMinPingMs = -1;
    block->delayMaxPingMs = -1;
    block->delayRecommended = -1;
    block->delayRangeMin = 0;
    block->delayRangeMax = 20;
    block->delayInputSerial = 0;
    block->delayInputServedSerial = 0;
    block->delayInputValue = -1;
    block->spectateConfirmPromptSerial = 0;
    block->spectateConfirmPromptServedSerial = 0;
    block->spectateConfirmInputSerial = 0;
    block->spectateConfirmInputServedSerial = 0;
    block->spectateConfirmInputValue = 0;
    block->dbgReadConsoleHits = 0;
    block->dbgReadConsoleAutoHits = 0;
    block->dbgCreateProcessHits = 0;
    block->dbgWriteProcessHits = 0;
    block->dbgCreateRemoteThreadHits = 0;
}

bool InvokeStartInitPlayer(int initMode)
{
    if (initMode != kLocalRoleOnline)
    {
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("Takeover: InvokeStartInitPlayer skipped (DLL not loaded)");
        return false;
    }

    const uintptr_t dllBase = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t fnAddr = dllBase + g_activeRevival->startInitPlayerRva;

    // Read the session pointer DIRECTLY from the DLL's global variable
    // (dword_100A02CC at RVA 0xA02CC) without the heuristic validation that
    // ReadSessionPointerFromRevival() performs.  That validation can return a
    // stale / wrong pointer from a previous session or from unrelated memory
    // that happens to pass the vtable + delay/ping heuristic.  Since we only
    // call this immediately after init() stored the freshly-allocated session
    // into that global, a raw read is both correct and sufficient.
    uintptr_t sessionPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(dllBase + g_activeRevival->sessionPtrOffsets[0]), &sessionPtr)
        || sessionPtr == 0 || sessionPtr < 0x00100000u)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer skipped (session pointer unavailable dllBase=0x%08lX raw=0x%08lX)",
            static_cast<unsigned long>(dllBase),
            static_cast<unsigned long>(sessionPtr));
        return false;
    }

    // Verify the session's initComplete flag is still 0 (un-initialized).
    // If already 1 (exactly), the function has already executed (perhaps
    // through the normal vtable path) and calling it again would reset frame
    // counters.  Any other non-zero value is garbage from a stale pointer and
    // must NOT be treated as "already initialized".
    int initComplete = -1;
    if (SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInitComplete), &initComplete)
        && initComplete == 1)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer skipped (already initialized session=0x%08lX initComplete=%d)",
            static_cast<unsigned long>(sessionPtr),
            initComplete);
        return true;
    }

    mod::Log(
        "Takeover: InvokeStartInitPlayer calling sub_10072880 session=0x%08lX fn=0x%08lX initComplete=%d",
        static_cast<unsigned long>(sessionPtr),
        static_cast<unsigned long>(fnAddr),
        initComplete);

    // sub_10072880 is void __thiscall(int this).
    // In MSVC __thiscall: ECX = this, no explicit first parameter.
    typedef void (__thiscall *StartInitPlayerFn)(void* session);
    StartInitPlayerFn fn = reinterpret_cast<StartInitPlayerFn>(fnAddr);

    __try
    {
        fn(reinterpret_cast<void*>(sessionPtr));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "Takeover: InvokeStartInitPlayer EXCEPTION session=0x%08lX fn=0x%08lX",
            static_cast<unsigned long>(sessionPtr),
            static_cast<unsigned long>(fnAddr));
        return false;
    }

    // Verify the session is now marked as initialized.
    int postInitComplete = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInitComplete), &postInitComplete);

    int activePlayer = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetActivePlayer), &activePlayer);
    mod::Log(
        "Takeover: InvokeStartInitPlayer completed session=0x%08lX initComplete=%d activePlayer=%d",
        static_cast<unsigned long>(sessionPtr),
        postInitComplete,
        activePlayer);

    return postInitComplete == 1;
}

void StabilizeOnlineSessionBindingAfterInit(int initMode)
{
    if (initMode != kLocalRoleOnline)
    {
        return;
    }

    if (g_revivalProcessId == 0)
    {
        mod::Log("Takeover: post-init session binding skipped (helper pid unavailable)");
        return;
    }

    bool usedCachedSessionPtr = false;
    const uintptr_t sessionPtr = ReadSessionPointerForMutation(&usedCachedSessionPtr);
    if (sessionPtr == 0)
    {
        mod::Log(
            "Takeover: post-init session binding skipped (session pointer unavailable role=%d helperPid=%lu strict=0 cached=0x%08lX)",
            initMode,
            static_cast<unsigned long>(g_revivalProcessId),
            static_cast<unsigned long>(g_lastValidatedSessionPtr));
        return;
    }

    const char* const sessionPtrSource = usedCachedSessionPtr ? "cached" : "strict";

    int helperPidField = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperPid), &helperPidField);

    uintptr_t helperHandleFieldRaw = 0;
    (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperHandle), &helperHandleFieldRaw);
    HANDLE helperHandleField = reinterpret_cast<HANDLE>(helperHandleFieldRaw);

    DWORD helperHandlePid = 0;
    DWORD helperHandleExitCode = 0xFFFFFFFFu;
    if (helperHandleField != nullptr && helperHandleField != INVALID_HANDLE_VALUE)
    {
        helperHandlePid = GetProcessId(helperHandleField);
        if (!GetExitCodeProcess(helperHandleField, &helperHandleExitCode))
        {
            helperHandleExitCode = 0xFFFFFFFEu;
        }
    }
    const DWORD expectedPid = g_revivalProcessId;
    const bool pidMatches = helperPidField == static_cast<int>(expectedPid);
    const bool handleMatches =
        helperHandleField != nullptr
        && helperHandleField != INVALID_HANDLE_VALUE
        && helperHandlePid == expectedPid;

    bool patchedPid = false;
    bool patchedHandle = false;
    HANDLE replacementHandle = nullptr;
    bool replacementIsOwnedHandle = false;

    if (!pidMatches || !handleMatches)
    {
        const void* const pidFieldAddress = reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperPid);
        const void* const handleFieldAddress = reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperHandle);
        if (!IsWritableRange(const_cast<void*>(pidFieldAddress), sizeof(int))
            || !IsWritableRange(const_cast<void*>(handleFieldAddress), sizeof(uintptr_t)))
        {
            mod::Log(
                "Takeover: post-init session binding skipped (fields not writable session=0x%08lX pidWritable=%d handleWritable=%d)",
                static_cast<unsigned long>(sessionPtr),
                IsWritableRange(const_cast<void*>(pidFieldAddress), sizeof(int)) ? 1 : 0,
                IsWritableRange(const_cast<void*>(handleFieldAddress), sizeof(uintptr_t)) ? 1 : 0);
            return;
        }

        if (!handleMatches)
        {
            replacementHandle = OpenProcess(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, expectedPid);
            replacementIsOwnedHandle = (replacementHandle != nullptr);
            if (replacementHandle == nullptr && g_revivalProcess != nullptr)
            {
                const DWORD hostHandlePid = GetProcessId(g_revivalProcess);
                if (hostHandlePid == expectedPid)
                {
                    HANDLE duplicated = nullptr;
                    if (DuplicateHandle(
                            GetCurrentProcess(),
                            g_revivalProcess,
                            GetCurrentProcess(),
                            &duplicated,
                            PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
                            FALSE,
                            0))
                    {
                        replacementHandle = duplicated;
                        replacementIsOwnedHandle = true;
                    }
                    else
                    {
                        replacementHandle = g_revivalProcess;
                        replacementIsOwnedHandle = false;
                    }
                }
            }
        }

        __try
        {
            if (!pidMatches)
            {
                *reinterpret_cast<int*>(const_cast<void*>(pidFieldAddress)) = static_cast<int>(expectedPid);
                patchedPid = true;
            }
            if (!handleMatches && replacementHandle != nullptr)
            {
                *reinterpret_cast<uintptr_t*>(const_cast<void*>(handleFieldAddress)) = reinterpret_cast<uintptr_t>(replacementHandle);
                patchedHandle = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (replacementIsOwnedHandle && replacementHandle != nullptr)
            {
                CloseHandle(replacementHandle);
            }
            mod::Log("Takeover: post-init session binding patch raised exception");
            return;
        }

        if (patchedHandle
            && helperHandleField != nullptr
            && helperHandleField != INVALID_HANDLE_VALUE
            && helperHandleField != g_revivalProcess
            && helperHandleField != replacementHandle)
        {
            CloseHandle(helperHandleField);
        }
        else if (!patchedHandle && replacementIsOwnedHandle && replacementHandle != nullptr)
        {
            CloseHandle(replacementHandle);
        }

        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperPid), &helperPidField);
        helperHandleFieldRaw = 0;
        (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperHandle), &helperHandleFieldRaw);
        helperHandleField = reinterpret_cast<HANDLE>(helperHandleFieldRaw);
        helperHandlePid = 0;
        helperHandleExitCode = 0xFFFFFFFFu;
        if (helperHandleField != nullptr && helperHandleField != INVALID_HANDLE_VALUE)
        {
            helperHandlePid = GetProcessId(helperHandleField);
            if (!GetExitCodeProcess(helperHandleField, &helperHandleExitCode))
            {
                helperHandleExitCode = 0xFFFFFFFEu;
            }
        }
    }

    mod::Log(
        "Takeover: post-init session binding mode=%d source=%s session=0x%08lX pidField=%d expectedPid=%lu handle=0x%08lX handlePid=%lu handleExit=%ld patched(pid=%d handle=%d)",
        initMode,
        sessionPtrSource,
        static_cast<unsigned long>(sessionPtr),
        helperPidField,
        static_cast<unsigned long>(expectedPid),
        static_cast<unsigned long>(helperHandleFieldRaw),
        static_cast<unsigned long>(helperHandlePid),
        static_cast<long>(helperHandleExitCode),
        patchedPid ? 1 : 0,
        patchedHandle ? 1 : 0);
}

void RepairRollbackHistoryBindingsIfNeeded()
{
    if (!g_localInitAppliedForSession)
    {
        return;
    }

    bool usedCachedSessionPtr = false;
    const uintptr_t sessionPtr = ReadSessionPointerForMutation(&usedCachedSessionPtr);
    if (sessionPtr == 0)
    {
        return;
    }
    const char* const sessionPtrSource = usedCachedSessionPtr ? "cached" : "strict";

    int activePlayer = -1;
    if (!SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetActivePlayer), &activePlayer))
    {
        return;
    }
    if (activePlayer != 0 && activePlayer != 1)
    {
        return;
    }

    int queuePlayer = -1;
    (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetQueuePlayer), &queuePlayer);

    uintptr_t historyPrimaryPtr = 0;
    uintptr_t historySecondaryPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryPtr), &historyPrimaryPtr)
        || !SafeReadPtr(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryPtr), &historySecondaryPtr))
    {
        return;
    }

    const uintptr_t expectedPrimary =
        sessionPtr + ((activePlayer == 0) ? g_activeRevival->sessionOffsetHistoryPrimaryVec : g_activeRevival->sessionOffsetHistorySecondaryVec);
    const uintptr_t expectedSecondary =
        sessionPtr + ((activePlayer == 0) ? g_activeRevival->sessionOffsetHistorySecondaryVec : g_activeRevival->sessionOffsetHistoryPrimaryVec);
    const int expectedQueuePlayer = (activePlayer == 0) ? 1 : 0;

    const bool queuePlayerInvalid = (queuePlayer != 0 && queuePlayer != 1) || queuePlayer != expectedQueuePlayer;
    const bool needsRepair =
        (historyPrimaryPtr != expectedPrimary)
        || (historySecondaryPtr != expectedSecondary)
        || queuePlayerInvalid;
    if (!needsRepair)
    {
        return;
    }

    void* const primaryField = reinterpret_cast<void*>(sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryPtr);
    void* const secondaryField = reinterpret_cast<void*>(sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryPtr);
    void* const queueField = reinterpret_cast<void*>(sessionPtr + g_activeRevival->sessionOffsetQueuePlayer);
    if (!IsWritableRange(primaryField, sizeof(uintptr_t))
        || !IsWritableRange(secondaryField, sizeof(uintptr_t))
        || !IsWritableRange(queueField, sizeof(int)))
    {
        const LONG hits = InterlockedIncrement(&g_sessionHistoryRepairHits);
        if (hits <= 8 || (hits % 64) == 0)
        {
            mod::Log(
                "Takeover: skipped history binding repair source=%s session=0x%08lX writable=%d/%d/%d count=%ld",
                sessionPtrSource,
                static_cast<unsigned long>(sessionPtr),
                IsWritableRange(primaryField, sizeof(uintptr_t)) ? 1 : 0,
                IsWritableRange(secondaryField, sizeof(uintptr_t)) ? 1 : 0,
                IsWritableRange(queueField, sizeof(int)) ? 1 : 0,
                static_cast<long>(hits));
        }
        return;
    }

    __try
    {
        *reinterpret_cast<uintptr_t*>(primaryField) = expectedPrimary;
        *reinterpret_cast<uintptr_t*>(secondaryField) = expectedSecondary;
        *reinterpret_cast<int*>(queueField) = expectedQueuePlayer;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        const LONG hits = InterlockedIncrement(&g_sessionHistoryRepairHits);
        if (hits <= 8 || (hits % 64) == 0)
        {
            mod::Log(
                "Takeover: exception while repairing history bindings source=%s session=0x%08lX count=%ld",
                sessionPtrSource,
                static_cast<unsigned long>(sessionPtr),
                static_cast<long>(hits));
        }
        return;
    }

    const LONG hits = InterlockedIncrement(&g_sessionHistoryRepairHits);
    if (hits <= 8 || (hits % 64) == 0)
    {
        mod::Log(
            "Takeover: repaired history bindings source=%s session=0x%08lX active=%d queue=%d->%d hist=0x%08lX/0x%08lX -> 0x%08lX/0x%08lX count=%ld",
            sessionPtrSource,
            static_cast<unsigned long>(sessionPtr),
            activePlayer,
            queuePlayer,
            expectedQueuePlayer,
            static_cast<unsigned long>(historyPrimaryPtr),
            static_cast<unsigned long>(historySecondaryPtr),
            static_cast<unsigned long>(expectedPrimary),
            static_cast<unsigned long>(expectedSecondary),
            static_cast<long>(hits));
    }
}

} // namespace netplay::bridge::takeover
