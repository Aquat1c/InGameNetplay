// Revival DLL memory introspection and session field manipulation.

#include "netplay/bridge/takeover_internal.h"
#include "crash_handler.h"

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

    LogInitWriteSnapshot("SetLocalRoleFlag_pre");

    // Destroy the current session to prevent leaking the old object.
    const uintptr_t oldSessionPtr = ReadSessionPointerFromRevival();
    DestroyCurrentSession("SetLocalRoleFlag");

    // Prevent init() from chaining another trampoline at 0x401582.
    mod::Log(
        "SetLocalRoleFlag: about to save EXE hook bytes before init() "
        "roleFlag=%d oldSession=0x%08lX",
        roleFlag, static_cast<unsigned long>(oldSessionPtr));
    SaveExeFrameHookBytes();
    // Restore original (pre-hook) bytes at mode-ctor hook sites BEFORE
    // init() so the new trampoline copies clean EXE bytes instead of
    // stale hooks from a previous session's mode (prevents chaining).
    RestoreModeCtorOriginalBytes();
    ResetModeConstructorTrampolineCache();

    // Dump the 10 bytes at 0x401582 right before init().
    {
        uint8_t pre[10] = {};
        memcpy(pre, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "SetLocalRoleFlag: 0x401582 pre-init  "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            pre[0], pre[1], pre[2], pre[3], pre[4],
            pre[5], pre[6], pre[7], pre[8], pre[9]);
    }

    int localParams[2] = {roleFlag, 102};
    mod::Log("SetLocalRoleFlag: calling init(mode=%d, magic=%d)",
             localParams[0], localParams[1]);
    const int result = g_localInitFn(localParams);

    // Dump the 10 bytes AFTER init() to see what sub_1006F160 wrote.
    {
        uint8_t post[10] = {};
        memcpy(post, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "SetLocalRoleFlag: 0x401582 post-init "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            post[0], post[1], post[2], post[3], post[4],
            post[5], post[6], post[7], post[8], post[9]);
    }

    // Undo the EXE frame-hook chain growth at 0x401582.
    mod::Log("SetLocalRoleFlag: restoring saved EXE hook bytes (roleFlag=%d)", roleFlag);
    RestoreExeFrameHookBytes();
    // Mode-ctor originals were already restored before init().
    // For non-local modes (online/spectate/tournament), init() installs
    // fresh hooks at 0x763E50/0x763F04 that intercept mode transitions;
    // those hooks are correct and don't chain through stale trampolines.

    // Verify the restore worked.
    {
        uint8_t verify[10] = {};
        memcpy(verify, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "SetLocalRoleFlag: 0x401582 restored  "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            verify[0], verify[1], verify[2], verify[3], verify[4],
            verify[5], verify[6], verify[7], verify[8], verify[9]);
    }

    // Fix up relative instructions in mode-constructor trampolines.
    FixupModeConstructorTrampolines("SetLocalRoleFlag");

    g_localRoleFlag = roleFlag;

    const uintptr_t newSessionPtr = ReadSessionPointerFromRevival();
    LogInitWriteSnapshot("SetLocalRoleFlag_post");

    mod::Log("Takeover: local role switch mode=%d result=%d reason=%s oldSession=0x%08lX newSession=0x%08lX",
        roleFlag, result, reason != nullptr ? reason : "",
        static_cast<unsigned long>(oldSessionPtr),
        static_cast<unsigned long>(newSessionPtr));
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
static uint8_t g_savedDllExitNearJccBytes[RevivalAddressProfile::kMaxExitProcessNearJccPatches][6];
static bool g_dllExitProcessPatchesSaved = false;

bool SaveAndApplyDllExitProcessPatches()
{
    if (g_dllExitProcessPatchesSaved)
    {
        // H4 diagnostic: this early-out means the next restore will use
        // stale saved bytes from the previous session's save.  Log a
        // warning so we can detect this in the trace.
        mod::Log("SaveAndApplyDllExitProcessPatches: SKIPPED (flag already true) "
                 "— H4: next restore will use previously saved bytes!");
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

    // --- Single-byte Jcc patches (0x74/0x75 → 0xEB) ---
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
            mod::Log("SaveAndApplyDllExitProcessPatches: site %zu RVA 0x%lX: "
                     "patched 0x%02X -> 0xEB",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(expected));
        }
        else
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: site %zu RVA 0x%lX: "
                     "VirtualProtect failed err=%lu",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned long>(GetLastError()));
        }
    }

    // --- 6-byte near-Jcc NOP patches (0F 84/85 rel32 → 6× NOP) ---
    int nearApplied = 0;
    for (size_t i = 0; i < g_activeRevival->exitProcessNearJccCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessNearJccRva[i];
        if (rva == 0)
        {
            continue;
        }

        auto* ptr = reinterpret_cast<uint8_t*>(base + rva);

        // Save original 6 bytes.
        std::memcpy(g_savedDllExitNearJccBytes[i], ptr, 6);

        // Validate: expect 0F 84 (jz near) or 0F 85 (jnz near).
        if (ptr[0] != 0x0F || (ptr[1] != 0x84 && ptr[1] != 0x85))
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: near-Jcc %zu at RVA 0x%lX: "
                     "expected 0F 84/85, found %02X %02X — skipping",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(ptr[0]),
                     static_cast<unsigned>(ptr[1]));
            continue;
        }

        DWORD oldProtect = 0;
        if (VirtualProtect(ptr, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            std::memset(ptr, 0x90, 6); // 6× NOP
            DWORD ignored = 0;
            VirtualProtect(ptr, 6, oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), ptr, 6);
            ++nearApplied;
            mod::Log("SaveAndApplyDllExitProcessPatches: near-Jcc %zu RVA 0x%lX: "
                     "patched %02X %02X -> 6xNOP",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(g_savedDllExitNearJccBytes[i][0]),
                     static_cast<unsigned>(g_savedDllExitNearJccBytes[i][1]));
        }
        else
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: near-Jcc %zu RVA 0x%lX: "
                     "VirtualProtect failed err=%lu",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned long>(GetLastError()));
        }
    }

    g_dllExitProcessPatchesSaved = true;
    mod::Log("SaveAndApplyDllExitProcessPatches: applied %d/%zu single-byte + "
             "%d/%zu near-Jcc patches",
             applied, g_activeRevival->exitProcessPatchCount,
             nearApplied, g_activeRevival->exitProcessNearJccCount);

    // --- Post-apply verification pass ---
    int verifyFail = 0;
    for (size_t i = 0; i < g_activeRevival->exitProcessPatchCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessPatchRva[i];
        if (rva == 0) continue;
        const auto* ptr = reinterpret_cast<const uint8_t*>(base + rva);
        if (*ptr != 0xEB)
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: VERIFY FAIL site %zu RVA 0x%lX: "
                     "expected 0xEB, found 0x%02X",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(*ptr));
            ++verifyFail;
        }
    }
    for (size_t i = 0; i < g_activeRevival->exitProcessNearJccCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessNearJccRva[i];
        if (rva == 0) continue;
        const auto* ptr = reinterpret_cast<const uint8_t*>(base + rva);
        if (ptr[0] != 0x90 || ptr[1] != 0x90)
        {
            mod::Log("SaveAndApplyDllExitProcessPatches: VERIFY FAIL near-Jcc %zu RVA 0x%lX: "
                     "expected 90 90, found %02X %02X",
                     i, static_cast<unsigned long>(rva),
                     static_cast<unsigned>(ptr[0]),
                     static_cast<unsigned>(ptr[1]));
            ++verifyFail;
        }
    }
    if (verifyFail > 0)
    {
        mod::Log("SaveAndApplyDllExitProcessPatches: WARNING — %d patches failed verification!",
                 verifyFail);
    }
    else
    {
        mod::Log("SaveAndApplyDllExitProcessPatches: all patches verified OK");
    }

    return (applied + nearApplied) > 0;
}

bool RestoreDllExitProcessPatches()
{
    if (!g_dllExitProcessPatchesSaved || g_activeRevival == nullptr)
    {
        mod::Log("RestoreDllExitProcessPatches: SKIPPED (saved=%d profile=%p)",
                 g_dllExitProcessPatchesSaved ? 1 : 0,
                 static_cast<const void*>(g_activeRevival));
        return false;
    }

    // Log the saved bytes we're about to restore (H4 diagnostic)
    for (size_t i = 0; i < g_activeRevival->exitProcessPatchCount; ++i)
    {
        mod::Log("RestoreDllExitProcessPatches: will restore site[%zu] RVA=0x%lX "
                 "savedByte=0x%02X",
                 i,
                 static_cast<unsigned long>(g_activeRevival->exitProcessPatchRva[i]),
                 static_cast<unsigned>(g_savedDllExitProcessBytes[i]));
    }
    for (size_t i = 0; i < g_activeRevival->exitProcessNearJccCount; ++i)
    {
        mod::Log("RestoreDllExitProcessPatches: will restore nearJcc[%zu] RVA=0x%lX "
                 "savedBytes=%02X %02X %02X %02X %02X %02X",
                 i,
                 static_cast<unsigned long>(g_activeRevival->exitProcessNearJccRva[i]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][0]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][1]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][2]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][3]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][4]),
                 static_cast<unsigned>(g_savedDllExitNearJccBytes[i][5]));
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    int restored = 0;

    // --- Restore single-byte Jcc patches ---
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

    // --- Restore 6-byte near-Jcc patches ---
    int nearRestored = 0;
    for (size_t i = 0; i < g_activeRevival->exitProcessNearJccCount; ++i)
    {
        const uintptr_t rva = g_activeRevival->exitProcessNearJccRva[i];
        if (rva == 0)
        {
            continue;
        }

        auto* ptr = reinterpret_cast<uint8_t*>(base + rva);

        DWORD oldProtect = 0;
        if (VirtualProtect(ptr, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
        {
            std::memcpy(ptr, g_savedDllExitNearJccBytes[i], 6);
            DWORD ignored = 0;
            VirtualProtect(ptr, 6, oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), ptr, 6);
            ++nearRestored;
        }
    }

    g_dllExitProcessPatchesSaved = false;
    mod::Log("RestoreDllExitProcessPatches: restored %d/%zu single-byte + "
             "%d/%zu near-Jcc patches",
             restored, g_activeRevival->exitProcessPatchCount,
             nearRestored, g_activeRevival->exitProcessNearJccCount);
    return (restored + nearRestored) > 0;
}

// ---------------------------------------------------------------------------
// ForceLocalPlayInit — unconditionally call init(2,102) to create a fresh
// local play session, then invoke the session's vtable[1] init method to
// fully initialize it (audio, BGM, etc.) before any other hooks dispatch
// to the new session.
//
// ---------------------------------------------------------------------------
// DestroyCurrentSession — tear down the current Revival session object
// BEFORE calling init() to create a new one.
//
// Root cause fix for the 2nd-session crash: init() allocates a new session
// via operator new, runs the constructor, and writes the pointer to
// dword_100A02CC — WITHOUT freeing or destructing the old session.  Every
// init() call therefore leaks the previous session's memory and OS handles.
// After several init() calls, heap corruption from these leaked objects
// causes a vtable dispatch crash in EFZ_GameMode_InvokeAdvance.
//
// The DLL's own mid-game swap function (sub_1006D810) demonstrates the
// correct pattern:
//     void* old = dword_100A02CC;
//     vtable[0](old, 1);              // scalar deleting destructor + free
//     dword_100A02CC = operator new(size);
//     ...
//
// We replicate that pattern here.  Additionally, we close the process
// handle at session offset +700 (helperHandle) for online/spectator
// sessions because the DLL's destructor does NOT close it — the vanilla
// DLL relies on ExitProcess for final handle cleanup.
//
// Session sizes per mode (from init() at RVA 0x6E830):
//   Mode 0 (Online):     0x5D0 = 1488 bytes — offset 700 IN BOUNDS
//   Mode 1 (Spectator):  0x440 = 1088 bytes — offset 700 IN BOUNDS
//   Mode 2 (Local play): 0x2B0 =  688 bytes — offset 700 OUT OF BOUNDS
//   Mode 3 (Tournament): 0x310 =  784 bytes — offset 700 in bounds (unused)
// ---------------------------------------------------------------------------
bool DestroyCurrentSession(const char* caller)
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr
        || g_activeRevival == nullptr
        || g_activeRevival->sessionPtrOffsetCount == 0)
    {
        mod::Log("%s: DestroyCurrentSession skipped (DLL not loaded)", caller);
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);

    // Read current session pointer from dword_100A02CC.
    const uintptr_t sessionGlobalAddr =
        base + g_activeRevival->sessionPtrOffsets[0];
    uintptr_t sessionPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionGlobalAddr),
                     &sessionPtr)
        || sessionPtr == 0)
    {
        mod::Log("%s: DestroyCurrentSession skipped (session NULL)", caller);
        return false;
    }

    const int currentRole = g_localRoleFlag;

    mod::Log(
        "%s: DestroyCurrentSession starting session=0x%08lX role=%d",
        caller,
        static_cast<unsigned long>(sessionPtr),
        currentRole);

    // Close the helper process handle for online/spectator sessions.
    // Mode 2 (local play, 688 bytes) has offset 700 out of bounds;
    // Mode 3 (tournament, 784 bytes) has it in bounds but unused.
    if (currentRole == kLocalRoleOnline || currentRole == kLocalRoleSpectate)
    {
        uintptr_t helperHandle = 0;
        if (SafeReadPtr(
                reinterpret_cast<const void*>(
                    sessionPtr + g_activeRevival->sessionOffsetHelperHandle),
                &helperHandle)
            && helperHandle != 0
            && helperHandle != static_cast<uintptr_t>(~uintptr_t(0)))
        {
            const BOOL closed =
                CloseHandle(reinterpret_cast<HANDLE>(helperHandle));
            mod::Log(
                "%s: DestroyCurrentSession closed helperHandle=0x%08lX result=%d",
                caller,
                static_cast<unsigned long>(helperHandle),
                closed);
        }
    }

    // Read vtable pointer.
    uintptr_t vtablePtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtablePtr)
        || vtablePtr == 0)
    {
        mod::Log(
            "%s: DestroyCurrentSession WARN vtable NULL session=0x%08lX, zeroing ptr only",
            caller,
            static_cast<unsigned long>(sessionPtr));
        goto zero_globals;
    }

    mod::Log(
        "%s: DestroyCurrentSession vtable=0x%08lX (RVA 0x%08lX)",
        caller,
        static_cast<unsigned long>(vtablePtr),
        static_cast<unsigned long>(vtablePtr - base));

    // Read vtable[0] — the scalar deleting destructor.
    {
        uintptr_t vtableSlot0 = 0;
        if (!SafeReadPtr(reinterpret_cast<const void*>(vtablePtr),
                         &vtableSlot0)
            || vtableSlot0 == 0)
        {
            mod::Log(
                "%s: DestroyCurrentSession WARN vtable[0] NULL vtable=0x%08lX, zeroing ptr only",
                caller,
                static_cast<unsigned long>(vtablePtr));
            goto zero_globals;
        }

        // Validate that vtable[0] points into the Revival DLL image.
        uintptr_t revBase = 0, revEnd = 0;
        if (ReadModuleImageRange(revival, &revBase, &revEnd))
        {
            if (vtableSlot0 < revBase || vtableSlot0 >= revEnd)
            {
                mod::Log(
                    "%s: DestroyCurrentSession SKIPPED — vtable[0]=0x%08lX "
                    "outside DLL [0x%08lX..0x%08lX], zeroing ptr only",
                    caller,
                    static_cast<unsigned long>(vtableSlot0),
                    static_cast<unsigned long>(revBase),
                    static_cast<unsigned long>(revEnd));
                goto zero_globals;
            }
        }

        // Call vtable[0](session, 1) as __thiscall.
        // Flag 1 = destruct AND free (operator delete).
        // This is exactly what sub_1006D810 does during mid-game mode swap.
        typedef void(__thiscall* ScalarDeletingDtorFn)(void*, int);
        auto dtorFn = reinterpret_cast<ScalarDeletingDtorFn>(vtableSlot0);

        mod::Log(
            "%s: DestroyCurrentSession calling dtor vtable[0]=0x%08lX "
            "session=0x%08lX role=%d",
            caller,
            static_cast<unsigned long>(vtableSlot0),
            static_cast<unsigned long>(sessionPtr),
            currentRole);

        dtorFn(reinterpret_cast<void*>(sessionPtr), 1);

        mod::Log("%s: DestroyCurrentSession destructor completed", caller);
    }

zero_globals:
    // Zero ALL session pointer globals to prevent stale references.
    int zeroed = 0;
    for (size_t i = 0; i < g_activeRevival->sessionPtrOffsetCount; ++i)
    {
        const uintptr_t offset = g_activeRevival->sessionPtrOffsets[i];
        if (offset != 0)
        {
            uintptr_t* addr = reinterpret_cast<uintptr_t*>(base + offset);
            if (IsWritableRange(addr, sizeof(uintptr_t)))
            {
                *addr = 0;
                ++zeroed;
            }
        }
    }

    // Reset the mode-constructor trampoline fixup cache.  The destructor
    // unhooks 0x763E50/0x763F04 and frees the old trampolines.  If a
    // subsequent init() allocates a new trampoline at the same heap
    // address, the g_lastFixedTrampoline[] guard must NOT skip it —
    // the new trampoline has fresh unrelocated bytes that need fixup.
    ResetModeConstructorTrampolineCache();

    mod::Log(
        "%s: DestroyCurrentSession done session=0x%08lX role=%d globalsZeroed=%d",
        caller,
        static_cast<unsigned long>(sessionPtr),
        currentRole,
        zeroed);

    return true;
}

// ---------------------------------------------------------------------------
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
// ---------------------------------------------------------------------------
// InvokeSessionVtableInit — read the session pointer from dword_100A02CC and
// call vtable slot 1 (vtable+4 = the init method).  This must be called
// immediately after init() so that field initialization (BGM manager,
// audio, etc.) completes before any other JMP-patched hook dispatches to
// the new session object.  Without this call, the spectator object's BGM
// manager pointer (offset +1068) stays NULL and the BGM dispatch hook at
// 0x40DE80 crashes on a double-dereference.
//
// Used by ForceLocalPlayInit (mode 2) and the spectator init path (mode 1).
// ---------------------------------------------------------------------------
bool InvokeSessionVtableInit(const char* caller)
{
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr
        || g_activeRevival == nullptr
        || g_activeRevival->sessionPtrOffsetCount == 0)
    {
        mod::Log("%s: InvokeSessionVtableInit skipped (DLL not loaded)", caller);
        return false;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t sessionGlobalAddr =
        base + g_activeRevival->sessionPtrOffsets[0];
    uintptr_t sessionPtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionGlobalAddr),
                     &sessionPtr)
        || sessionPtr == 0)
    {
        mod::Log("%s: InvokeSessionVtableInit skipped (session NULL)", caller);
        return false;
    }

    uintptr_t vtablePtr = 0;
    if (!SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtablePtr)
        || vtablePtr == 0)
    {
        mod::Log("%s: InvokeSessionVtableInit skipped (vtable NULL)", caller);
        return false;
    }

    uintptr_t vtableSlot1 = 0;
    if (!SafeReadPtr(
            reinterpret_cast<const void*>(vtablePtr + sizeof(uintptr_t)),
            &vtableSlot1)
        || vtableSlot1 == 0)
    {
        mod::Log("%s: InvokeSessionVtableInit skipped (vtable[1] NULL)", caller);
        return false;
    }

    // __thiscall: this in ECX, no extra args.
    typedef void(__thiscall* SessionInitFn)(void*);
    auto initFn = reinterpret_cast<SessionInitFn>(vtableSlot1);
    initFn(reinterpret_cast<void*>(sessionPtr));
    mod::Log(
        "%s: InvokeSessionVtableInit vtable[1] 0x%08lX called on session 0x%08lX",
        caller,
        static_cast<unsigned long>(vtableSlot1),
        static_cast<unsigned long>(sessionPtr));
    return true;
}

bool ForceLocalPlayInit()
{
    if (g_localInitFn == nullptr)
    {
        mod::Log("ForceLocalPlayInit: init function not available");
        return false;
    }

    IncrementForceLocalPlayInitCount();
    const int callCount = GetForceLocalPlayInitCount();

    // --- Full snapshot BEFORE init() ---
    LogInitWriteSnapshot("ForceLocalPlayInit_pre");

    // If we were the client (P2 / joiner), Revival swapped the P1/P2
    // input-config blocks during StartInitPlayer.  Reverse that swap now
    // BEFORE destroying the session so controls return to their default
    // layout for local play.
    if (g_netplayRole == kNetplayRoleClient)
    {
        ReverseInputSwapIfClient();
    }
    g_netplayRole = kNetplayRoleNone;

    // Destroy the current session to prevent leaking the old object.
    // This is the root cause fix for the 2nd-session crash (H1).
    const uintptr_t oldSessionPtr = ReadSessionPointerFromRevival();
    DestroyCurrentSession("ForceLocalPlayInit");

    // Prevent init() from chaining another trampoline at 0x401582.
    mod::Log(
        "ForceLocalPlayInit: about to save EXE hook bytes before init() "
        "oldSession=0x%08lX callCount=%d",
        static_cast<unsigned long>(oldSessionPtr), callCount);
    SaveExeFrameHookBytes();
    // Restore original (pre-hook) bytes at mode-ctor hook sites BEFORE
    // init() so the new trampoline copies clean EXE bytes instead of
    // stale hooks from a previous session's mode (prevents chaining
    // trampolines across sessions — root cause of the 0x26D19881 crash).
    RestoreModeCtorOriginalBytes();
    ResetModeConstructorTrampolineCache();

    // Dump the 10 bytes at 0x401582 right before init() for verification.
    {
        uint8_t pre[10] = {};
        memcpy(pre, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "ForceLocalPlayInit: 0x401582 pre-init  "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            pre[0], pre[1], pre[2], pre[3], pre[4],
            pre[5], pre[6], pre[7], pre[8], pre[9]);
    }

    int localParams[2] = {kLocalRoleLocalPlay, 102};
    mod::Log("ForceLocalPlayInit: calling init(mode=%d, magic=%d)",
             localParams[0], localParams[1]);
    const int result = g_localInitFn(localParams);

    // Dump the 10 bytes AFTER init() to see what sub_1006F160 wrote.
    {
        uint8_t post[10] = {};
        memcpy(post, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "ForceLocalPlayInit: 0x401582 post-init "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            post[0], post[1], post[2], post[3], post[4],
            post[5], post[6], post[7], post[8], post[9]);
    }

    // Undo the trampoline chain growth at 0x401582 — restore saved bytes.
    mod::Log("ForceLocalPlayInit: restoring saved EXE hook bytes");
    RestoreExeFrameHookBytes();
    // Mode-ctor originals were already restored before init(); local play
    // init(2,102) does not install hooks at 0x763E50/0x763F04, so the
    // originals are still in place.  No further action needed.

    // Verify the restore worked.
    {
        uint8_t verify[10] = {};
        memcpy(verify, reinterpret_cast<const void*>(0x401582), 10);
        mod::Log(
            "ForceLocalPlayInit: 0x401582 restored  "
            "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
            verify[0], verify[1], verify[2], verify[3], verify[4],
            verify[5], verify[6], verify[7], verify[8], verify[9]);
    }

    // Fix up relative instructions in mode-constructor trampolines.
    FixupModeConstructorTrampolines("ForceLocalPlayInit");

    g_localRoleFlag = kLocalRoleLocalPlay;

    const uintptr_t newSessionPtr = ReadSessionPointerFromRevival();

    // --- Full snapshot AFTER init() (before vtable[1]) ---
    LogInitWriteSnapshot("ForceLocalPlayInit_post_init");

    mod::Log(
        "ForceLocalPlayInit: init(2,102) result=%d callCount=%d oldSession=0x%08lX newSession=0x%08lX",
        result, callCount,
        static_cast<unsigned long>(oldSessionPtr),
        static_cast<unsigned long>(newSessionPtr));

    // Immediately call vtable[1] on the new session so that BGM and other
    // fields are initialized before any dispatch hook fires.
    (void)InvokeSessionVtableInit("ForceLocalPlayInit");

    // --- Full snapshot AFTER vtable[1] init ---
    LogInitWriteSnapshot("ForceLocalPlayInit_post_vtable1");

    return true;
}

// ---------------------------------------------------------------------------
// ReverseInputSwapIfClient — calls Revival's EFZ_Obj_SubStruct448_CleanupPair
// on dword_100A0760 to toggle the P1/P2 input-config swap back to its
// original state.  Only fires when g_netplayRole == kNetplayRoleClient,
// meaning we joined as P2 and Revival swapped the two 4-DWORD controller
// blocks during StartInitPlayer.  The swap is a toggle (XOR-style), so
// calling it a second time restores the original layout.
//
// Must be called during session teardown (ForceLocalPlayInit) BEFORE the
// session object is destroyed, though the swap targets a persistent global
// structure (dword_100A0760[20]+448) that survives session changes.
// ---------------------------------------------------------------------------
bool ReverseInputSwapIfClient()
{
    if (g_netplayRole != kNetplayRoleClient)
    {
        mod::Log("ReverseInputSwapIfClient: skipped (role=%d, not client)",
                 g_netplayRole);
        return false;
    }

    if (g_activeRevival == nullptr
        || g_activeRevival->inputSwapPairRva == 0
        || g_activeRevival->renderContextBaseOffset == 0)
    {
        mod::Log("ReverseInputSwapIfClient: skipped (address profile incomplete)");
        return false;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("ReverseInputSwapIfClient: skipped (Revival DLL not loaded)");
        return false;
    }

    const uintptr_t dllBase = reinterpret_cast<uintptr_t>(revival);
    const uintptr_t contextBaseAddr = dllBase + g_activeRevival->renderContextBaseOffset;

    // EFZ_Obj_SubStruct448_CleanupPair is __thiscall with
    // this = &dword_100A0760 = dllBase + renderContextBaseOffset.
    typedef int(__thiscall* SwapInputsFn)(void* thisPtr);
    auto swapFn = reinterpret_cast<SwapInputsFn>(
        dllBase + g_activeRevival->inputSwapPairRva);

    __try
    {
        const int result = swapFn(reinterpret_cast<void*>(contextBaseAddr));
        mod::Log(
            "ReverseInputSwapIfClient: swap reversed OK (fn=0x%08lX this=0x%08lX result=%d)",
            static_cast<unsigned long>(dllBase + g_activeRevival->inputSwapPairRva),
            static_cast<unsigned long>(contextBaseAddr),
            result);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "ReverseInputSwapIfClient: EXCEPTION calling swap fn=0x%08lX this=0x%08lX",
            static_cast<unsigned long>(dllBase + g_activeRevival->inputSwapPairRva),
            static_cast<unsigned long>(contextBaseAddr));
        return false;
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

    // H2 diagnostic: log the init-once guard to show whether future init()
    // calls will refresh the render context (guard==0) or skip it (guard!=0).
    int initOnceGuard = -1;
    if (g_activeRevival->initOnceGuardOffset != 0)
    {
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->initOnceGuardOffset),
            &initOnceGuard);
    }
    mod::Log("SaveRenderContext: saved EfzRender* 0x%08lX from offset 0x%lX "
             "initOnceGuard=0x%08X (H2: %s)",
             static_cast<unsigned long>(value),
             static_cast<unsigned long>(g_activeRevival->renderContextGlobalOffset),
             static_cast<unsigned>(initOnceGuard),
             (initOnceGuard & 0xFF) != 0
                 ? "guard SET — init() will NOT refresh renderCtx"
                 : "guard CLEAR — init() will refresh renderCtx");
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

    // H2 diagnostic: read current render context before we overwrite it
    uintptr_t currentRenderCtx = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(base + g_activeRevival->renderContextGlobalOffset),
        &currentRenderCtx);

    auto* ptr = reinterpret_cast<uintptr_t*>(
        base + g_activeRevival->renderContextGlobalOffset);
    *ptr = g_savedRenderContext;
    mod::Log("RestoreRenderContext: restored EfzRender* 0x%08lX (was 0x%08lX, %s)",
             static_cast<unsigned long>(g_savedRenderContext),
             static_cast<unsigned long>(currentRenderCtx),
             currentRenderCtx == 0 ? "was NULL — H2 confirmed stale!"
                                   : (currentRenderCtx == g_savedRenderContext
                                          ? "unchanged"
                                          : "was different"));
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

// ---------------------------------------------------------------------------
// NetplayFrameHook — wraps sub_1006E590 (the DLL per-frame dispatcher,
// RVA 0x6E590 in EfzRevival.dll 1.02e) with a setjmp recovery point.
//
// For tournament sessions, Jcc patches make ExitProcess calls unreachable.
// For netplay (online/spectate) sessions, no Jcc patches are applied, so
// ExitProcess CAN fire from EFZ_Main_RollbackLoopTick when the peer process
// terminates.  Because ExitProcess is called on the EFZ.exe main game loop
// thread (via the frame hook dispatch chain), NeutralizeExitProcess must NOT
// hang that thread with Sleep(INFINITE).
//
// This hook installs a 6-byte JMP at sub_1006E590's entry so every frame
// passes through OurFrameDispatch.  OurFrameDispatch sets a jmp_buf before
// calling the original function.  NeutralizeExitProcess then longjmp()s
// back here instead of sleeping, allowing the main loop to continue normally.
// On the next title-screen frame, ConsumeRevivalExitInterception runs the
// full cleanup and re-enters the netplay menu.
//
// sub_1006E590 first 6 bytes (complete instructions, safe trampoline unit):
//   55        push ebp
//   8B EC     mov  ebp, esp
//   83 E4 C0  and  esp, 0xC0   (64-byte stack alignment)
// ---------------------------------------------------------------------------

static constexpr uintptr_t kFrameHookRva = 0x6E590u;

// The jmp_buf and active-flag are read by NeutralizeExitProcess in
// iat_stubs.cpp.  They are declared extern in takeover_internal.h.
jmp_buf         g_netplayFrameJmpBuf    = {};
volatile bool   g_netplayFrameJmpActive = false;

// Fallback recovery context armed by HookedTitleUpdateImpl while title/menu
// logic is executing. Used when ExitProcess fires outside OurFrameDispatch.
jmp_buf         g_netplayUiJmpBuf       = {};
volatile bool   g_netplayUiJmpActive    = false;

// Trampoline: first 6 original bytes + near JMP back to original+6.
static uint8_t g_frameHookTrampoline[12] = {};
static bool    g_frameHookInstalled      = false;

using FrameDispatchFn = void (*)();
static FrameDispatchFn g_origFrameDispatch = nullptr;

// ---------------------------------------------------------------------------
// EXE frame-hook trampoline chain prevention
//
// Revival DLL's init() calls sub_1006F160(0x401582, 10, sub_1006E590) EVERY
// TIME it runs.  sub_1006F160 is an inline-hook installer that:
//   1. Reads the current 10 bytes at 0x401582 into a new malloc'd trampoline
//      (raw memcpy — no relocation of relative branches).
//   2. Overwrites 0x401582 with a JMP to the new trampoline + NOP padding.
//
// After the very first init() (run by Revival DLL at startup), 0x401582
// contains a JMP to Trampoline-1, whose displaced bytes are the genuine
// original EXE instructions — safe to execute from any address.
//
// When our code calls init() again (Tick_init_handshake, ForceLocalPlayInit,
// SetLocalRoleFlag), sub_1006F160 creates Trampoline-2 whose displaced bytes
// are the raw JMP from 0x401582 → Trampoline-1.  But that JMP is an E9
// rel32 displacement calculated for 0x401582; executing it from Trampoline-2
// produces a wild target address → EIP crash (0x22FFF281, 0x2702FBB9, etc.).
//
// Fix: save the 10 bytes at 0x401582 BEFORE every init() call from our code
// and restore them AFTER.  init() still creates/registers the session object
// and writes all DLL globals; only the trampoline chain growth is undone.
// ---------------------------------------------------------------------------
static constexpr uintptr_t kExeFrameHookAddr = 0x401582u;
static constexpr size_t    kExeFrameHookSize = 10u;
static uint8_t g_exeFrameHookSaved[kExeFrameHookSize] = {};
static bool    g_exeFrameHookSavedValid = false;

void SaveExeFrameHookBytes()
{
    memcpy(g_exeFrameHookSaved,
           reinterpret_cast<const void*>(kExeFrameHookAddr),
           kExeFrameHookSize);
    g_exeFrameHookSavedValid = true;
    mod::Log("SaveExeFrameHookBytes: saved %zu bytes at 0x%08lX "
             "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
             kExeFrameHookSize,
             static_cast<unsigned long>(kExeFrameHookAddr),
             g_exeFrameHookSaved[0], g_exeFrameHookSaved[1],
             g_exeFrameHookSaved[2], g_exeFrameHookSaved[3],
             g_exeFrameHookSaved[4], g_exeFrameHookSaved[5],
             g_exeFrameHookSaved[6], g_exeFrameHookSaved[7],
             g_exeFrameHookSaved[8], g_exeFrameHookSaved[9]);
}

void RestoreExeFrameHookBytes()
{
    if (!g_exeFrameHookSavedValid)
    {
        mod::Log("RestoreExeFrameHookBytes: no saved bytes — skipped");
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kExeFrameHookAddr),
                        kExeFrameHookSize,
                        PAGE_EXECUTE_READWRITE,
                        &oldProtect))
    {
        mod::Log("RestoreExeFrameHookBytes: VirtualProtect failed err=%lu",
                 static_cast<unsigned long>(GetLastError()));
        return;
    }

    memcpy(reinterpret_cast<void*>(kExeFrameHookAddr),
           g_exeFrameHookSaved,
           kExeFrameHookSize);

    VirtualProtect(reinterpret_cast<void*>(kExeFrameHookAddr),
                   kExeFrameHookSize,
                   oldProtect,
                   &oldProtect);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(kExeFrameHookAddr),
                          kExeFrameHookSize);

    // Invalidate the saved buffer so a subsequent Restore without a
    // matching Save gives a clear diagnostic instead of silently
    // writing stale bytes from a previous session.
    g_exeFrameHookSavedValid = false;

    // Read back for verification.
    uint8_t verify[kExeFrameHookSize] = {};
    memcpy(verify,
           reinterpret_cast<const void*>(kExeFrameHookAddr),
           kExeFrameHookSize);
    mod::Log("RestoreExeFrameHookBytes: restored %zu bytes at 0x%08lX "
             "[%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X]",
             kExeFrameHookSize,
             static_cast<unsigned long>(kExeFrameHookAddr),
             verify[0], verify[1], verify[2], verify[3], verify[4],
             verify[5], verify[6], verify[7], verify[8], verify[9]);
}

// ---------------------------------------------------------------------------
// Save / restore bytes at EXE addresses 0x763E50 (7 bytes) and 0x763F04
// (7 bytes) — the mode-constructor hook sites.  Prevents trampoline chain
// growth in the same way SaveExeFrameHookBytes prevents it at 0x401582.
//
// Each init() call has the mode constructor run sub_1006F160 which reads
// the bytes at the hook site, allocates a new trampoline, copies the bytes,
// and overwrites the hook site with JMP trampoline.  Without save/restore
// the chain grows by one link per init() call — leaking ~16 bytes of
// malloc'd memory per link and adding hot-path PUSHAD/POPFD overhead.
// ---------------------------------------------------------------------------
static constexpr size_t kModeCtorHookCount = 2;
static constexpr uintptr_t kModeCtorHookAddrs[kModeCtorHookCount] = {
    0x763E50, 0x763F04
};
static constexpr size_t kModeCtorHookPatchSize = 7;
static uint8_t g_modeCtorHookSaved[kModeCtorHookCount][kModeCtorHookPatchSize] = {};
static bool    g_modeCtorHookSavedValid = false;

// Permanent copy of the original (pre-hook) EXE bytes at the mode-ctor
// hook sites.  Captured once on first use and never overwritten.  Used to
// restore a clean state before every init() call so that new trampolines
// never chain through stale hooks from a previous session's mode.
static uint8_t g_modeCtorOriginalBytes[kModeCtorHookCount][kModeCtorHookPatchSize] = {};
static bool    g_modeCtorOriginalsSaved = false;

static void SaveModeCtorOriginalBytesOnce()
{
    if (g_modeCtorOriginalsSaved)
        return;
    for (size_t i = 0; i < kModeCtorHookCount; ++i)
    {
        memcpy(g_modeCtorOriginalBytes[i],
               reinterpret_cast<const void*>(kModeCtorHookAddrs[i]),
               kModeCtorHookPatchSize);
    }
    g_modeCtorOriginalsSaved = true;
    mod::Log(
        "SaveModeCtorOriginalBytesOnce: captured "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X] "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X]",
        static_cast<unsigned long>(kModeCtorHookAddrs[0]),
        g_modeCtorOriginalBytes[0][0], g_modeCtorOriginalBytes[0][1],
        g_modeCtorOriginalBytes[0][2], g_modeCtorOriginalBytes[0][3],
        g_modeCtorOriginalBytes[0][4], g_modeCtorOriginalBytes[0][5],
        g_modeCtorOriginalBytes[0][6],
        static_cast<unsigned long>(kModeCtorHookAddrs[1]),
        g_modeCtorOriginalBytes[1][0], g_modeCtorOriginalBytes[1][1],
        g_modeCtorOriginalBytes[1][2], g_modeCtorOriginalBytes[1][3],
        g_modeCtorOriginalBytes[1][4], g_modeCtorOriginalBytes[1][5],
        g_modeCtorOriginalBytes[1][6]);
}

void SaveModeCtorHookBytes()
{
    for (size_t i = 0; i < kModeCtorHookCount; ++i)
    {
        memcpy(g_modeCtorHookSaved[i],
               reinterpret_cast<const void*>(kModeCtorHookAddrs[i]),
               kModeCtorHookPatchSize);
    }
    g_modeCtorHookSavedValid = true;

    mod::Log(
        "SaveModeCtorHookBytes: saved 0x%08lX=[%02X %02X %02X %02X %02X %02X %02X] "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X]",
        static_cast<unsigned long>(kModeCtorHookAddrs[0]),
        g_modeCtorHookSaved[0][0], g_modeCtorHookSaved[0][1],
        g_modeCtorHookSaved[0][2], g_modeCtorHookSaved[0][3],
        g_modeCtorHookSaved[0][4], g_modeCtorHookSaved[0][5],
        g_modeCtorHookSaved[0][6],
        static_cast<unsigned long>(kModeCtorHookAddrs[1]),
        g_modeCtorHookSaved[1][0], g_modeCtorHookSaved[1][1],
        g_modeCtorHookSaved[1][2], g_modeCtorHookSaved[1][3],
        g_modeCtorHookSaved[1][4], g_modeCtorHookSaved[1][5],
        g_modeCtorHookSaved[1][6]);
}

void DiscardModeCtorHookBytes()
{
    if (g_modeCtorHookSavedValid)
    {
        g_modeCtorHookSavedValid = false;
        mod::Log("DiscardModeCtorHookBytes: discarded saved bytes without restoring");
    }
}

void RestoreModeCtorOriginalBytes()
{
    SaveModeCtorOriginalBytesOnce();
    if (!g_modeCtorOriginalsSaved)
    {
        mod::Log("RestoreModeCtorOriginalBytes: no originals captured — skipped");
        return;
    }

    for (size_t i = 0; i < kModeCtorHookCount; ++i)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                            kModeCtorHookPatchSize,
                            PAGE_EXECUTE_READWRITE,
                            &oldProtect))
        {
            mod::Log(
                "RestoreModeCtorOriginalBytes: VirtualProtect(0x%08lX) failed err=%lu",
                static_cast<unsigned long>(kModeCtorHookAddrs[i]),
                static_cast<unsigned long>(GetLastError()));
            continue;
        }

        memcpy(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
               g_modeCtorOriginalBytes[i],
               kModeCtorHookPatchSize);

        VirtualProtect(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                       kModeCtorHookPatchSize,
                       oldProtect,
                       &oldProtect);
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                              kModeCtorHookPatchSize);
    }

    // Read back for verification.
    uint8_t v0[kModeCtorHookPatchSize] = {};
    uint8_t v1[kModeCtorHookPatchSize] = {};
    memcpy(v0, reinterpret_cast<const void*>(kModeCtorHookAddrs[0]), kModeCtorHookPatchSize);
    memcpy(v1, reinterpret_cast<const void*>(kModeCtorHookAddrs[1]), kModeCtorHookPatchSize);
    mod::Log(
        "RestoreModeCtorOriginalBytes: restored "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X] "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X]",
        static_cast<unsigned long>(kModeCtorHookAddrs[0]),
        v0[0], v0[1], v0[2], v0[3], v0[4], v0[5], v0[6],
        static_cast<unsigned long>(kModeCtorHookAddrs[1]),
        v1[0], v1[1], v1[2], v1[3], v1[4], v1[5], v1[6]);
}

void RestoreModeCtorHookBytes()
{
    if (!g_modeCtorHookSavedValid)
    {
        mod::Log("RestoreModeCtorHookBytes: no saved bytes — skipped");
        return;
    }

    for (size_t i = 0; i < kModeCtorHookCount; ++i)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                            kModeCtorHookPatchSize,
                            PAGE_EXECUTE_READWRITE,
                            &oldProtect))
        {
            mod::Log(
                "RestoreModeCtorHookBytes: VirtualProtect(0x%08lX) failed err=%lu",
                static_cast<unsigned long>(kModeCtorHookAddrs[i]),
                static_cast<unsigned long>(GetLastError()));
            continue;
        }

        memcpy(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
               g_modeCtorHookSaved[i],
               kModeCtorHookPatchSize);

        VirtualProtect(reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                       kModeCtorHookPatchSize,
                       oldProtect,
                       &oldProtect);
        FlushInstructionCache(GetCurrentProcess(),
                              reinterpret_cast<void*>(kModeCtorHookAddrs[i]),
                              kModeCtorHookPatchSize);
    }

    g_modeCtorHookSavedValid = false;

    // Read back for verification.
    uint8_t v0[kModeCtorHookPatchSize] = {};
    uint8_t v1[kModeCtorHookPatchSize] = {};
    memcpy(v0, reinterpret_cast<const void*>(kModeCtorHookAddrs[0]), kModeCtorHookPatchSize);
    memcpy(v1, reinterpret_cast<const void*>(kModeCtorHookAddrs[1]), kModeCtorHookPatchSize);
    mod::Log(
        "RestoreModeCtorHookBytes: restored 0x%08lX=[%02X %02X %02X %02X %02X %02X %02X] "
        "0x%08lX=[%02X %02X %02X %02X %02X %02X %02X]",
        static_cast<unsigned long>(kModeCtorHookAddrs[0]),
        v0[0], v0[1], v0[2], v0[3], v0[4], v0[5], v0[6],
        static_cast<unsigned long>(kModeCtorHookAddrs[1]),
        v1[0], v1[1], v1[2], v1[3], v1[4], v1[5], v1[6]);
}

// ---------------------------------------------------------------------------
// Mode-constructor trampoline fixup
//
// Revival DLL's mode constructors (online=sub_10073440, spectator=sub_100749A0,
// tournament=sub_100792D0) hook EXE addresses 0x763E50 and 0x763F04 via
// sub_1006F160.  The trampoline installer (sub_1006F060) copies the original
// bytes into a malloc'd trampoline WITHOUT fixing up relative instructions
// (E8 CALL rel32, E9 JMP rel32, 0F 8x near Jcc rel32).
//
// On re-initialization, new trampolines are allocated at different heap
// addresses.  Any relative instruction in the copied bytes computes its target
// as: trampoline_addr + offset + insn_length + original_displacement.  Since
// the original displacement was calculated for the EXE location (not the
// trampoline), the computed target is wrong by (trampoline_addr - exe_addr) →
// wild EIP crash (0x210132F9, 0x22FFF281, 0x2702FBB9, etc.).
//
// Trampoline layout from sub_1006F060 for a N-byte hook:
//   +0: 0x60 PUSHAD
//   +1: 0x9C PUSHFD
//   +2: 0xE8 rel32 → CALL callback
//   +7: 0x9D POPFD
//   +8: 0x61 POPAD
//   +9: [N bytes copied from original hook site]
//   +9+N: 0xE9 rel32 → JMP back to original+N
// ---------------------------------------------------------------------------
struct ModeCtorHookSite {
    uintptr_t hookAddr;
    size_t    patchSize;
};

static constexpr ModeCtorHookSite kModeCtorHookSites[] = {
    { 0x763E50, 7 },
    { 0x763F04, 7 },
};

static constexpr size_t kModeCtorTrampolinePrologueSize = 9; // PUSHAD+PUSHFD+CALL+POPFD+POPAD

// Track trampoline addresses we've already fixed up, per hook site.
// If the same trampoline is seen again, its displacements are already
// relative to the trampoline — re-fixing would drift the targets
// further with each call, eventually causing a wild-EIP crash.
static uintptr_t g_lastFixedTrampoline[sizeof(kModeCtorHookSites) /
                                        sizeof(kModeCtorHookSites[0])] = {};

void ResetModeConstructorTrampolineCache()
{
    memset(g_lastFixedTrampoline, 0, sizeof(g_lastFixedTrampoline));
    mod::Log("ResetModeConstructorTrampolineCache: cleared %zu entries",
             sizeof(g_lastFixedTrampoline) / sizeof(g_lastFixedTrampoline[0]));
}

void FixupModeConstructorTrampolines(const char* caller)
{
    for (size_t siteIdx = 0;
         siteIdx < sizeof(kModeCtorHookSites) / sizeof(kModeCtorHookSites[0]);
         ++siteIdx)
    {
        const auto& site = kModeCtorHookSites[siteIdx];
        // Check if the hook site starts with E9 (JMP near rel32).
        uint8_t firstByte = 0;
        if (!SafeReadByte(reinterpret_cast<uint8_t*>(site.hookAddr), &firstByte)
            || firstByte != 0xE9)
        {
            mod::Log(
                "%s: modeCtorFixup 0x%08lX not hooked (byte=0x%02X), skip",
                caller,
                static_cast<unsigned long>(site.hookAddr),
                static_cast<unsigned>(firstByte));
            continue;
        }

        // Read the E9 displacement to find the trampoline address.
        int32_t jmpDisp = 0;
        memcpy(&jmpDisp, reinterpret_cast<const void*>(site.hookAddr + 1), 4);
        const uintptr_t trampAddr = site.hookAddr + 5 + jmpDisp;

        // If we already fixed this trampoline, its displacements are already
        // correct (relative to the trampoline).  Re-fixing would interpret
        // them as relative to the EXE hook site, drifting the target each
        // call until it becomes a wild EIP.
        if (trampAddr == g_lastFixedTrampoline[siteIdx])
        {
            mod::Log(
                "%s: modeCtorFixup 0x%08lX tramp=%p already fixed, skip",
                caller,
                static_cast<unsigned long>(site.hookAddr),
                reinterpret_cast<void*>(trampAddr));
            continue;
        }

        // Verify trampoline prologue: must be PUSHAD (0x60) PUSHFD (0x9C).
        uint8_t prologue[2] = {};
        memcpy(prologue, reinterpret_cast<const void*>(trampAddr), 2);
        if (prologue[0] != 0x60 || prologue[1] != 0x9C)
        {
            mod::Log(
                "%s: modeCtorFixup 0x%08lX tramp=%p bad prologue [%02X %02X], skip",
                caller,
                static_cast<unsigned long>(site.hookAddr),
                reinterpret_cast<void*>(trampAddr),
                prologue[0], prologue[1]);
            continue;
        }

        // Read the original bytes from trampoline + prologue offset.
        const size_t origOff = kModeCtorTrampolinePrologueSize;
        uint8_t origBytes[16] = {};
        memcpy(origBytes,
               reinterpret_cast<const void*>(trampAddr + origOff),
               site.patchSize);

        mod::Log(
            "%s: modeCtorFixup 0x%08lX tramp=%p orig=[%02X %02X %02X %02X %02X %02X %02X]",
            caller,
            static_cast<unsigned long>(site.hookAddr),
            reinterpret_cast<void*>(trampAddr),
            origBytes[0], origBytes[1], origBytes[2], origBytes[3],
            origBytes[4], origBytes[5], origBytes[6]);

        bool anyFixed = false;

        // Scan for E8 (CALL rel32) and E9 (JMP rel32) — 5-byte instructions.
        for (size_t i = 0; i + 5 <= site.patchSize; ++i)
        {
            if (origBytes[i] != 0xE8 && origBytes[i] != 0xE9)
                continue;

            int32_t origDisp = 0;
            memcpy(&origDisp, &origBytes[i + 1], 4);

            // Absolute target the original instruction was meant to reach.
            const uintptr_t origInsnAddr = site.hookAddr + i;
            const uintptr_t absTarget = origInsnAddr + 5 +
                                        static_cast<uintptr_t>(static_cast<uint32_t>(origDisp));

            // Correct displacement from the trampoline location.
            const uintptr_t newInsnAddr = trampAddr + origOff + i;
            const int32_t newDisp =
                static_cast<int32_t>(absTarget - (newInsnAddr + 5));

            // Write the fixed-up displacement.
            DWORD oldProtect = 0;
            VirtualProtect(reinterpret_cast<void*>(newInsnAddr + 1), 4,
                           PAGE_EXECUTE_READWRITE, &oldProtect);
            memcpy(reinterpret_cast<void*>(newInsnAddr + 1), &newDisp, 4);
            VirtualProtect(reinterpret_cast<void*>(newInsnAddr + 1), 4,
                           oldProtect, &oldProtect);

            mod::Log(
                "%s: modeCtorFixup 0x%08lX fixed E%X at +%zu: "
                "origDisp=0x%08X newDisp=0x%08X absTarget=%p",
                caller,
                static_cast<unsigned long>(site.hookAddr),
                static_cast<unsigned>(origBytes[i]),
                i,
                static_cast<unsigned>(origDisp),
                static_cast<unsigned>(newDisp),
                reinterpret_cast<void*>(absTarget));

            anyFixed = true;
            i += 4; // skip displacement bytes
        }

        // Scan for 0F 8x (near conditional JMP rel32) — 6-byte instructions.
        for (size_t i = 0; i + 6 <= site.patchSize; ++i)
        {
            if (origBytes[i] != 0x0F || (origBytes[i + 1] & 0xF0) != 0x80)
                continue;

            int32_t origDisp = 0;
            memcpy(&origDisp, &origBytes[i + 2], 4);

            const uintptr_t origInsnAddr = site.hookAddr + i;
            const uintptr_t absTarget = origInsnAddr + 6 +
                                        static_cast<uintptr_t>(static_cast<uint32_t>(origDisp));

            const uintptr_t newInsnAddr = trampAddr + origOff + i;
            const int32_t newDisp =
                static_cast<int32_t>(absTarget - (newInsnAddr + 6));

            DWORD oldProtect = 0;
            VirtualProtect(reinterpret_cast<void*>(newInsnAddr + 2), 4,
                           PAGE_EXECUTE_READWRITE, &oldProtect);
            memcpy(reinterpret_cast<void*>(newInsnAddr + 2), &newDisp, 4);
            VirtualProtect(reinterpret_cast<void*>(newInsnAddr + 2), 4,
                           oldProtect, &oldProtect);

            mod::Log(
                "%s: modeCtorFixup 0x%08lX fixed 0F %02X at +%zu: "
                "origDisp=0x%08X newDisp=0x%08X absTarget=%p",
                caller,
                static_cast<unsigned long>(site.hookAddr),
                static_cast<unsigned>(origBytes[i + 1]),
                i,
                static_cast<unsigned>(origDisp),
                static_cast<unsigned>(newDisp),
                reinterpret_cast<void*>(absTarget));

            anyFixed = true;
            i += 5;
        }

        // Remember this trampoline so we don't re-fix it on later calls.
        g_lastFixedTrampoline[siteIdx] = trampAddr;

        if (anyFixed)
        {
            FlushInstructionCache(
                GetCurrentProcess(),
                reinterpret_cast<void*>(trampAddr + origOff),
                site.patchSize);
        }
        else
        {
            mod::Log(
                "%s: modeCtorFixup 0x%08lX no relative instructions found",
                caller,
                static_cast<unsigned long>(site.hookAddr));
        }
    }
}

// g_frameRecoveryPending: set by RunFrameDispatch on longjmp, read and cleared
// by OurFrameDispatch outside the setjmp scope so C++ code (ForceLocalPlayInit)
// can run safely.
static volatile bool g_frameRecoveryPending = false;

// RunFrameDispatch — MSVC C4611 guard: no C++ objects with destructors in scope.
// Only POD types here.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4611)
#endif
static void RunFrameDispatch()
{
    g_netplayFrameJmpActive = true;
    if (setjmp(g_netplayFrameJmpBuf) != 0)
    {
        // longjmp path: ExitProcess was intercepted during this frame tick.
        // Signal OurFrameDispatch to execute C++ recovery outside setjmp scope.
        g_netplayFrameJmpActive = false;
        g_frameRecoveryPending = true;
        return;
    }
    // Normal path: dispatch through the original sub_1006E590 trampoline.
    if (g_origFrameDispatch != nullptr)
    {
        g_origFrameDispatch();
    }
    g_netplayFrameJmpActive = false;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---------------------------------------------------------------------------
// Per-frame tick hook on sub_1006E570 (RVA 0x6E570)
//
// BACKGROUND
// ----------
// sub_1006E590 (RVA 0x6E590) runs ONCE during DLL init.  Among other things
// it patches EXE address 0x401642 to JMP into sub_1006E570.  From that point
// the EXE's main loop calls sub_1006E570 on EVERY frame.
//
// sub_1006E570 is __thiscall — ECX comes from the EXE.  It reads the session
// pointer from dword_100A02CC, looks up vtable[2], and calls it PASSING ECX
// (the EXE-supplied this) as the first argument:
//
//   return (vtable[2])(this);    // this = ECX from EXE
//
// vtable[2] (typically EFZ_Main_RollbackLoopTick) treats that argument as
// the session's `self` pointer.  During a normal match this is correct:
// InvokeStartInitPlayer writes the session pointer into an EXE global that
// feeds ECX.  But ForceLocalPlayInit replaces the DLL-side session pointer
// without updating the EXE global, so ECX carries the OLD (freed) session
// address → crash.
//
// FIX
// ---
// We hook sub_1006E570 and replace ECX with the current value of
// dword_100A02CC before calling the original.  This guarantees the
// correct session pointer reaches vtable[2] regardless of what the EXE
// passes.
// ---------------------------------------------------------------------------

static constexpr uintptr_t kPerFrameTickRva = 0x6E570u;
static uint8_t g_perFrameTickTrampoline[12] = {};
static bool    g_perFrameTickInstalled      = false;
static bool    g_perFrameMismatchLogged     = false;

// Guard flag: true while g_origPerFrameTick is running.  ForceLocalPlayInit
// must NOT run during this window because sub_1006E570 -> vtable[2] ->
// RollbackLoopTick is using the current session as 'this'. Destroying it
// mid-tick causes a use-after-free crash in SetEvent(this[2]).
static volatile bool g_insideFrameTick = false;
static volatile bool g_deferredCancelCleanup = false;

// __thiscall trampoline: ECX = this, no other args.
using PerFrameTickFn = int (__thiscall *)(void* thisPtr);
static PerFrameTickFn g_origPerFrameTick = nullptr;

static uint32_t g_frameTick = 0;           // monotonic per-frame counter

// Read the raw session pointer from dword_100A02CC without heuristic
// validation.  Used in the per-frame tick hot path.
static uintptr_t ReadSessionPtrRaw()
{
    if (g_activeRevival == nullptr || g_activeRevival->sessionPtrOffsetCount == 0)
        return 0;
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
        return 0;
    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);
    uintptr_t sessionPtr = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(base + g_activeRevival->sessionPtrOffsets[0]),
        &sessionPtr);
    return sessionPtr;
}

// Screen-index change monitor — logs every time byte_790148 transitions.
static uint8_t g_lastMonitoredScreenIndex = 0xFF;

static void MonitorScreenIndexChange()
{
    constexpr uintptr_t kScreenIndexAddr = 0x00790148;
    constexpr uintptr_t kScreenTableAddr = 0x00790110;
    constexpr uint32_t kOffsetGameSystem = 0x1C;
    constexpr uint32_t kModeOffset = 4964;

    uint8_t currentIdx = 0xFF;
    uint8_t gameMode = 0xFF;
    uint8_t secondaryMode = 0xFF;
    uint32_t screenObj = 0;
    uint8_t csInit = 0xFF, csExit = 0xFF;
    uint32_t csObj = 0;

    __try
    {
        currentIdx = *reinterpret_cast<const uint8_t*>(kScreenIndexAddr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (currentIdx == g_lastMonitoredScreenIndex)
        return;

    __try
    {
        if (currentIdx < 16)
        {
            screenObj = reinterpret_cast<const uint32_t*>(kScreenTableAddr)[currentIdx];
            if (screenObj != 0)
            {
                const uint32_t gameSys =
                    *reinterpret_cast<const uint32_t*>(screenObj + kOffsetGameSystem);
                if (gameSys != 0)
                {
                    gameMode = *reinterpret_cast<const uint8_t*>(gameSys + kModeOffset);
                    secondaryMode = *reinterpret_cast<const uint8_t*>(gameSys + kModeOffset + 1);
                }
            }
        }

        csObj = reinterpret_cast<const uint32_t*>(kScreenTableAddr)[1];
        if (csObj != 0)
        {
            csInit = *reinterpret_cast<const uint8_t*>(csObj + 44);
            csExit = *reinterpret_cast<const uint8_t*>(csObj + 45);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    mod::Log(
        "SCREEN_MONITOR: index %u -> %u tick=%u mode=%u/%u "
        "screenObj=0x%08lX charselect(init=%u exit=%u obj=0x%08lX)",
        static_cast<unsigned>(g_lastMonitoredScreenIndex),
        static_cast<unsigned>(currentIdx),
        g_frameTick,
        static_cast<unsigned>(gameMode),
        static_cast<unsigned>(secondaryMode),
        static_cast<unsigned long>(screenObj),
        static_cast<unsigned>(csInit),
        static_cast<unsigned>(csExit),
        static_cast<unsigned long>(csObj));

    g_lastMonitoredScreenIndex = currentIdx;
}

// Our per-frame tick hook.  Uses __fastcall to capture ECX (first arg) and
// EDX (second, unused).  Replaces ECX with the current session pointer
// before calling the original sub_1006E570.
static int __fastcall OurPerFrameTickHook(void* exeThis, void* /*edx*/)
{
    ++g_frameTick;
    MonitorScreenIndexChange();

    const uintptr_t exeThisAddr = reinterpret_cast<uintptr_t>(exeThis);
    const uintptr_t currentSession = ReadSessionPtrRaw();

    // Use the current DLL session if available; fall back to EXE's value.
    void* fixedThis = (currentSession != 0)
        ? reinterpret_cast<void*>(currentSession)
        : exeThis;

    // Detect and log the first ECX mismatch (stale session pointer).
    if (exeThisAddr != currentSession && !g_perFrameMismatchLogged)
    {
        g_perFrameMismatchLogged = true;
        mod::Log(
            "TICK_HOOK: *** ECX MISMATCH *** frameTick=%u "
            "exeECX=0x%08lX dllSession=0x%08lX — "
            "overriding ECX with current session",
            g_frameTick,
            static_cast<unsigned long>(exeThisAddr),
            static_cast<unsigned long>(currentSession));

        // Dump session vtable for context.
        if (currentSession != 0)
        {
            uintptr_t vtable = 0;
            (void)SafeReadPtr(
                reinterpret_cast<const void*>(currentSession), &vtable);
            HMODULE revival = GetModuleHandleA("EfzRevival.dll");
            const uintptr_t revBase = revival
                ? reinterpret_cast<uintptr_t>(revival) : 0;
            mod::Log(
                "TICK_HOOK: current session vtable=0x%08lX (RVA=0x%lX)",
                static_cast<unsigned long>(vtable),
                static_cast<unsigned long>(
                    revBase != 0 ? vtable - revBase : 0));
        }

        LogSessionDiagnosticState("tick_ecx_mismatch");
    }

    // Periodic heartbeat every 600 frames (~10s at 60fps).
    if (g_frameTick == 1
        || (g_frameTick % 600 == 0 && g_frameTick <= 6000))
    {
        mod::Log(
            "TICK_HOOK: heartbeat frameTick=%u session=0x%08lX exeECX=0x%08lX match=%d",
            g_frameTick,
            static_cast<unsigned long>(currentSession),
            static_cast<unsigned long>(exeThisAddr),
            (exeThisAddr == currentSession) ? 1 : 0);
    }

    // Call the original sub_1006E570 with the corrected ECX.
    // Set the guard flag so ForceLocalPlayInit knows we're mid-tick.
    g_insideFrameTick = true;
    const int result = g_origPerFrameTick(fixedThis);
    g_insideFrameTick = false;

    // If CancelSession tried to run ForceLocalPlayInit while we were inside
    // the tick (which would destroy the session that RollbackLoopTick was
    // using as 'this'), it deferred the work.  Execute it now that the tick
    // has completed safely.
    if (g_deferredCancelCleanup)
    {
        g_deferredCancelCleanup = false;
        mod::Log(
            "TICK_HOOK: executing deferred cancel cleanup "
            "(ForceLocalPlayInit was unsafe mid-tick)");

        const bool initOk = ForceLocalPlayInit();
        mod::Log(
            "TICK_HOOK: deferred ForceLocalPlayInit result=%d",
            initOk ? 1 : 0);

        (void)ClearRevivalText();
        (void)RestoreRenderContext();
        (void)DisableRevivalTextRendering();
        mod::ResetCrashRecoveryState();
        ResetGameModeValidation();

        mod::Log("TICK_HOOK: deferred cancel cleanup complete");
    }

    return result;
}

// Returns true while inside the per-frame tick (g_origPerFrameTick running).
bool IsInsideFrameTick()
{
    return g_insideFrameTick;
}

// Request deferred cancel cleanup after the frame tick returns.
void RequestDeferredCancelCleanup()
{
    g_deferredCancelCleanup = true;
}

// Reset the per-frame validator state.  Called when a session ends so the
// next session gets fresh validation.
void ResetGameModeValidation()
{
    g_frameTick = 0;
    g_perFrameMismatchLogged = false;

    // Clear stale deferred-cleanup flags from a previous session.
    // If g_deferredCancelCleanup persists into session 2, the first
    // frame tick would run ForceLocalPlayInit and destroy the new session.
    // If g_frameRecoveryPending persists, OurFrameDispatch would run
    // the full ExitProcess recovery path on the wrong session.
    if (g_deferredCancelCleanup)
    {
        mod::Log("ResetGameModeValidation: clearing stale g_deferredCancelCleanup");
        g_deferredCancelCleanup = false;
    }
    if (g_frameRecoveryPending)
    {
        mod::Log("ResetGameModeValidation: clearing stale g_frameRecoveryPending");
        g_frameRecoveryPending = false;
    }
}

// OurFrameDispatch — entry point patched over sub_1006E590's prologue.
//
// On the normal path, delegates to RunFrameDispatch (→ trampoline → original).
//
// On the ExitProcess interception path (peer process died during rollback tick):
//   1. RunFrameDispatch returns with g_frameRecoveryPending = true.
//   2. Full reverse-init cleanup: reinstate local-play session, terminate the
//      dead helper process, restore DLL patches, disable text overlays, and
//      reset the VEH crash-recovery guard.
//   3. Force game mode to 0 (title screen) so HookedTitleUpdateImplBody can
//      consume the exit-interception flag and re-enter the netplay menu
//      immediately instead of waiting for the match to end naturally.
static void OurFrameDispatch()
{
    RunFrameDispatch();

    if (g_frameRecoveryPending)
    {
        g_frameRecoveryPending = false;

        // ---- Full reverse-init recovery ------------------------------------
        // The online/spectate session tick fired ExitProcess (peer died).
        // NeutralizeExitProcess has already:
        //   - captured g_revivalExitMode = g_localRoleFlag
        //   - set g_revivalExitIntercepted = 1
        //   - neutralised the session vtable (all slots → no-op stubs)
        //   - longjmp'd back here via g_netplayFrameJmpBuf
        //
        // Step 1: Reinstate a live local-play session immediately so the
        // game loop gets a valid vtable for subsequent frame dispatches.
        // --------------------------------------------------------------------
        const int recoveredRole = g_localRoleFlag;
        const DWORD recoveredPid = g_revivalProcessId;
        LogSessionDiagnosticState("OurFrameDispatch_recovery_entry");
        mod::Log(
            "OurFrameDispatch: ExitProcess intercepted during frame tick "
            "(role=%d pid=%lu) — performing full cleanup",
            recoveredRole,
            static_cast<unsigned long>(recoveredPid));

        // Step 1: Reinstate a live local-play session.
        const bool initOk = ForceLocalPlayInit();
        mod::Log(
            "OurFrameDispatch: step 1 ForceLocalPlayInit result=%d",
            initOk ? 1 : 0);

        // Step 2: Terminate the dead helper process and close its handle.
        // The session is already neutralised; terminating ensures OS
        // resources are released immediately.
        if (g_revivalProcess != nullptr)
        {
            const BOOL termOk = TerminateProcess(g_revivalProcess, 0);
            const DWORD termErr = termOk ? 0 : GetLastError();
            CloseHandle(g_revivalProcess);
            g_revivalProcess = nullptr;
            g_revivalProcessId = 0;
            mod::Log(
                "OurFrameDispatch: step 2 helper process terminated "
                "(pid=%lu termOk=%d err=%lu) and handle closed",
                static_cast<unsigned long>(recoveredPid),
                termOk ? 1 : 0,
                static_cast<unsigned long>(termErr));
        }
        else
        {
            mod::Log("OurFrameDispatch: step 2 skipped (no helper process handle)");
        }

        // Step 3: Restore DLL Jcc patches that made ExitProcess call-sites
        // unreachable.  No longer needed now that the session is local-play.
        const bool patchOk = RestoreDllExitProcessPatches();
        mod::Log(
            "OurFrameDispatch: step 3 RestoreDllExitProcessPatches result=%d",
            patchOk ? 1 : 0);

        // Step 4: Disable stale text overlays left by the online session
        // (nicknames, ping, delay).  ClearRevivalText is unsafe here
        // (ForceLocalPlayInit may have zeroed the render context pointer)
        // so we only disable the rendering hook.
        const bool textOk = DisableRevivalTextRendering();
        mod::Log(
            "OurFrameDispatch: step 4 DisableRevivalTextRendering result=%d",
            textOk ? 1 : 0);

        // Step 5: Reset the one-shot VEH TOCTOU recovery guard so a
        // subsequent session can still be recovered if needed.
        mod::ResetCrashRecoveryState();
        ResetGameModeValidation();
        mod::Log("OurFrameDispatch: step 5 crash/validation state reset");

        // Step 6: Force game mode to 0 (title screen).  On the next main-
        // loop iteration, HookedTitleUpdateImplBody runs, detects
        // g_revivalExitIntercepted, calls ConsumeRevivalExitInterception,
        // and re-enters the netplay menu — skipping the rest of the match.
        const bool modeOk = ForceGameModeToTitle();
        mod::Log(
            "OurFrameDispatch: step 6 ForceGameModeToTitle result=%d",
            modeOk ? 1 : 0);

        g_localInitAppliedForSession = false;
        mod::Log(
            "OurFrameDispatch: full recovery complete (was role=%d), "
            "next title-screen frame will consume exit interception",
            recoveredRole);
        LogSessionDiagnosticState("OurFrameDispatch_recovery_exit");
    }
}

bool InstallNetplayFrameHook()
{
    if (g_frameHookInstalled)
    {
        return true;
    }

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr)
    {
        mod::Log("InstallNetplayFrameHook: EfzRevival.dll not loaded");
        return false;
    }

    const uintptr_t base    = reinterpret_cast<uintptr_t>(revival);
    uint8_t* const  target  = reinterpret_cast<uint8_t*>(base + kFrameHookRva);

    // Sanity-check: expect push ebp (0x55) as the first byte.
    uint8_t firstByte = 0;
    if (!SafeReadByte(target, &firstByte) || firstByte != 0x55)
    {
        mod::Log(
            "InstallNetplayFrameHook: unexpected byte 0x%02X at RVA 0x%lX — skipping",
            static_cast<unsigned>(firstByte),
            static_cast<unsigned long>(kFrameHookRva));
        return false;
    }

    // Build trampoline: 6 original bytes + JMP-near back to original+6.
    memcpy(g_frameHookTrampoline, target, 6);
    const uintptr_t origContinue = base + kFrameHookRva + 6;
    g_frameHookTrampoline[6]     = 0xE9; // JMP near rel32
    const uintptr_t jmpFrom      = reinterpret_cast<uintptr_t>(&g_frameHookTrampoline[6]) + 5;
    *reinterpret_cast<int32_t*>(&g_frameHookTrampoline[7]) =
        static_cast<int32_t>(origContinue - jmpFrom);
    g_frameHookTrampoline[11] = 0x90; // padding NOP

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            g_frameHookTrampoline,
            sizeof(g_frameHookTrampoline),
            PAGE_EXECUTE_READWRITE,
            &oldProtect))
    {
        mod::Log("InstallNetplayFrameHook: VirtualProtect(trampoline) failed");
        return false;
    }

    g_origFrameDispatch = reinterpret_cast<FrameDispatchFn>(
        reinterpret_cast<void*>(g_frameHookTrampoline));

    // Patch the first 6 bytes of sub_1006E590:
    //   E9 rel32 (5-byte JMP to OurFrameDispatch) + 90 (NOP).
    uint8_t patch[6];
    patch[0] = 0xE9;
    *reinterpret_cast<int32_t*>(&patch[1]) =
        static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&OurFrameDispatch)
            - (reinterpret_cast<uintptr_t>(target) + 5));
    patch[5] = 0x90;

    if (!VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        mod::Log("InstallNetplayFrameHook: VirtualProtect(target) failed");
        return false;
    }
    memcpy(target, patch, 6);
    VirtualProtect(target, 6, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, 6);

    g_frameHookInstalled = true;
    mod::Log(
        "InstallNetplayFrameHook: installed at DLL RVA 0x%lX, trampoline at %p",
        static_cast<unsigned long>(kFrameHookRva),
        static_cast<void*>(g_frameHookTrampoline));

    // -----------------------------------------------------------------------
    // Per-frame tick hook on sub_1006E570 (RVA 0x6E570).
    //
    // sub_1006E570 is the ACTUAL per-frame entry point.  sub_1006E590 (hooked
    // above) runs once during init and installs an EXE patch at 0x401642 that
    // calls sub_1006E570 every frame.  We hook sub_1006E570 to:
    //   (a) Fix the stale ECX that the EXE passes after session replacement.
    //   (b) Run per-frame diagnostic heartbeats.
    //
    // sub_1006E570 prologue — may start with either:
    //   55        push ebp       (standard frame-pointer prologue)
    //   51        push ecx       (__thiscall saving this)
    // We overwrite the first 6 bytes with a 5-byte JMP + NOP.
    // -----------------------------------------------------------------------
    if (!g_perFrameTickInstalled)
    {
        uint8_t* const tickTarget =
            reinterpret_cast<uint8_t*>(base + kPerFrameTickRva);

        uint8_t tickFirstByte = 0;
        if (!SafeReadByte(tickTarget, &tickFirstByte)
            || (tickFirstByte != 0x55 && tickFirstByte != 0x51))
        {
            mod::Log(
                "InstallNetplayFrameHook: per-frame tick unexpected byte "
                "0x%02X at RVA 0x%lX — skipping",
                static_cast<unsigned>(tickFirstByte),
                static_cast<unsigned long>(kPerFrameTickRva));
        }
        else
        {
            // Build trampoline: 6 original bytes + JMP-near back to orig+6.
            //
            // IMPORTANT: the stolen bytes contain a relative E8 CALL at
            // offset 1 (bytes: 51 E8 xx xx xx xx).  A raw memcpy would
            // preserve the displacement that was correct at the *original*
            // address but wrong at the trampoline address.  We must fix
            // up the displacement so the CALL reaches the same absolute
            // target from the new location.
            memcpy(g_perFrameTickTrampoline, tickTarget, 6);

            // Fix up the relative CALL at trampoline[1] if present.
            if (g_perFrameTickTrampoline[1] == 0xE8
                || g_perFrameTickTrampoline[1] == 0xE9)
            {
                // Original call/jmp displacement (little-endian int32).
                int32_t origDisp = 0;
                memcpy(&origDisp, &tickTarget[2], sizeof(origDisp));

                // Absolute target the original instruction reached.
                const uintptr_t origInsnAddr =
                    reinterpret_cast<uintptr_t>(tickTarget) + 1; // addr of E8
                const uintptr_t absTarget = origInsnAddr + 5 + origDisp;

                // New displacement from trampoline location.
                const uintptr_t newInsnAddr =
                    reinterpret_cast<uintptr_t>(&g_perFrameTickTrampoline[1]);
                const int32_t newDisp =
                    static_cast<int32_t>(absTarget - (newInsnAddr + 5));
                memcpy(&g_perFrameTickTrampoline[2], &newDisp, sizeof(newDisp));

                mod::Log(
                    "InstallNetplayFrameHook: fixed up trampoline E8/E9 at "
                    "+1: origDisp=0x%08X newDisp=0x%08X absTarget=%p",
                    static_cast<unsigned>(origDisp),
                    static_cast<unsigned>(newDisp),
                    reinterpret_cast<void*>(absTarget));
            }

            const uintptr_t tickContinue = base + kPerFrameTickRva + 6;
            g_perFrameTickTrampoline[6] = 0xE9;
            const uintptr_t tickJmpFrom =
                reinterpret_cast<uintptr_t>(&g_perFrameTickTrampoline[6]) + 5;
            *reinterpret_cast<int32_t*>(&g_perFrameTickTrampoline[7]) =
                static_cast<int32_t>(tickContinue - tickJmpFrom);
            g_perFrameTickTrampoline[11] = 0x90;

            DWORD tickTrampolineProtect = 0;
            if (!VirtualProtect(
                    g_perFrameTickTrampoline,
                    sizeof(g_perFrameTickTrampoline),
                    PAGE_EXECUTE_READWRITE,
                    &tickTrampolineProtect))
            {
                mod::Log(
                    "InstallNetplayFrameHook: VirtualProtect(tick trampoline) failed");
            }
            else
            {
                g_origPerFrameTick = reinterpret_cast<PerFrameTickFn>(
                    reinterpret_cast<void*>(g_perFrameTickTrampoline));

                // Patch sub_1006E570 prologue: JMP to OurPerFrameTickHook.
                uint8_t tickPatch[6];
                tickPatch[0] = 0xE9;
                *reinterpret_cast<int32_t*>(&tickPatch[1]) =
                    static_cast<int32_t>(
                        reinterpret_cast<uintptr_t>(&OurPerFrameTickHook)
                        - (reinterpret_cast<uintptr_t>(tickTarget) + 5));
                tickPatch[5] = 0x90;

                DWORD tickTargetProtect = 0;
                if (!VirtualProtect(
                        tickTarget, 6, PAGE_EXECUTE_READWRITE, &tickTargetProtect))
                {
                    mod::Log(
                        "InstallNetplayFrameHook: VirtualProtect(tick target) failed");
                }
                else
                {
                    memcpy(tickTarget, tickPatch, 6);
                    VirtualProtect(tickTarget, 6, tickTargetProtect, &tickTargetProtect);
                    FlushInstructionCache(GetCurrentProcess(), tickTarget, 6);

                    g_perFrameTickInstalled = true;
                    mod::Log(
                        "InstallNetplayFrameHook: per-frame tick hook installed "
                        "at DLL RVA 0x%lX, trampoline at %p",
                        static_cast<unsigned long>(kPerFrameTickRva),
                        static_cast<void*>(g_perFrameTickTrampoline));
                }
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// ForceGameModeToTitle — write 0 to the EFZ.exe game-mode index so the
// next main-loop iteration dispatches to the title-screen update, where
// HookedTitleUpdateImplBody can run ConsumeRevivalExitInterception and
// re-enter the netplay menu.
//
// Deliberately minimal: called from the VEH crash handler where complex
// operations (allocations, locks, deep call chains) are unsafe.
// ---------------------------------------------------------------------------
bool ForceGameModeToTitle()
{
    if (g_activeRevival == nullptr || g_activeRevival->addrGameModeCurrentIndex == 0)
    {
        mod::Log("ForceGameModeToTitle: no active Revival profile or game mode address");
        return false;
    }

    int currentGameMode = -1;
    if (!SafeReadInt(
            reinterpret_cast<const void*>(g_activeRevival->addrGameModeCurrentIndex),
            &currentGameMode))
    {
        mod::Log(
            "ForceGameModeToTitle: failed to read game mode at 0x%08lX",
            static_cast<unsigned long>(g_activeRevival->addrGameModeCurrentIndex));
        return false;
    }

    if (currentGameMode == 0)
    {
        mod::Log("ForceGameModeToTitle: already on title screen (mode=0), no-op");
        return true;
    }

    auto* const modePtr = reinterpret_cast<int*>(
        g_activeRevival->addrGameModeCurrentIndex);

    DWORD oldProtect = 0;
    if (!VirtualProtect(modePtr, sizeof(int), PAGE_READWRITE, &oldProtect))
    {
        mod::Log(
            "ForceGameModeToTitle: VirtualProtect failed addr=0x%08lX err=%lu",
            static_cast<unsigned long>(g_activeRevival->addrGameModeCurrentIndex),
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    *modePtr = 0;
    DWORD ignored = 0;
    (void)VirtualProtect(modePtr, sizeof(int), oldProtect, &ignored);

    mod::Log(
        "ForceGameModeToTitle: game mode %d -> 0 (title screen)",
        currentGameMode);
    return true;
}

// ---------------------------------------------------------------------------
// Diagnostic logging — session lifecycle state dump
// ---------------------------------------------------------------------------

static int g_forceLocalPlayInitCount = 0;

void IncrementForceLocalPlayInitCount()
{
    ++g_forceLocalPlayInitCount;
}

int GetForceLocalPlayInitCount()
{
    return g_forceLocalPlayInitCount;
}

void ResetForceLocalPlayInitCount()
{
    g_forceLocalPlayInitCount = 0;
}

void LogSessionDiagnosticState(const char* context)
{
    const char* ctx = (context != nullptr) ? context : "unknown";

    // --- DLL patch state ---
    mod::Log(
        "DIAG[%s]: dllExitPatchesSaved=%d tournamentPatchesSaved=%d "
        "renderContextSaved=%d savedRenderCtx=0x%08lX",
        ctx,
        g_dllExitProcessPatchesSaved ? 1 : 0,
        g_tournamentPatchesSaved ? 1 : 0,
        g_renderContextSaved ? 1 : 0,
        static_cast<unsigned long>(g_savedRenderContext));

    // --- Revival DLL globals ---
    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival != nullptr && g_activeRevival != nullptr)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(revival);

        // Role flag
        int roleFlag = -1;
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->roleFlagOffsets[0]),
            &roleFlag);

        // Session pointer
        uintptr_t sessionPtr = 0;
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->sessionPtrOffsets[0]),
            &sessionPtr);

        // Render context
        uintptr_t renderCtx = 0;
        if (g_activeRevival->renderContextGlobalOffset != 0)
        {
            (void)SafeReadPtr(
                reinterpret_cast<const void*>(base + g_activeRevival->renderContextGlobalOffset),
                &renderCtx);
        }

        // Global state
        uintptr_t globalState = 0;
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->globalStatePtrOffset),
            &globalState);

        mod::Log(
            "DIAG[%s]: dllBase=0x%08lX roleFlag=%d sessionPtr=0x%08lX "
            "renderCtx=0x%08lX globalState=0x%08lX",
            ctx,
            static_cast<unsigned long>(base),
            roleFlag,
            static_cast<unsigned long>(sessionPtr),
            static_cast<unsigned long>(renderCtx),
            static_cast<unsigned long>(globalState));

        // Session vtable validation
        if (sessionPtr != 0 && sessionPtr >= 0x00100000u)
        {
            uintptr_t vtable = 0;
            int initComplete = -1;
            int activePlayer = -1;
            (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable);
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInitComplete),
                &initComplete);
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetActivePlayer),
                &activePlayer);

            mod::Log(
                "DIAG[%s]: session vtable=0x%08lX initComplete=%d activePlayer=%d",
                ctx,
                static_cast<unsigned long>(vtable),
                initComplete,
                activePlayer);
        }

        // ExitProcess patch site byte values (verify actual DLL code state)
        for (size_t i = 0; i < g_activeRevival->exitProcessPatchCount; ++i)
        {
            const uintptr_t rva = g_activeRevival->exitProcessPatchRva[i];
            if (rva == 0) continue;
            uint8_t currentByte = 0;
            (void)SafeReadByte(reinterpret_cast<const void*>(base + rva), &currentByte);
            mod::Log(
                "DIAG[%s]: exitPatch[%zu] RVA=0x%lX byte=0x%02X expected=0x%02X",
                ctx,
                i,
                static_cast<unsigned long>(rva),
                static_cast<unsigned>(currentByte),
                static_cast<unsigned>(g_activeRevival->exitProcessPatchOriginal[i]));
        }
        for (size_t i = 0; i < g_activeRevival->exitProcessNearJccCount; ++i)
        {
            const uintptr_t rva = g_activeRevival->exitProcessNearJccRva[i];
            if (rva == 0) continue;
            uint8_t b0 = 0, b1 = 0;
            (void)SafeReadByte(reinterpret_cast<const void*>(base + rva), &b0);
            (void)SafeReadByte(reinterpret_cast<const void*>(base + rva + 1), &b1);
            mod::Log(
                "DIAG[%s]: nearJccPatch[%zu] RVA=0x%lX bytes=%02X %02X",
                ctx,
                i,
                static_cast<unsigned long>(rva),
                static_cast<unsigned>(b0),
                static_cast<unsigned>(b1));
        }
    }
    else
    {
        mod::Log(
            "DIAG[%s]: EfzRevival.dll not loaded or no active profile",
            ctx);
    }

    // --- Takeover state ---
    mod::Log(
        "DIAG[%s]: localRoleFlag=%d localInitApplied=%d "
        "exitIntercepted=%ld exitMode=%ld frameHookInstalled=%d "
        "frameRecoveryPending=%d forceLocalPlayInitCount=%d",
        ctx,
        g_localRoleFlag,
        g_localInitAppliedForSession ? 1 : 0,
        static_cast<long>(InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0)),
        static_cast<long>(InterlockedCompareExchange(&g_revivalExitMode, 0, 0)),
        g_frameHookInstalled ? 1 : 0,
        g_frameRecoveryPending ? 1 : 0,
        g_forceLocalPlayInitCount);

    // --- Process state ---
    mod::Log(
        "DIAG[%s]: revivalProcess=0x%p revivalPid=%lu lastValidatedSession=0x%08lX "
        "lastSessionPtrOffset=0x%lX",
        ctx,
        static_cast<void*>(g_revivalProcess),
        static_cast<unsigned long>(g_revivalProcessId),
        static_cast<unsigned long>(g_lastValidatedSessionPtr),
        static_cast<unsigned long>(g_lastSessionPtrOffset));
}

// ---------------------------------------------------------------------------
// LogInitWriteSnapshot — comprehensive snapshot of every value init() writes.
//
// Captures ALL DLL globals and session object fields that init() modifies,
// in a format designed for before/after diff comparison.  Call this
// immediately before AND after every g_localInitFn() call to produce a
// complete write trace.  This covers all 5 crash hypotheses:
//
//   H1 (double init leak):  sessionPtr changes → old object leaked
//   H2 (stale render ctx):  initOnceGuard, renderCtx, renderCtxBase
//   H3 (patch state machine): exitPatch bytes (already in LogSessionDiagnosticState)
//   H4 (saved flag stale):  g_dllExitProcessPatchesSaved
//   H5 (history binding):   historyPrimary/SecondaryPtr, activePlayer
// ---------------------------------------------------------------------------
void LogInitWriteSnapshot(const char* context)
{
    const char* ctx = (context != nullptr) ? context : "unknown";

    HMODULE revival = GetModuleHandleA("EfzRevival.dll");
    if (revival == nullptr || g_activeRevival == nullptr)
    {
        mod::Log("INIT_SNAP[%s]: EfzRevival.dll not loaded or no profile", ctx);
        return;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(revival);

    // ---- DLL globals that init() writes directly ----

    // Role flag (dword_100A05D0)
    int roleFlag = -1;
    (void)SafeReadInt(
        reinterpret_cast<const void*>(base + g_activeRevival->roleFlagOffsets[0]),
        &roleFlag);

    // Session pointer (dword_100A02CC)
    uintptr_t sessionPtr = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(base + g_activeRevival->sessionPtrOffsets[0]),
        &sessionPtr);

    // Init flag (dword_100A05D4) — zeroed at top of init()
    int initFlag = -1;
    if (g_activeRevival->initFlagOffset != 0)
    {
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->initFlagOffset),
            &initFlag);
    }

    // Init byte (byte_100A0289) — zeroed after version check
    uint8_t initByte = 0xFF;
    if (g_activeRevival->initByteOffset != 0)
    {
        (void)SafeReadByte(
            reinterpret_cast<const void*>(base + g_activeRevival->initByteOffset),
            &initByte);
    }

    // Render context (dword_100A0778 = EfzRender*)
    uintptr_t renderCtx = 0;
    if (g_activeRevival->renderContextGlobalOffset != 0)
    {
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->renderContextGlobalOffset),
            &renderCtx);
    }

    // Render context base (dword_100A0760)
    uintptr_t renderCtxBase = 0;
    if (g_activeRevival->renderContextBaseOffset != 0)
    {
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->renderContextBaseOffset),
            &renderCtxBase);
    }

    // Init-once guard (word_100A0774) — critical for H2
    int initOnceGuard = -1;
    if (g_activeRevival->initOnceGuardOffset != 0)
    {
        // Read as int (the low byte is the guard; full word gives more context)
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->initOnceGuardOffset),
            &initOnceGuard);
    }

    // Timer pointer (dword_100A0764)
    uintptr_t timerPtr = 0;
    if (g_activeRevival->timerPtrOffset != 0)
    {
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->timerPtrOffset),
            &timerPtr);
    }

    // Global state pointer (dword_100A07B8)
    uintptr_t globalStatePtr = 0;
    (void)SafeReadPtr(
        reinterpret_cast<const void*>(base + g_activeRevival->globalStatePtrOffset),
        &globalStatePtr);

    mod::Log(
        "INIT_SNAP[%s]: roleFlag=%d sessionPtr=0x%08lX initFlag=%d initByte=0x%02X "
        "renderCtx=0x%08lX renderCtxBase=0x%08lX initOnceGuard=0x%08X "
        "timerPtr=0x%08lX globalStatePtr=0x%08lX",
        ctx,
        roleFlag,
        static_cast<unsigned long>(sessionPtr),
        initFlag,
        static_cast<unsigned>(initByte),
        static_cast<unsigned long>(renderCtx),
        static_cast<unsigned long>(renderCtxBase),
        static_cast<unsigned>(initOnceGuard),
        static_cast<unsigned long>(timerPtr),
        static_cast<unsigned long>(globalStatePtr));

    // ---- Secondary role flag / session pointer globals (detect stale copies) ----
    for (size_t i = 1; i < g_activeRevival->roleFlagOffsetCount; ++i)
    {
        int rf = -1;
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->roleFlagOffsets[i]),
            &rf);
        mod::Log(
            "INIT_SNAP[%s]: roleFlag[%zu]=0x%lX val=%d",
            ctx, i,
            static_cast<unsigned long>(g_activeRevival->roleFlagOffsets[i]),
            rf);
    }
    for (size_t i = 1; i < g_activeRevival->sessionPtrOffsetCount; ++i)
    {
        uintptr_t sp = 0;
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->sessionPtrOffsets[i]),
            &sp);
        mod::Log(
            "INIT_SNAP[%s]: sessionPtr[%zu]=0x%lX val=0x%08lX",
            ctx, i,
            static_cast<unsigned long>(g_activeRevival->sessionPtrOffsets[i]),
            static_cast<unsigned long>(sp));
    }

    // ---- Session object fields (only if session pointer is valid) ----
    if (sessionPtr != 0 && sessionPtr >= 0x00100000u)
    {
        uintptr_t vtable = 0;
        int initComplete = -1;
        int activePlayer = -1;
        int queuePlayer = -1;
        int inputDelay = -1;
        int currentFrame = -1;
        int gameModeSnapshot = -1;
        int matchId = -1;
        int sentinel = -1;
        uintptr_t helperHandle = 0;
        uintptr_t histPrimary = 0;
        uintptr_t histSecondary = 0;

        (void)SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInitComplete),
            &initComplete);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetActivePlayer),
            &activePlayer);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetQueuePlayer),
            &queuePlayer);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay),
            &inputDelay);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetCurrentFrame),
            &currentFrame);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetGameModeSnapshot),
            &gameModeSnapshot);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetMatchId),
            &matchId);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetSentinel),
            &sentinel);
        // Guard helperHandle read: mode 2 sessions are only 0x2B0 (688) bytes,
        // and sessionOffsetHelperHandle (700) is out of bounds.  Only read it
        // for modes where the session object is large enough.
        const bool helperHandleInBounds =
            (g_localRoleFlag != kLocalRoleLocalPlay);
        if (helperHandleInBounds)
        {
            (void)SafeReadPtr(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHelperHandle),
                &helperHandle);
        }
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryPtr),
            &histPrimary);
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryPtr),
            &histSecondary);

        mod::Log(
            "INIT_SNAP[%s]: session vtable=0x%08lX initComplete=%d "
            "activePlayer=%d queuePlayer=%d inputDelay=%d",
            ctx,
            static_cast<unsigned long>(vtable),
            initComplete,
            activePlayer,
            queuePlayer,
            inputDelay);

        mod::Log(
            "INIT_SNAP[%s]: session currentFrame=%d gameModeSnap=%d "
            "matchId=%d sentinel=%d helperHandle=0x%08lX",
            ctx,
            currentFrame,
            gameModeSnapshot,
            matchId,
            sentinel,
            static_cast<unsigned long>(helperHandle));

        mod::Log(
            "INIT_SNAP[%s]: session histPrimary=0x%08lX histSecondary=0x%08lX "
            "histPrimaryVec=0x%08lX histSecondaryVec=0x%08lX",
            ctx,
            static_cast<unsigned long>(histPrimary),
            static_cast<unsigned long>(histSecondary),
            static_cast<unsigned long>(sessionPtr + g_activeRevival->sessionOffsetHistoryPrimaryVec),
            static_cast<unsigned long>(sessionPtr + g_activeRevival->sessionOffsetHistorySecondaryVec));

        // Read first few bytes of session for mode-specific field detection.
        // Offset +4/+8/+12/+16 are zeroed by StartInitPlayer; +9 (dword) is
        // set to 1 by the local-play constructor.
        int field4 = -1, field8 = -1, field12 = -1, field16 = -1;
        int field36 = -1;  // this[9] = 1 for local play
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + 4), &field4);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + 8), &field8);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + 12), &field12);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + 16), &field16);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + 36), &field36);

        mod::Log(
            "INIT_SNAP[%s]: session +4=%d +8=%d +12=%d +16=%d +36=%d",
            ctx,
            field4, field8, field12, field16, field36);
    }
    else
    {
        mod::Log("INIT_SNAP[%s]: sessionPtr=0x%08lX (invalid/NULL, no field dump)",
                 ctx, static_cast<unsigned long>(sessionPtr));
    }

    // ---- Our module state (hypothesis flags) ----
    mod::Log(
        "INIT_SNAP[%s]: g_localRoleFlag=%d g_localInitApplied=%d "
        "g_dllExitPatchesSaved=%d g_tournamentPatchesSaved=%d "
        "g_renderContextSaved=%d g_savedRenderCtx=0x%08lX "
        "g_frameHookInstalled=%d initCount=%d",
        ctx,
        g_localRoleFlag,
        g_localInitAppliedForSession ? 1 : 0,
        g_dllExitProcessPatchesSaved ? 1 : 0,
        g_tournamentPatchesSaved ? 1 : 0,
        g_renderContextSaved ? 1 : 0,
        static_cast<unsigned long>(g_savedRenderContext),
        g_frameHookInstalled ? 1 : 0,
        g_forceLocalPlayInitCount);
}

} // namespace netplay::bridge::takeover
