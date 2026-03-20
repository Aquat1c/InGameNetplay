// Revival DLL memory introspection and session field manipulation.

#include "netplay/bridge/takeover_internal.h"
#include "crash_handler.h"

#include <cmath>
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

static bool SafeReadWord(const void* address, uint16_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uint16_t)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const uint16_t*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

static bool SafeReadDouble(const void* address, double* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(double)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const double*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return true;
}

static bool SafeReadDword(const void* address, uint32_t* outValue)
{
    if (outValue == nullptr || !IsReadableRange(address, sizeof(uint32_t)))
    {
        return false;
    }
    __try
    {
        *outValue = *reinterpret_cast<const uint32_t*>(address);
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

// Forward-declared here (before RefreshRuntimeStatus) because the spectator
// name-reading path needs it.  Defined alongside the DLL exit-process patch
// save/restore logic further below.
static bool g_dllExitProcessPatchesSaved = false;

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
    ioStatus->spectateConfirmPromptKind = static_cast<int>(NetbridgeSpectatePromptKind::None);
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
    int spectateConfirmPromptKind = static_cast<int>(NetbridgeSpectatePromptKind::None);
    ReadSpectateConfirmPromptSignal(&spectateConfirmSerial, &spectateConfirmServedSerial, &spectateConfirmPromptKind);
    ioStatus->spectateConfirmPromptSerial = static_cast<int>(spectateConfirmSerial);
    ioStatus->spectateConfirmPromptServedSerial = static_cast<int>(spectateConfirmServedSerial);
    ioStatus->spectateConfirmPromptKind = spectateConfirmPromptKind;

    LONG consoleErrorSerial = 0;
    ReadConsoleError(&consoleErrorSerial, ioStatus->consoleErrorText, sizeof(ioStatus->consoleErrorText));
    ioStatus->consoleErrorSerial = static_cast<int>(consoleErrorSerial);

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

    // The session-field offsets (inputDelay, pingMs, P1Name, P2Name, etc.)
    // are only valid for online sessions (mode 0).  Spectator sessions
    // (mode 1) have a different, smaller layout (0x440 = 1088 bytes) where
    // the online-session offsets map to Config / shared-memory fields.
    // Spectator sessions store wins and names at different offsets:
    //   +128 = P1 wins, +132 = P2 wins
    //   +154 = raw wchar_t[64] P1 name, +282 = raw wchar_t[64] P2 name
    // These spectator offsets are identical across all Revival versions
    // (1.02e through 1.02i).
    const bool isOnlineSession    = (g_localRoleFlag == kLocalRoleOnline);
    const bool isSpectatorSession = (g_localRoleFlag == kLocalRoleSpectate);

    // Spectator session field offsets (constant across all Revival versions).
    constexpr uintptr_t kSpectatorOffsetP1Wins = 128;
    constexpr uintptr_t kSpectatorOffsetP2Wins = 132;
    constexpr uintptr_t kSpectatorOffsetP1Name = 154;  // raw wchar_t[64]
    constexpr uintptr_t kSpectatorOffsetP2Name = 282;  // raw wchar_t[64]

    int delayFrames = -1;
    int pingMs = -1;
    int sessionActivePlayer = -1;
    int sessionP1Wins = 0;
    int sessionP2Wins = 0;
    if (isOnlineSession)
    {
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay), &delayFrames);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetPingMs), &pingMs);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetActivePlayer), &sessionActivePlayer);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetP1Wins), &sessionP1Wins);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetP2Wins), &sessionP2Wins);
    }
    else if (isSpectatorSession)
    {
        // Spectator sessions have no activePlayer, inputDelay, or ping —
        // only wins are meaningful.
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSpectatorOffsetP1Wins), &sessionP1Wins);
        (void)SafeReadInt(reinterpret_cast<const void*>(sessionPtr + kSpectatorOffsetP2Wins), &sessionP2Wins);
    }

    // Expose activePlayer and wins through the bridge status.
    if (sessionActivePlayer == 0 || sessionActivePlayer == 1)
    {
        ioStatus->activePlayer = sessionActivePlayer;
    }
    if (sessionP1Wins >= 0 && sessionP1Wins < 1000)
    {
        ioStatus->sessionP1Wins = sessionP1Wins;
    }
    if (sessionP2Wins >= 0 && sessionP2Wins < 1000)
    {
        ioStatus->sessionP2Wins = sessionP2Wins;
    }

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

    // Read a raw null-terminated wchar_t[] buffer (NOT an std::wstring SSO
    // object) and convert to UTF-8 into outText.  The source buffers are
    // wchar_t[64] (128 bytes) inside the 276-byte config snapshot at
    // session + configStructOffset + 14 / + 142.
    auto tryReadInlineName = [](uintptr_t baseAddress, char* outText, size_t outSize) -> void {
        if (outText == nullptr || outSize == 0)
        {
            return;
        }
        outText[0] = '\0';

        // The raw config Name buffers are wchar_t[64] (128 bytes).
        // We read up to 63 characters; the 64th is guaranteed null by
        // the memset(0, 0x100) in the config struct constructor.
        constexpr size_t kMaxChars = 63;
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

        // Convert to UTF-8.  If the full name exceeds the output buffer,
        // WideCharToMultiByte returns 0 (ERROR_INSUFFICIENT_BUFFER), so we
        // fall back to a safe prefix guaranteed to fit ((outSize-1)/3 chars
        // → worst-case 3 bytes/char for BMP code-points).
        int converted = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(n),
                                            outText, static_cast<int>(outSize - 1),
                                            nullptr, nullptr);
        if (converted <= 0 && n > 0)
        {
            const int safeLen = static_cast<int>((outSize - 1) / 3);
            if (safeLen > 0)
            {
                converted = WideCharToMultiByte(CP_UTF8, 0, wide,
                                                (safeLen < static_cast<int>(n)) ? safeLen : static_cast<int>(n),
                                                outText, static_cast<int>(outSize - 1),
                                                nullptr, nullptr);
            }
        }
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
        isOnlineSession
        && (syncFlags.inRollbackSyncState
            || syncFlags.inRollbackActiveState
            || static_cast<NetbridgePhase>(ioStatus->phase) == NetbridgePhase::Connected);

    if (allowSessionInlineNames)
    {
        tryReadInlineName(sessionPtr + g_activeRevival->sessionOffsetP1Name, ioStatus->p1Name, sizeof(ioStatus->p1Name));
        tryReadInlineName(sessionPtr + g_activeRevival->sessionOffsetP2Name, ioStatus->p2Name, sizeof(ioStatus->p2Name));
        sanitizeInlineName(ioStatus->p1Name, sizeof(ioStatus->p1Name));
        sanitizeInlineName(ioStatus->p2Name, sizeof(ioStatus->p2Name));
    }
    else if (isSpectatorSession && g_dllExitProcessPatchesSaved)
    {
        // Spectator session stores raw wchar_t[64] names at different offsets
        // than the online session.  These are populated from the Init shared
        // memory when the spectator object is fully initialized.
        //
        // We gate on g_dllExitProcessPatchesSaved (set at the end of the
        // init sequence) instead of phase==Connected because spectator
        // sessions may not be promoted to Connected until the next
        // takeover::Tick() call — and TickExportOnly() (per-frame tick
        // hook) only calls RefreshRuntimeStatus(), not the full Tick().
        tryReadInlineName(sessionPtr + kSpectatorOffsetP1Name, ioStatus->p1Name, sizeof(ioStatus->p1Name));
        tryReadInlineName(sessionPtr + kSpectatorOffsetP2Name, ioStatus->p2Name, sizeof(ioStatus->p2Name));
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
// g_dllExitProcessPatchesSaved is declared earlier (before RefreshRuntimeStatus).

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

bool AreDllExitPatchesSaved()
{
    return g_dllExitProcessPatchesSaved;
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
    // Save the 8 bytes at 0x401642 before init() to prevent trampoline leak.
    SaveExeDispatchHookBytes();
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
    // Restore saved 0x401642 bytes to undo init()'s new trampoline.
    RestoreExeDispatchHookBytes();
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
    g_renderContextSaved = false;
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
    block->spectateConfirmPromptKind = 0;
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

    // Only online sessions (mode 0) have the rollback history bindings.
    // Spectator sessions (mode 1) have a different, smaller layout where
    // the online-session offsets (activePlayer, queuePlayer, historyPtrs)
    // overlap with the Config object.  Running this repair on a spectator
    // session would write heap addresses into Config WString fields,
    // corrupting them and likely crashing on the next string operation.
    if (g_localRoleFlag != kLocalRoleOnline)
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

// Frame hook RVA is now profile-driven: g_activeRevival->frameHookRva

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
// Save / restore 8 bytes at EXE address 0x401642 before and after every
// init() call.  This is the REPLACEMENT hook site written by
// EFZ_BufferProcess_WithSize (sub_1006EFB0).  Each init() call mallocs a
// NEW 10‑byte trampoline and overwrites 0x401642 with E9 rel32 + 3 NOPs.
// Without save/restore the old trampoline leaks and the JMP target changes
// — which is benign per se, but accumulates memory and makes the hook
// inconsistent across sessions.  Save/restore keeps the same JMP bytes as
// session 1, eliminating any target-address drift.
// ---------------------------------------------------------------------------
static constexpr uintptr_t kExeDispatchHookAddr = 0x401642u;
static constexpr size_t    kExeDispatchHookSize = 8u;
static uint8_t g_exeDispatchHookSaved[kExeDispatchHookSize] = {};
static bool    g_exeDispatchHookSavedValid = false;

void SaveExeDispatchHookBytes()
{
    memcpy(g_exeDispatchHookSaved,
           reinterpret_cast<const void*>(kExeDispatchHookAddr),
           kExeDispatchHookSize);
    g_exeDispatchHookSavedValid = true;
    mod::Log("SaveExeDispatchHookBytes: saved %zu bytes at 0x%08lX "
             "[%02X %02X %02X %02X %02X %02X %02X %02X]",
             kExeDispatchHookSize,
             static_cast<unsigned long>(kExeDispatchHookAddr),
             g_exeDispatchHookSaved[0], g_exeDispatchHookSaved[1],
             g_exeDispatchHookSaved[2], g_exeDispatchHookSaved[3],
             g_exeDispatchHookSaved[4], g_exeDispatchHookSaved[5],
             g_exeDispatchHookSaved[6], g_exeDispatchHookSaved[7]);
}

void RestoreExeDispatchHookBytes()
{
    if (!g_exeDispatchHookSavedValid)
    {
        mod::Log("RestoreExeDispatchHookBytes: no saved bytes — skipped");
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kExeDispatchHookAddr),
                        kExeDispatchHookSize,
                        PAGE_EXECUTE_READWRITE,
                        &oldProtect))
    {
        mod::Log("RestoreExeDispatchHookBytes: VirtualProtect failed err=%lu",
                 static_cast<unsigned long>(GetLastError()));
        return;
    }

    memcpy(reinterpret_cast<void*>(kExeDispatchHookAddr),
           g_exeDispatchHookSaved,
           kExeDispatchHookSize);

    VirtualProtect(reinterpret_cast<void*>(kExeDispatchHookAddr),
                   kExeDispatchHookSize,
                   oldProtect,
                   &oldProtect);
    FlushInstructionCache(GetCurrentProcess(),
                          reinterpret_cast<void*>(kExeDispatchHookAddr),
                          kExeDispatchHookSize);

    g_exeDispatchHookSavedValid = false;

    uint8_t verify[kExeDispatchHookSize] = {};
    memcpy(verify,
           reinterpret_cast<const void*>(kExeDispatchHookAddr),
           kExeDispatchHookSize);
    mod::Log("RestoreExeDispatchHookBytes: restored %zu bytes at 0x%08lX "
             "[%02X %02X %02X %02X %02X %02X %02X %02X]",
             kExeDispatchHookSize,
             static_cast<unsigned long>(kExeDispatchHookAddr),
             verify[0], verify[1], verify[2], verify[3],
             verify[4], verify[5], verify[6], verify[7]);
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

// g_tickRecoveryPending: same pattern but for RunPerFrameTickDispatch /
// OurPerFrameTickHook.  The per-frame tick is the ACTUAL every-frame entry
// point (sub_1006E570 → vtable[2] → RollbackLoopTick).  ExitProcess most
// commonly fires here when the peer process dies mid-match.
static volatile bool g_tickRecoveryPending = false;

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

// Per-frame tick RVA is now profile-driven: g_activeRevival->perFrameTickRva
static uint8_t g_perFrameTickTrampoline[12] = {};
static bool    g_perFrameTickInstalled      = false;
static bool    g_perFrameMismatchLogged     = false;

// Guard flag: true while g_origPerFrameTick is running.  ForceLocalPlayInit
// must NOT run during this window because sub_1006E570 -> vtable[2] ->
// RollbackLoopTick is using the current session as 'this'. Destroying it
// mid-tick causes a use-after-free crash in SetEvent(this[2]).
static volatile bool g_insideFrameTick = false;
static volatile bool g_deferredCancelCleanup = false;
static volatile LONG g_onlineMatchEscGracefulQuitArmed = 0;
static volatile LONG* g_quitRingHeader = nullptr;

// Spectator tick holdoff: when true, the per-frame tick hook will not
// call RunPerFrameTickDispatch while the spectator session is active on
// the title screen.  This prevents the DLL's spectator input-replay
// loop from consuming shared-memory ring buffer entries before the game
// has transitioned to charselect, which would cause frame misalignment
// and desync (the spectator would advance past charselect inputs while
// still on the title screen, then see mid-game data once charselect
// actually appears).
static bool g_spectateHoldoffLogged = false;
// Set true once the holdoff has been active during a session.  Used to
// trigger a one-shot ring-buffer flush at the exact moment the holdoff
// releases, discarding pre-charselect inputs that accumulated while the
// spectator was still on the title screen.
static bool g_spectateHoldoffWasActive = false;

// Hard-fallback watchdog: counts consecutive frames where the Revival child
// process is dead but no existing recovery mechanism (ExitProcess interception,
// consoleErrorSerial, spectator ESC) has fired.  After a grace period the
// watchdog forces a full cleanup and return to the netplay menu.
static unsigned int g_watchdogDeadFrameCount = 0;
static constexpr unsigned int kWatchdogGraceFrames = 30; // ~0.5s at 60fps

// __thiscall trampoline: ECX = this, no other args.
using PerFrameTickFn = int (__thiscall *)(void* thisPtr);
static PerFrameTickFn g_origPerFrameTick = nullptr;

static uint32_t g_frameTick = 0;           // monotonic per-frame counter

static bool EnsureQuitRingHeader();
static void ReleaseQuitRingHeader();
static bool ConsumeGracefulQuitRingSignal(LONG* outHeadBefore, LONG* outTailBefore);
static char RecoverFromQuitRingSignal(const char* phaseTag, LONG quitHeadBefore, LONG quitTailBefore);

// --- Double-speed diagnostics -------------------------------------------
// Wall-clock time tracking: measure actual FPS by comparing timeGetTime()
// between heartbeats.
static DWORD g_lastHeartbeatTimeMs = 0;
static uint32_t g_lastHeartbeatFrameTick = 0;

// Toggle tracking: gameSys+4968 toggles exactly once per EXE main loop
// iteration.  If our hook sees the same toggle value on consecutive calls,
// the hook is being invoked more than once per main loop frame.
static uint32_t g_lastToggleValue = 0xFFFFFFFFu;
static uint32_t g_toggleSameCount = 0;  // consecutive same-toggle detections
static bool     g_toggleDiagLogged = false;

// Session number: incremented each time ResetGameModeValidation is called
// (i.e. each new session).  Logged in every SPEED_DIAG line so we can
// immediately tell which session produced a given log entry.
static uint32_t g_sessionNumber = 0;

// Guard for per-frame SPEED_DIAG logging.  Disabled by default to avoid
// flooding the log in tournament mode.  Enable when debugging speed issues.
static bool g_speedDiagEnabled = false;

// Timer baseline snapshot: captured on the first SPEED_DIAG read of each
// session.  If timerScalar or timerInterval change later, we log an alert.
static double   g_baselineTimerScalar   = 0.0;
static double   g_baselineTimerInterval = 0.0;
static uintptr_t g_baselineTimerPtr     = 0;
static bool     g_timerBaselineCaptured = false;

// Init-once guard transition tracking: detect when the guard changes
// between frames (should only happen once during first-time global init).
static uint16_t g_lastInitOnceGuard     = 0;
static bool     g_initOnceGuardTracked  = false;

// Render context pointer tracking: detect if the EfzRender* global goes
// NULL or changes unexpectedly between frames (H2 stale context).
static uintptr_t g_lastRenderCtxPtr     = 0;
static bool     g_renderCtxTracked      = false;

// Session pointer tracking within a session: detect if dword_100A02CC
// is silently replaced mid-session (H1 double-init / leaked session).
static uintptr_t g_lastSessionPtrInTick = 0;
static bool     g_sessionPtrTracked     = false;

// Per-frame QPC delta for burst logging: log individual frame-to-frame
// intervals during the first 30 ticks to detect doubled rate from tick 1.
static LARGE_INTEGER g_prevFrameQpc     = {};
static bool     g_prevFrameQpcValid     = false;

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

// ---------------------------------------------------------------------------
// Screen-transition–based win tracking.
//
// Revival's internal win increment (session+1224/1228) does not fire in our
// takeover context because the vtable[2] → RollbackLoopTick →
// BuildMatchInfoAndHUD change-detection wrapper never triggers.  Instead of
// relying on Revival's session wins, we replicate the logic directly:
//
//   When the screen index transitions from 3 (battle) to 5 (results), we
//   read byte 4940 from the EFZ global-state object to determine the match
//   winner (0 = P1, 1 = P2) and increment our own counter.
//
// The global-state object is the same one accessed via gameSys (screenObj+0x1C)
// and via the Revival DLL's globalStatePtrOffset.  Offset 4940 is the
// EFZ_GlobalStruct_GetByte4940() return value used in BuildMatchInfoAndHUD's
// mode-5 branch.
// ---------------------------------------------------------------------------
static volatile LONG g_trackedP1Wins = 0;
static volatile LONG g_trackedP2Wins = 0;

void ResetTrackedWins()
{
    InterlockedExchange(&g_trackedP1Wins, 0);
    InterlockedExchange(&g_trackedP2Wins, 0);
    mod::Log("WIN_TRACK: reset tracked wins to 0-0");
}

static void MonitorScreenIndexChange()
{
    constexpr uintptr_t kScreenIndexAddr = 0x00790148;
    constexpr uintptr_t kScreenTableAddr = 0x00790110;
    constexpr uint32_t kOffsetGameSystem = 0x1C;
    constexpr uint32_t kModeOffset = 4964;
    constexpr uint32_t kWinnerByteOffset = 4940;

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

// RunPerFrameTickDispatch — setjmp guard for the per-frame tick hook.
// Same pattern as RunFrameDispatch: isolates setjmp into a POD-only
// function so C++ recovery can run safely in OurPerFrameTickHook.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4611)
#endif
static int RunPerFrameTickDispatch(void* fixedThis)
{
    g_netplayFrameJmpActive = true;
    if (setjmp(g_netplayFrameJmpBuf) != 0)
    {
        // longjmp path: ExitProcess was intercepted during this frame tick.
        g_netplayFrameJmpActive = false;
        g_insideFrameTick = false;
        g_tickRecoveryPending = true;
        return 0;
    }
    g_insideFrameTick = true;
    const int result = g_origPerFrameTick(fixedThis);
    g_insideFrameTick = false;
    g_netplayFrameJmpActive = false;
    return result;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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
        const DWORD nowMs = GetTickCount();
        DWORD elapsedMs = 0;
        double measuredFps = 0.0;
        if (g_lastHeartbeatTimeMs != 0 && g_lastHeartbeatFrameTick != 0)
        {
            elapsedMs = nowMs - g_lastHeartbeatTimeMs;
            const uint32_t elapsedFrames = g_frameTick - g_lastHeartbeatFrameTick;
            if (elapsedMs > 0)
                measuredFps = static_cast<double>(elapsedFrames) * 1000.0
                              / static_cast<double>(elapsedMs);
        }
        g_lastHeartbeatTimeMs = nowMs;
        g_lastHeartbeatFrameTick = g_frameTick;

        mod::Log(
            "TICK_HOOK: heartbeat S#%u frameTick=%u session=0x%08lX exeECX=0x%08lX match=%d "
            "elapsed=%lums fps=%.1f toggleSame=%u",
            g_sessionNumber,
            g_frameTick,
            static_cast<unsigned long>(currentSession),
            static_cast<unsigned long>(exeThisAddr),
            (exeThisAddr == currentSession) ? 1 : 0,
            static_cast<unsigned long>(elapsedMs),
            measuredFps,
            g_toggleSameCount);
    }

    // --- gameSys+4968 toggle double-tick detection --------------------------
    // The EXE main loop toggles gameSys+4968 once per iteration.  If we see
    // the same value on two consecutive calls, the per-frame tick hook is
    // running more than once per main loop frame.
    {
        constexpr uintptr_t kGameSystemPtr = 0x0079010C;
        uint32_t toggleVal = 0xFFFFFFFFu;
        __try {
            const uint32_t gameSys =
                *reinterpret_cast<const volatile uint32_t*>(kGameSystemPtr);
            if (gameSys != 0)
                toggleVal =
                    *reinterpret_cast<const volatile uint32_t*>(gameSys + 4968);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (toggleVal != 0xFFFFFFFFu
            && g_lastToggleValue != 0xFFFFFFFFu
            && toggleVal == g_lastToggleValue)
        {
            ++g_toggleSameCount;
            if (!g_toggleDiagLogged)
            {
                g_toggleDiagLogged = true;
                mod::Log(
                    "TICK_HOOK: *** DOUBLE TICK DETECTED *** frameTick=%u "
                    "toggleVal=%u — per-frame hook fired twice in same "
                    "main loop iteration",
                    g_frameTick,
                    toggleVal);
            }
        }
        g_lastToggleValue = toggleVal;
    }

    // ---- Pre-tick graceful-end / disconnect detection --------------------
    // Replace Revival's patched-out quitMem -> ExitProcess path on the host
    // side, and keep the existing console-error short-circuit as well.
    bool preTickDisconnect = false;
    bool preTickGracefulQuit = false;
    LONG preTickQuitHead = 0;
    LONG preTickQuitTail = 0;
    if (g_dllExitProcessPatchesSaved
        && InterlockedCompareExchange(&g_onlineMatchEscGracefulQuitArmed, 0, 0) == 0)
    {
        if (ConsumeGracefulQuitRingSignal(&preTickQuitHead, &preTickQuitTail))
        {
            mod::Log(
                "TICK_HOOK: *** PRE-TICK GRACEFUL SESSION END *** frameTick=%u "
                "quitHead=%ld quitTail=%ld — skipping DLL tick",
                g_frameTick,
                static_cast<long>(preTickQuitHead),
                static_cast<long>(preTickQuitTail));
            preTickDisconnect = true;
            preTickGracefulQuit = true;
        }

        if (!preTickDisconnect && g_hostBlock != nullptr)
        {
            const LONG preTickErrSerial =
                InterlockedCompareExchange(&g_hostBlock->consoleErrorSerial, 0, 0);
            if (preTickErrSerial > 0)
            {
                mod::Log(
                    "TICK_HOOK: *** PRE-TICK DISCONNECT *** frameTick=%u "
                    "consoleErrorSerial=%ld — skipping DLL tick to prevent "
                    "corrupted render",
                    g_frameTick,
                    static_cast<long>(preTickErrSerial));
                preTickDisconnect = true;
            }
        }
    }

    // ---- Online match ESC graceful-quit priming --------------------------
    // Pressing Esc during an active online battle should queue Revival's own
    // Quit packet before the later ExitProcess interception tears the helper
    // down. Restrict this to the battle screen so normal post-match cleanup
    // remains untouched.
    {
        static bool s_onlineMatchEscWasDown = false;
        const bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        uint8_t escScreen = 0xFF;
        __try {
            escScreen = *reinterpret_cast<const volatile uint8_t*>(0x00790148u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        const bool onlineBattleActive =
            g_localRoleFlag == kLocalRoleOnline
            && g_localInitAppliedForSession
            && g_dllExitProcessPatchesSaved
            && currentSession != 0
            && escScreen == 3;
        const bool escRisingEdge =
            onlineBattleActive && escDown && !s_onlineMatchEscWasDown;
        s_onlineMatchEscWasDown = escDown;

        if (escRisingEdge)
        {
            const bool quitQueued =
                SignalGracefulQuitRing("online_match_esc_key", 0);
            mod::Log(
                "TICK_HOOK: online match ESC detected frameTick=%u screen=%u "
                "session=0x%08lX quitQueued=%d",
                g_frameTick,
                static_cast<unsigned>(escScreen),
                static_cast<unsigned long>(currentSession),
                quitQueued ? 1 : 0);
            if (quitQueued)
            {
                ArmOnlineMatchEscGracefulQuit();
            }
        }
    }

    // ---- Spectator tick holdoff -----------------------------------------------
    // While spectating and still on the title screen, the DLL's spectator
    // session tick must NOT run.  The session's input-replay loop reads
    // InputP1/P2 ring buffers and calls EFZ_ReplayStream_PushTwoChars +
    // EFZ_GameMode_InvokeAdvance for each available frame.  If this runs
    // while the game is on the title screen (before HandoffSpectateSession
    // transitions to charselect), the spectator would:
    //   (a) consume the match's opening input frames on a screen where
    //       they serve no purpose,
    //   (b) advance EFZ_GameMode_InvokeAdvance on a stale game state,
    //   (c) desync once charselect arrives because the shared-memory
    //       cursor has already moved past the charselect data.
    //
    // Fix: skip RunPerFrameTickDispatch when role == spectator AND the
    // game is on screen index 0 (title).  HandoffSpectateSession sets
    // g_pendingGlobalStateTransition = 1 (charselect), which takes effect
    // on the same frame.  The next frame sees screen index 1 and the
    // holdoff naturally clears.
    //
    // Post-tick paths (disconnect detection, ESC exit) still run normally
    // because we only gate the RunPerFrameTickDispatch call.
    // -----------------------------------------------------------------------
    bool spectateTickHoldoff = false;
    if (g_localRoleFlag == kLocalRoleSpectate
        && g_localInitAppliedForSession
        && g_dllExitProcessPatchesSaved)
    {
        uint8_t holdoffScreen = 0xFF;
        __try {
            holdoffScreen = *reinterpret_cast<const volatile uint8_t*>(0x00790148u);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (holdoffScreen == 0)
        {
            spectateTickHoldoff = true;
            g_spectateHoldoffWasActive = true;
            if (!g_spectateHoldoffLogged)
            {
                g_spectateHoldoffLogged = true;
                mod::Log(
                    "TICK_HOOK: spectator tick holdoff engaged — preventing "
                    "input consumption while on title screen (frameTick=%u)",
                    g_frameTick);
            }
        }
        else if (g_spectateHoldoffLogged)
        {
            // ---- Holdoff-release ring buffer flush -------------------------
            // While the holdoff was active (game on title screen), the child
            // EfzRevival.exe received the host's entire replay history and
            // wrote it to the shared-memory ring buffers.  This history
            // starts from the host's session start — which was on the *host's*
            // title screen, NOT charselect.  The first T frames contain inputs
            // the host's online session captured while the player navigated
            // the mod's netplay menu (potentially Down, Enter, Escape, etc.).
            //
            // The spectator is now entering charselect directly.  If the DLL's
            // spectator tick processes those T title-screen-era inputs on the
            // charselect screen, they would be interpreted as cursor movements
            // and character selections — causing an immediate desync ("inputs
            // became misaligned ... spectator side don't even see characters
            // picked").
            //
            // Fix: flush ALL ring buffers at the holdoff→release transition.
            // This discards every entry accumulated during the holdoff (both
            // title-screen and early-charselect data from the host's replay).
            // The child EXE continues writing live data, so the spectator
            // picks up from the current match state going forward.
            //
            // The trade-off is that the spectator won't replay the charselect
            // from the beginning — it joins the match in progress.  This
            // matches what users observe in practice and avoids the desync.
            // ----------------------------------------------------------------
            if (g_spectateHoldoffWasActive)
            {
                g_spectateHoldoffWasActive = false;

                struct RingMapping {
                    const char* name;
                    HANDLE      hMap;
                    volatile DWORD* view;
                };
                RingMapping mappings[] = {
                    {"InputP1",   nullptr, nullptr},
                    {"InputP2",   nullptr, nullptr},
                    {"PaletteP1", nullptr, nullptr},
                    {"PaletteP2", nullptr, nullptr},
                    {"Sync",      nullptr, nullptr},
                    {"Quit",      nullptr, nullptr},
                    {"LoadMatch", nullptr, nullptr},
                    {"Init",      nullptr, nullptr},
                    {"Net",       nullptr, nullptr},
                };
                constexpr int kMappingCount = 9;

                for (int mi = 0; mi < kMappingCount; ++mi)
                {
                    mappings[mi].hMap = OpenFileMappingA(
                        FILE_MAP_ALL_ACCESS, FALSE, mappings[mi].name);
                    if (mappings[mi].hMap != nullptr)
                    {
                        mappings[mi].view = static_cast<volatile DWORD*>(
                            MapViewOfFile(mappings[mi].hMap,
                                         FILE_MAP_ALL_ACCESS, 0, 0, 8));
                    }
                }
                int flushedCount = 0;
                for (int mi = 0; mi < kMappingCount; ++mi)
                {
                    if (mappings[mi].view == nullptr)
                        continue;
                    const DWORD oldHead = mappings[mi].view[0];
                    const DWORD oldTail = mappings[mi].view[1];
                    if (oldHead != oldTail)
                    {
                        mappings[mi].view[0] = oldTail;
                        ++flushedCount;
                        mod::Log(
                            "TICK_HOOK: holdoff-release flushed '%s' "
                            "head=%lu->%lu tail=%lu",
                            mappings[mi].name,
                            static_cast<unsigned long>(oldHead),
                            static_cast<unsigned long>(oldTail),
                            static_cast<unsigned long>(oldTail));
                    }
                }
                for (int mi = 0; mi < kMappingCount; ++mi)
                {
                    if (mappings[mi].view != nullptr)
                        UnmapViewOfFile(const_cast<DWORD*>(mappings[mi].view));
                    if (mappings[mi].hMap != nullptr)
                        CloseHandle(mappings[mi].hMap);
                }
                mod::Log(
                    "TICK_HOOK: spectator holdoff-release flush complete "
                    "flushed=%d buffers (frameTick=%u)",
                    flushedCount, g_frameTick);
            }

            mod::Log(
                "TICK_HOOK: spectator tick holdoff released — screen=%u, "
                "DLL session tick now active (frameTick=%u)",
                static_cast<unsigned>(holdoffScreen),
                g_frameTick);
            g_spectateHoldoffLogged = false;
        }
    }
    else if (g_spectateHoldoffLogged)
    {
        // Session ended or role changed while holdoff was logged.
        g_spectateHoldoffLogged = false;
    }

    // Call the original sub_1006E570 with the corrected ECX — unless
    // a pre-tick disconnect was detected or spectator tick holdoff is
    // active, in which case we skip the DLL's tick entirely.
    int result = 0;
    LARGE_INTEGER tickQpcBefore = {}, tickQpcAfter = {};

    if (!preTickDisconnect && !spectateTickHoldoff)
    {
        // Wrapped in RunPerFrameTickDispatch which sets up a setjmp recovery
        // point so NeutralizeExitProcess can longjmp back if ExitProcess fires
        // during the DLL's session tick (vtable[2] → RollbackLoopTick).
        QueryPerformanceCounter(&tickQpcBefore);
        result = RunPerFrameTickDispatch(fixedThis);
        QueryPerformanceCounter(&tickQpcAfter);
    }

    // ====================================================================
    // Comprehensive per-frame diagnostics (double-speed bug)
    // ====================================================================
    // Log EVERYTHING that could explain why the game runs at double FPS
    // on the second (and subsequent) netplay sessions.
    //
    // Fires: every frame for first 30 ticks (burst), every 30 frames after
    // (2x/sec at 60fps), and ALWAYS when result > 1 (multi-iteration).
    // ====================================================================
    if (g_speedDiagEnabled
        && g_activeRevival != nullptr
        && currentSession != 0
        && g_dllExitProcessPatchesSaved
        && g_localRoleFlag != kLocalRoleLocalPlay)
    {
        const bool isBurst = (g_frameTick <= 30);
        const bool isPeriodic = (g_frameTick % 30 == 0);
        const bool isMultiIter = (result > 1);
        if (isBurst || isPeriodic || isMultiIter)
        {
            HMODULE revival = GetModuleHandleA("EfzRevival.dll");
            const uintptr_t dllBase = revival
                ? reinterpret_cast<uintptr_t>(revival) : 0;

            // ---- 1. Session object fields ----
            int inputDelay = -1, activePlayer = -1, queuePlayer = -1;
            int currentFrame = -1, matchId = -1, initComplete = -1;
            uint32_t pingStruct[4] = {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF};
            uint8_t screenIdx = 0xFF;
            // Fields at known fixed offsets relative to sessionOffsetGameModeSnapshot:
            //   +716 = previousGameMode,  +720 = currentGameMode
            //   +724 = matchStartFrame,   +728 = advanceCounter
            //   +736 = syncFrameCounter
            int prevGameMode = -1, curGameMode = -1, matchStartFrame = -1;
            int advanceCounter = -1, syncFrameCounter = -1;
            int windowBaseDelay = -1;  // +692 = prediction enabled / windowBaseDelayOffset
            uint32_t sentinelVal = 0;
            uintptr_t sessionVtable = 0;
            // +1236 = highestFrameReached (DWORD[309])
            int highestFrame = -1;

            (void)SafeReadPtr(reinterpret_cast<const void*>(currentSession), &sessionVtable);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetInputDelay), &inputDelay);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetActivePlayer), &activePlayer);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetQueuePlayer), &queuePlayer);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetCurrentFrame), &currentFrame);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetMatchId), &matchId);
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetInitComplete), &initComplete);
            (void)SafeReadDword(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetSentinel), &sentinelVal);

            // Game mode fields — always 4 bytes after sessionOffsetGameModeSnapshot.
            const uintptr_t gmBase = currentSession + g_activeRevival->sessionOffsetGameModeSnapshot;
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase), &prevGameMode);       // +716
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 4), &curGameMode);    // +720
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 8), &matchStartFrame);// +724
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 12), &advanceCounter);// +728
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 20), &syncFrameCounter);// +736

            // windowBaseDelayOffset = sessionOffsetInputDelay + 4 (byte offset +692)
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetInputDelay + 4), &windowBaseDelay);

            // highestFrameReached = sentinel offset + 4 (byte offset +1236)
            (void)SafeReadInt(reinterpret_cast<const void*>(
                currentSession + g_activeRevival->sessionOffsetSentinel + 4), &highestFrame);

            // Read the 4-DWORD ping struct (AdjustPrediction / WaitLoop fields).
            const uintptr_t pingBase = currentSession + g_activeRevival->sessionOffsetPingStructBase;
            for (int pi = 0; pi < 4; ++pi)
            {
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(pingBase + pi * 4),
                    reinterpret_cast<int*>(&pingStruct[pi]));
            }

            __try {
                screenIdx = *reinterpret_cast<const volatile uint8_t*>(0x00790148u);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            // ---- 2. DLL global variables ----
            uint16_t initOnceGuard = 0;
            uintptr_t timerPtr = 0, renderCtxPtr = 0, globalStatePtr = 0;
            uintptr_t subStructBase = 0;
            int initFlag = -1;
            double timerScalar = 0.0;   // timerPtr+32: 1000.0/interval
            double timerInterval = 0.0; // timerPtr+16: frame interval in ms

            if (dllBase != 0)
            {
                (void)SafeReadWord(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->initOnceGuardOffset),
                    &initOnceGuard);
                (void)SafeReadPtr(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->timerPtrOffset),
                    &timerPtr);
                (void)SafeReadPtr(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->renderContextGlobalOffset),
                    &renderCtxPtr);
                (void)SafeReadPtr(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->globalStatePtrOffset),
                    &globalStatePtr);
                (void)SafeReadPtr(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->renderContextBaseOffset),
                    &subStructBase);
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(dllBase + g_activeRevival->initFlagOffset),
                    &initFlag);

                // Read timer context: [+16]=interval(double), [+32]=scalar(double)
                if (timerPtr != 0)
                {
                    (void)SafeReadDouble(
                        reinterpret_cast<const void*>(timerPtr + 16), &timerInterval);
                    (void)SafeReadDouble(
                        reinterpret_cast<const void*>(timerPtr + 32), &timerScalar);
                }
            }

            // ---- 3. Compute what AdjustPrediction would compute ----
            // Replicate the math so we can see the intermediate values:
            //   halfPeriod = pingTicks / (2 * timerScalar)
            //   threshold = floor(halfPeriod) - 1
            // If (localFrames - remoteFrames) < threshold → "Add frame"
            double halfPeriodFloat = 0.0;
            int predThreshold = -9999;
            if (timerScalar > 0.0)
            {
                halfPeriodFloat = static_cast<double>(pingStruct[0])
                                  / (2.0 * timerScalar);
                predThreshold = static_cast<int>(floor(halfPeriodFloat)) - 1;
            }

            // ---- 4. DLL tick timing ----
            LARGE_INTEGER qpcFreq;
            QueryPerformanceFrequency(&qpcFreq);
            double tickDurationUs = 0.0;
            if (tickQpcAfter.QuadPart > tickQpcBefore.QuadPart)
            {
                tickDurationUs = static_cast<double>(
                    tickQpcAfter.QuadPart - tickQpcBefore.QuadPart)
                    * 1000000.0 / static_cast<double>(qpcFreq.QuadPart);
            }

            // ---- 5. Vtable RVA for session type identification ----
            uintptr_t vtableRva = (dllBase != 0 && sessionVtable >= dllBase)
                ? (sessionVtable - dllBase) : 0;

            // ---- LOG LINE 1: core session state ----
            mod::Log(
                "SPEED_DIAG[1]: S#%u tick=%u result=%d screen=%u role=%d "
                "session=0x%08lX vtableRVA=0x%lX "
                "frame=%d matchId=%d initComp=%d sentinel=0x%08lX "
                "tickUs=%.0f%s",
                g_sessionNumber, g_frameTick, result,
                static_cast<unsigned>(screenIdx), g_localRoleFlag,
                static_cast<unsigned long>(currentSession),
                static_cast<unsigned long>(vtableRva),
                currentFrame, matchId, initComplete,
                static_cast<unsigned long>(sentinelVal),
                tickDurationUs,
                isMultiIter ? " *** MULTI-ITER ***" : "");

            // ---- LOG LINE 2: game mode / timing fields ----
            mod::Log(
                "SPEED_DIAG[2]: prevMode=%d curMode=%d matchStart=%d "
                "advCtr=%d syncFrame=%d highFrame=%d "
                "inputDelay=%d wndBaseDelay=%d "
                "active=%d queue=%d",
                prevGameMode, curGameMode, matchStartFrame,
                advanceCounter, syncFrameCounter, highestFrame,
                inputDelay, windowBaseDelay,
                activePlayer, queuePlayer);

            // ---- LOG LINE 3: ping struct + prediction math ----
            mod::Log(
                "SPEED_DIAG[3]: ping[0]=%u [1]=%u [2]=%u [3]=%u "
                "timerScalar=%.4f timerInterval=%.4f "
                "halfPeriod=%.6f predThreshold=%d",
                pingStruct[0], pingStruct[1], pingStruct[2], pingStruct[3],
                timerScalar, timerInterval,
                halfPeriodFloat, predThreshold);

            // ---- LOG LINE 4: DLL globals ----
            mod::Log(
                "SPEED_DIAG[4]: initOnceGuard=0x%04X initFlag=%d "
                "timerPtr=0x%08lX renderCtx=0x%08lX "
                "globalState=0x%08lX subStructBase=0x%08lX",
                static_cast<unsigned>(initOnceGuard), initFlag,
                static_cast<unsigned long>(timerPtr),
                static_cast<unsigned long>(renderCtxPtr),
                static_cast<unsigned long>(globalStatePtr),
                static_cast<unsigned long>(subStructBase));

            // ---- LOG LINE 5: EXE hook bytes at 0x401582 and 0x401642 ----
            // 0x401582 = frame-hook dispatcher (sub_1006E590 entry point)
            // 0x401642 = per-frame tick (sub_1006E570 dispatch site)
            if (isBurst || isMultiIter)
            {
                uint8_t exeHookA[12] = {}, exeHookB[12] = {};
                __try {
                    for (int bi = 0; bi < 12; ++bi)
                    {
                        exeHookA[bi] = reinterpret_cast<const volatile uint8_t*>(0x401582u)[bi];
                        exeHookB[bi] = reinterpret_cast<const volatile uint8_t*>(0x401642u)[bi];
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}

                mod::Log(
                    "SPEED_DIAG[5a]: EXE@0x401582: "
                    "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    exeHookA[0], exeHookA[1], exeHookA[2], exeHookA[3],
                    exeHookA[4], exeHookA[5], exeHookA[6], exeHookA[7],
                    exeHookA[8], exeHookA[9], exeHookA[10], exeHookA[11]);
                mod::Log(
                    "SPEED_DIAG[5b]: EXE@0x401642: "
                    "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                    exeHookB[0], exeHookB[1], exeHookB[2], exeHookB[3],
                    exeHookB[4], exeHookB[5], exeHookB[6], exeHookB[7],
                    exeHookB[8], exeHookB[9], exeHookB[10], exeHookB[11]);

                // Also dump DLL-side trampoline bytes (perFrameTickRva)
                if (dllBase != 0)
                {
                    const uintptr_t hookAddr = dllBase + g_activeRevival->perFrameTickRva;
                    uint8_t dllHook[12] = {};
                    __try {
                        for (int bi = 0; bi < 12; ++bi)
                            dllHook[bi] = reinterpret_cast<const volatile uint8_t*>(hookAddr)[bi];
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}

                    mod::Log(
                        "SPEED_DIAG[5c]: DLL perFrameTick @0x%08lX: "
                        "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                        static_cast<unsigned long>(hookAddr),
                        dllHook[0], dllHook[1], dllHook[2], dllHook[3],
                        dllHook[4], dllHook[5], dllHook[6], dllHook[7],
                        dllHook[8], dllHook[9], dllHook[10], dllHook[11]);

                    // Also dump frameHookRva (sub_1006E590) to see if it's been
                    // re-hooked or clobbered.
                    const uintptr_t fhAddr = dllBase + g_activeRevival->frameHookRva;
                    uint8_t fhHook[12] = {};
                    __try {
                        for (int bi = 0; bi < 12; ++bi)
                            fhHook[bi] = reinterpret_cast<const volatile uint8_t*>(fhAddr)[bi];
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}

                    mod::Log(
                        "SPEED_DIAG[5d]: DLL frameHook @0x%08lX: "
                        "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                        static_cast<unsigned long>(fhAddr),
                        fhHook[0], fhHook[1], fhHook[2], fhHook[3],
                        fhHook[4], fhHook[5], fhHook[6], fhHook[7],
                        fhHook[8], fhHook[9], fhHook[10], fhHook[11]);
                }
            }

            // ---- LOG LINE 6: Ring buffer head/tail for InputP1/InputP2 ----
            // These are shared-memory regions read by the DLL session.
            // Head/tail at DWORD[0] and DWORD[1] of each mapping.
            if (isBurst || isPeriodic || isMultiIter)
            {
                struct RingProbe {
                    const char* name;
                    DWORD head, tail;
                    bool ok;
                };
                RingProbe probes[] = {
                    {"InputP1", 0, 0, false},
                    {"InputP2", 0, 0, false},
                    {"Sync",    0, 0, false},
                    {"Net",     0, 0, false},
                };
                constexpr int kProbeCount = 4;

                for (int pi = 0; pi < kProbeCount; ++pi)
                {
                    HANDLE hMap = OpenFileMappingA(
                        FILE_MAP_READ, FALSE, probes[pi].name);
                    if (hMap != nullptr)
                    {
                        const volatile DWORD* view = static_cast<const volatile DWORD*>(
                            MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 8));
                        if (view != nullptr)
                        {
                            probes[pi].head = view[0];
                            probes[pi].tail = view[1];
                            probes[pi].ok = true;
                            UnmapViewOfFile(const_cast<DWORD*>(view));
                        }
                        CloseHandle(hMap);
                    }
                }

                mod::Log(
                    "SPEED_DIAG[6]: ring InputP1(%s h=%lu t=%lu d=%ld) "
                    "InputP2(%s h=%lu t=%lu d=%ld) "
                    "Sync(%s h=%lu t=%lu) Net(%s h=%lu t=%lu)",
                    probes[0].ok ? "ok" : "NO",
                    static_cast<unsigned long>(probes[0].head),
                    static_cast<unsigned long>(probes[0].tail),
                    static_cast<long>(probes[0].tail - probes[0].head),
                    probes[1].ok ? "ok" : "NO",
                    static_cast<unsigned long>(probes[1].head),
                    static_cast<unsigned long>(probes[1].tail),
                    static_cast<long>(probes[1].tail - probes[1].head),
                    probes[2].ok ? "ok" : "NO",
                    static_cast<unsigned long>(probes[2].head),
                    static_cast<unsigned long>(probes[2].tail),
                    probes[3].ok ? "ok" : "NO",
                    static_cast<unsigned long>(probes[3].head),
                    static_cast<unsigned long>(probes[3].tail));
            }

            // ---- LOG LINE 7: gameSys state (mode bytes, toggle) ----
            if (isBurst || isPeriodic)
            {
                uint32_t gameSys = 0;
                uint8_t gameSysMode = 0xFF, gameSysMode2 = 0xFF;
                uint32_t gameSysToggle = 0xFFFFFFFF;
                __try {
                    gameSys = *reinterpret_cast<const volatile uint32_t*>(0x0079010Cu);
                    if (gameSys != 0)
                    {
                        gameSysMode = *reinterpret_cast<const volatile uint8_t*>(gameSys + 4964);
                        gameSysMode2 = *reinterpret_cast<const volatile uint8_t*>(gameSys + 4965);
                        gameSysToggle = *reinterpret_cast<const volatile uint32_t*>(gameSys + 4968);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {}

                mod::Log(
                    "SPEED_DIAG[7]: gameSys=0x%08lX mode=%u/%u toggle=%u "
                    "toggleSameCount=%u exeECX=0x%08lX dllSession=0x%08lX "
                    "ecxMatch=%d",
                    static_cast<unsigned long>(gameSys),
                    static_cast<unsigned>(gameSysMode),
                    static_cast<unsigned>(gameSysMode2),
                    gameSysToggle,
                    g_toggleSameCount,
                    static_cast<unsigned long>(exeThisAddr),
                    static_cast<unsigned long>(currentSession),
                    (exeThisAddr == currentSession) ? 1 : 0);
            }

            // ---- QPC-based precise FPS measurement (every 30 frames) ----
            if (isPeriodic)
            {
                static LARGE_INTEGER s_lastQpc = {};
                static uint32_t s_lastQpcTick = 0;
                LARGE_INTEGER qpcNow;
                QueryPerformanceCounter(&qpcNow);
                if (s_lastQpcTick != 0 && s_lastQpcTick < g_frameTick)
                {
                    const double elapsed =
                        static_cast<double>(qpcNow.QuadPart - s_lastQpc.QuadPart)
                        / static_cast<double>(qpcFreq.QuadPart);
                    const uint32_t dFrames = g_frameTick - s_lastQpcTick;
                    const double qpcFps = (elapsed > 0.0)
                        ? static_cast<double>(dFrames) / elapsed : 0.0;
                    mod::Log(
                        "SPEED_DIAG[8]: QPC fps=%.2f elapsed=%.4fs frames=%u "
                        "qpcFreq=%lld",
                        qpcFps, elapsed, dFrames,
                        static_cast<long long>(qpcFreq.QuadPart));
                }
                s_lastQpc = qpcNow;
                s_lastQpcTick = g_frameTick;
            }

            // ---- LOG LINE 9: cross-frame change detection ----
            // Detects silent changes to timer, init-once guard, render
            // context, and session pointer between frames.  Fires every
            // time the SPEED_DIAG block fires (burst + periodic + multi).
            {
                // 9a: Timer baseline capture and drift detection.
                if (!g_timerBaselineCaptured && timerPtr != 0
                    && timerScalar != 0.0)
                {
                    g_baselineTimerScalar   = timerScalar;
                    g_baselineTimerInterval = timerInterval;
                    g_baselineTimerPtr      = timerPtr;
                    g_timerBaselineCaptured = true;
                    mod::Log(
                        "SPEED_DIAG[9a]: S#%u timer baseline captured "
                        "ptr=0x%08lX scalar=%.4f interval=%.4f",
                        g_sessionNumber,
                        static_cast<unsigned long>(timerPtr),
                        timerScalar, timerInterval);
                }
                else if (g_timerBaselineCaptured)
                {
                    const bool ptrChanged = (timerPtr != g_baselineTimerPtr);
                    const bool scalarChanged =
                        (fabs(timerScalar - g_baselineTimerScalar) > 0.001);
                    const bool intervalChanged =
                        (fabs(timerInterval - g_baselineTimerInterval) > 0.001);
                    if (ptrChanged || scalarChanged || intervalChanged)
                    {
                        mod::Log(
                            "SPEED_DIAG[9a]: S#%u *** TIMER CHANGED *** "
                            "ptr 0x%08lX->0x%08lX "
                            "scalar %.4f->%.4f interval %.4f->%.4f",
                            g_sessionNumber,
                            static_cast<unsigned long>(g_baselineTimerPtr),
                            static_cast<unsigned long>(timerPtr),
                            g_baselineTimerScalar, timerScalar,
                            g_baselineTimerInterval, timerInterval);
                        // Update baseline so we don't spam.
                        g_baselineTimerScalar   = timerScalar;
                        g_baselineTimerInterval = timerInterval;
                        g_baselineTimerPtr      = timerPtr;
                    }
                }

                // 9b: Init-once guard transition detection.
                if (!g_initOnceGuardTracked)
                {
                    g_lastInitOnceGuard    = initOnceGuard;
                    g_initOnceGuardTracked = true;
                }
                else if (initOnceGuard != g_lastInitOnceGuard)
                {
                    mod::Log(
                        "SPEED_DIAG[9b]: S#%u *** INIT-ONCE GUARD CHANGED *** "
                        "0x%04X -> 0x%04X (low byte: %s)",
                        g_sessionNumber,
                        static_cast<unsigned>(g_lastInitOnceGuard),
                        static_cast<unsigned>(initOnceGuard),
                        (initOnceGuard & 0xFF) != 0
                            ? "SET — global init SKIPPED"
                            : "CLEAR — global init WILL RUN");
                    g_lastInitOnceGuard = initOnceGuard;
                }

                // 9c: Render context pointer change detection (H2).
                if (!g_renderCtxTracked)
                {
                    g_lastRenderCtxPtr  = renderCtxPtr;
                    g_renderCtxTracked  = true;
                }
                else if (renderCtxPtr != g_lastRenderCtxPtr)
                {
                    mod::Log(
                        "SPEED_DIAG[9c]: S#%u *** RENDER CTX CHANGED *** "
                        "0x%08lX -> 0x%08lX%s",
                        g_sessionNumber,
                        static_cast<unsigned long>(g_lastRenderCtxPtr),
                        static_cast<unsigned long>(renderCtxPtr),
                        renderCtxPtr == 0
                            ? " — NOW NULL (H2 stale context!)"
                            : "");
                    g_lastRenderCtxPtr = renderCtxPtr;
                }

                // 9d: Session pointer change detection (H1 double-init).
                if (!g_sessionPtrTracked)
                {
                    g_lastSessionPtrInTick = currentSession;
                    g_sessionPtrTracked    = true;
                }
                else if (currentSession != g_lastSessionPtrInTick)
                {
                    mod::Log(
                        "SPEED_DIAG[9d]: S#%u *** SESSION PTR CHANGED *** "
                        "0x%08lX -> 0x%08lX (tick=%u)",
                        g_sessionNumber,
                        static_cast<unsigned long>(g_lastSessionPtrInTick),
                        static_cast<unsigned long>(currentSession),
                        g_frameTick);
                    g_lastSessionPtrInTick = currentSession;
                }
            }

            // ---- LOG LINE 10: per-frame QPC delta (burst only) ----
            // During the first 30 ticks, log the exact wall-clock interval
            // between consecutive frames.  At 64fps each delta should be
            // ~15.6ms; at 128fps (double-speed bug) each delta is ~7.8ms.
            if (isBurst)
            {
                LARGE_INTEGER qpcNow;
                QueryPerformanceCounter(&qpcNow);
                if (g_prevFrameQpcValid)
                {
                    const double deltaMs =
                        static_cast<double>(
                            qpcNow.QuadPart - g_prevFrameQpc.QuadPart)
                        * 1000.0
                        / static_cast<double>(qpcFreq.QuadPart);
                    mod::Log(
                        "SPEED_DIAG[10]: S#%u tick=%u frameDeltaMs=%.3f "
                        "(expect ~15.6 at 64fps, ~7.8 at 128fps)",
                        g_sessionNumber, g_frameTick, deltaMs);
                }
                g_prevFrameQpc = qpcNow;
                g_prevFrameQpcValid = true;
            }

            // ---- LOG LINE 11: timer object deep dump (burst only) ----
            // Read additional timer object fields beyond +16/+32 to
            // capture the full timer state on session start.
            if (isBurst && timerPtr != 0 && g_frameTick <= 5)
            {
                double timerField0 = 0.0, timerField8 = 0.0;
                double timerField24 = 0.0, timerField40 = 0.0;
                int32_t timerField48 = 0;
                (void)SafeReadDouble(
                    reinterpret_cast<const void*>(timerPtr + 0),
                    &timerField0);
                (void)SafeReadDouble(
                    reinterpret_cast<const void*>(timerPtr + 8),
                    &timerField8);
                (void)SafeReadDouble(
                    reinterpret_cast<const void*>(timerPtr + 24),
                    &timerField24);
                (void)SafeReadDouble(
                    reinterpret_cast<const void*>(timerPtr + 40),
                    &timerField40);
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(timerPtr + 48),
                    &timerField48);
                mod::Log(
                    "SPEED_DIAG[11]: S#%u timerDump "
                    "+0=%.6f +8=%.6f +16=%.6f +24=%.6f "
                    "+32=%.6f +40=%.6f +48=%d",
                    g_sessionNumber,
                    timerField0, timerField8, timerInterval,
                    timerField24, timerScalar, timerField40,
                    timerField48);
            }
        }
    }

    // ---- ExitProcess recovery path -----------------------------------------
    // If ExitProcess fired during the per-frame tick, NeutralizeExitProcess
    // longjmp'd back through RunPerFrameTickDispatch, which set
    // g_tickRecoveryPending.  Perform full cleanup now that we're outside
    // the setjmp scope and can safely use C++ constructs.
    if (g_tickRecoveryPending)
    {
        g_tickRecoveryPending = false;

        const int recoveredRole = g_localRoleFlag;
        const DWORD recoveredPid = g_revivalProcessId;
        LogSessionDiagnosticState("TickHook_recovery_entry");
        mod::Log(
            "TICK_HOOK: ExitProcess intercepted during per-frame tick "
            "(role=%d pid=%lu) — performing full cleanup",
            recoveredRole,
            static_cast<unsigned long>(recoveredPid));

        // Step 1: Reinstate a live local-play session.
        const bool initOk = ForceLocalPlayInit();
        mod::Log(
            "TICK_HOOK: recovery step 1 ForceLocalPlayInit result=%d",
            initOk ? 1 : 0);

        // Step 2: Terminate the dead helper process.
        if (g_revivalProcess != nullptr)
        {
            const BOOL termOk = TerminateProcess(g_revivalProcess, 0);
            const DWORD termErr = termOk ? 0 : GetLastError();
            CloseHandle(g_revivalProcess);
            g_revivalProcess = nullptr;
            g_revivalProcessId = 0;
            mod::Log(
                "TICK_HOOK: recovery step 2 helper terminated "
                "(pid=%lu termOk=%d err=%lu)",
                static_cast<unsigned long>(recoveredPid),
                termOk ? 1 : 0,
                static_cast<unsigned long>(termErr));
        }
        else
        {
            mod::Log("TICK_HOOK: recovery step 2 skipped (no helper handle)");
        }

        // Step 3: Restore DLL Jcc patches.
        const bool patchOk = RestoreDllExitProcessPatches();
        mod::Log(
            "TICK_HOOK: recovery step 3 RestoreDllExitProcessPatches result=%d",
            patchOk ? 1 : 0);

        // Step 4: Disable stale text overlays.
        const bool textOk = DisableRevivalTextRendering();
        mod::Log(
            "TICK_HOOK: recovery step 4 DisableRevivalTextRendering result=%d",
            textOk ? 1 : 0);

        // Step 5: Reset crash/validation state.
        mod::ResetCrashRecoveryState();
        ResetGameModeValidation();
        mod::Log("TICK_HOOK: recovery step 5 crash/validation state reset");

        // Step 6: Force game mode to title screen.
        const bool modeOk = ForceGameModeToTitle();
        mod::Log(
            "TICK_HOOK: recovery step 6 ForceGameModeToTitle result=%d",
            modeOk ? 1 : 0);

        g_localInitAppliedForSession = false;
        mod::Log(
            "TICK_HOOK: full recovery complete (was role=%d), "
            "next title-screen frame will consume exit interception",
            recoveredRole);
        LogSessionDiagnosticState("TickHook_recovery_exit");

        return 0;
    }

    if (preTickGracefulQuit)
    {
        return RecoverFromQuitRingSignal("PRE-TICK", preTickQuitHead, preTickQuitTail);
    }

    // ---- Proactive graceful session-end detection -------------------------
    // Also catch Quit-ring signals that were published during the current
    // DLL tick, not just between frames.
    if (g_dllExitProcessPatchesSaved
        && InterlockedCompareExchange(&g_onlineMatchEscGracefulQuitArmed, 0, 0) == 0)
    {
        LONG quitHeadAfter = 0;
        LONG quitTailAfter = 0;
        if (ConsumeGracefulQuitRingSignal(&quitHeadAfter, &quitTailAfter))
        {
            return RecoverFromQuitRingSignal("POST-TICK", quitHeadAfter, quitTailAfter);
        }
    }

    // ---- Proactive network-disconnect detection ---------------------------
    // The DLL exit-process Jcc patches make ExitProcess unreachable, which
    // is a problem during charselect/loading: the DLL's session tick doesn't
    // actively drive rollback and never triggers ExitProcess even after the
    // remote opponent disconnects.  The helper process (EfzRevival.exe)
    // stays alive because ExitProcess is patched out, but its console
    // capture *does* detect the disconnect and publishes an error string
    // to the IPC shared block.  Detected messages include:
    //   - "Connection timed out"      (initial handshake timeout)
    //   - "Source quit or timed out"  (connected source peer died)
    //   - "Host timed out"            (host peer timed out)
    //   - "Remote timed out"          (remote peer timed out, general)
    //   - "Peer died"                 (backup: any "<endpoint> died" trace)
    //   - "Socket error"              (low-level network failure)
    //
    // Check every frame (not just every ~60) to minimise the window where
    // the DLL could render corrupted frames with missing opponent data.
    // This post-tick check catches errors set DURING the current tick
    // (complementing the pre-tick check which catches errors from BETWEEN
    // frames).
    // -----------------------------------------------------------------------
    if (g_dllExitProcessPatchesSaved
        && g_hostBlock != nullptr)
    {
        const LONG consoleErrSerial =
            InterlockedCompareExchange(&g_hostBlock->consoleErrorSerial, 0, 0);
        if (consoleErrSerial > 0)
        {
            // Read the error text for logging before recovery clears it.
            char consoleErrText[128] = {};
            ReadConsoleError(nullptr, consoleErrText, sizeof(consoleErrText));

            const int deadRole = g_localRoleFlag;
            const DWORD deadPid = g_revivalProcessId;
            LogSessionDiagnosticState("TickHook_disconnectDetected_entry");
            mod::Log(
                "TICK_HOOK: *** NETWORK DISCONNECT *** frameTick=%u "
                "role=%d pid=%lu consoleError='%s' — synthesizing exit interception",
                g_frameTick,
                deadRole,
                static_cast<unsigned long>(deadPid),
                consoleErrText);

            // Mimic NeutralizeExitProcess: set exit-interception flags so
            // ConsumeRevivalExitInterception fires on the title screen.
            InterlockedExchange(&g_revivalExitMode,
                                static_cast<LONG>(g_localRoleFlag));
            InterlockedExchange(&g_revivalExitIntercepted, 1);

            // Neutralise the session vtable so subsequent ticks are no-ops.
            NeutralizeRevivalSessionVtable();

            // Step 1: Reinstate a live local-play session.
            const bool initOk = ForceLocalPlayInit();
            mod::Log(
                "TICK_HOOK: disconnect step 1 ForceLocalPlayInit result=%d",
                initOk ? 1 : 0);

            // Step 2: Terminate the dead helper process.
            if (g_revivalProcess != nullptr)
            {
                const BOOL termOk = TerminateProcess(g_revivalProcess, 0);
                const DWORD termErr = termOk ? 0 : GetLastError();
                CloseHandle(g_revivalProcess);
                g_revivalProcess = nullptr;
                g_revivalProcessId = 0;
                mod::Log(
                    "TICK_HOOK: disconnect step 2 helper terminated "
                    "(pid=%lu termOk=%d err=%lu)",
                    static_cast<unsigned long>(deadPid),
                    termOk ? 1 : 0,
                    static_cast<unsigned long>(termErr));
            }

            // Step 3: Restore DLL Jcc patches.
            const bool patchOk = RestoreDllExitProcessPatches();
            mod::Log(
                "TICK_HOOK: disconnect step 3 RestoreDllExitProcessPatches result=%d",
                patchOk ? 1 : 0);

            // Step 4: Disable stale text overlays.
            const bool textOk = DisableRevivalTextRendering();
            mod::Log(
                "TICK_HOOK: disconnect step 4 DisableRevivalTextRendering result=%d",
                textOk ? 1 : 0);

            // Step 5: Reset crash/validation state.
            mod::ResetCrashRecoveryState();
            ResetGameModeValidation();
            mod::Log("TICK_HOOK: disconnect step 5 crash/validation state reset");

            // Step 6: Force game mode to title screen.
            const bool modeOk = ForceGameModeToTitle();
            mod::Log(
                "TICK_HOOK: disconnect step 6 ForceGameModeToTitle result=%d",
                modeOk ? 1 : 0);

            g_localInitAppliedForSession = false;
            mod::Log(
                "TICK_HOOK: disconnect recovery complete (was role=%d), "
                "next title-screen frame will consume exit interception",
                deadRole);
            LogSessionDiagnosticState("TickHook_disconnectDetected_exit");

            return 0;
        }
    }

    // -----------------------------------------------------------------------
    // Spectator ESC exit
    // -----------------------------------------------------------------------
    // While spectating, the Revival DLL's input-replay system consumes all
    // local keyboard input, so the game's own Esc handler never fires.
    // We detect Esc ourselves with GetAsyncKeyState and synthesize the same
    // exit interception that the disconnect path uses, which routes the
    // player back through the title screen into the netplay menu.
    // -----------------------------------------------------------------------
    if (g_localRoleFlag == kLocalRoleSpectate
        && g_dllExitProcessPatchesSaved)
    {
        static bool s_spectateEscWasDown = false;
        const bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        const bool escRisingEdge = escDown && !s_spectateEscWasDown;
        s_spectateEscWasDown = escDown;

        if (escRisingEdge)
        {
            const int deadRole = g_localRoleFlag;
            const DWORD deadPid = g_revivalProcessId;
            LogSessionDiagnosticState("TickHook_spectatorEsc_entry");
            mod::Log(
                "TICK_HOOK: *** SPECTATOR ESC EXIT *** frameTick=%u "
                "role=%d pid=%lu — user requested spectate disconnect",
                g_frameTick,
                deadRole,
                static_cast<unsigned long>(deadPid));

            InterlockedExchange(&g_revivalExitMode,
                                static_cast<LONG>(g_localRoleFlag));
            InterlockedExchange(&g_revivalExitIntercepted, 1);

            NeutralizeRevivalSessionVtable();

            const bool initOk = ForceLocalPlayInit();
            mod::Log(
                "TICK_HOOK: spectator-esc step 1 ForceLocalPlayInit result=%d",
                initOk ? 1 : 0);

            if (g_revivalProcess != nullptr)
            {
                const BOOL termOk = TerminateProcess(g_revivalProcess, 0);
                const DWORD termErr = termOk ? 0 : GetLastError();
                CloseHandle(g_revivalProcess);
                g_revivalProcess = nullptr;
                g_revivalProcessId = 0;
                mod::Log(
                    "TICK_HOOK: spectator-esc step 2 helper terminated "
                    "(pid=%lu termOk=%d err=%lu)",
                    static_cast<unsigned long>(deadPid),
                    termOk ? 1 : 0,
                    static_cast<unsigned long>(termErr));
            }

            const bool patchOk = RestoreDllExitProcessPatches();
            mod::Log(
                "TICK_HOOK: spectator-esc step 3 RestoreDllExitProcessPatches result=%d",
                patchOk ? 1 : 0);

            const bool textOk = DisableRevivalTextRendering();
            mod::Log(
                "TICK_HOOK: spectator-esc step 4 DisableRevivalTextRendering result=%d",
                textOk ? 1 : 0);

            mod::ResetCrashRecoveryState();
            ResetGameModeValidation();
            mod::Log("TICK_HOOK: spectator-esc step 5 crash/validation state reset");

            const bool modeOk = ForceGameModeToTitle();
            mod::Log(
                "TICK_HOOK: spectator-esc step 6 ForceGameModeToTitle result=%d",
                modeOk ? 1 : 0);

            g_localInitAppliedForSession = false;
            mod::Log(
                "TICK_HOOK: spectator-esc recovery complete (was role=%d), "
                "next title-screen frame will consume exit interception",
                deadRole);
            LogSessionDiagnosticState("TickHook_spectatorEsc_exit");

            return 0;
        }
    }

    // ---- Hard-fallback watchdog -------------------------------------------
    // Detect Revival child process death that slipped past ExitProcess
    // interception and consoleErrorSerial detection.  This catches external
    // process kills (Task Manager, OS termination), crashes in non-DLL code
    // (unhandled SEH in the helper), and any scenario where the process
    // dies without going through our hooked ExitProcess or publishing a
    // console error.
    //
    // Uses a grace-period counter to avoid racing with the existing recovery
    // mechanisms (which fire synchronously within the same frame).  Only
    // triggers after kWatchdogGraceFrames consecutive frames of sustained
    // dead-process detection with no other recovery path having fired.
    // -----------------------------------------------------------------------
    if (g_dllExitProcessPatchesSaved
        && g_localRoleFlag != kLocalRoleLocalPlay
        && !g_tickRecoveryPending
        && !g_frameRecoveryPending
        && !g_deferredCancelCleanup)
    {
        const bool processWasCreated = (g_revivalProcess != nullptr);
        const bool processAlive = processWasCreated && IsPeerProcessAlive();
        const bool exitAlreadyPending =
            (InterlockedCompareExchange(&g_revivalExitIntercepted, 0, 0) != 0);

        if (!processAlive && processWasCreated && !exitAlreadyPending)
        {
            // Read screen index — only trigger on non-title screens.
            uint8_t wdScreen = 0;
            __try {
                wdScreen = *reinterpret_cast<const volatile uint8_t*>(0x00790148u);
            } __except (EXCEPTION_EXECUTE_HANDLER) {}

            if (wdScreen != 0)
            {
                ++g_watchdogDeadFrameCount;
                if (g_watchdogDeadFrameCount == 1)
                {
                    mod::Log(
                        "TICK_HOOK: WATCHDOG: Revival process dead, grace period "
                        "started (frameTick=%u screen=%u role=%d pid=%lu)",
                        g_frameTick,
                        static_cast<unsigned int>(wdScreen),
                        g_localRoleFlag,
                        static_cast<unsigned long>(g_revivalProcessId));
                }

                if (g_watchdogDeadFrameCount >= kWatchdogGraceFrames)
                {
                    const int deadRole = g_localRoleFlag;
                    const DWORD deadPid = g_revivalProcessId;
                    LogSessionDiagnosticState("TickHook_hardFallback_entry");
                    mod::Log(
                        "TICK_HOOK: *** HARD FALLBACK *** frameTick=%u "
                        "role=%d pid=%lu screen=%u — Revival process died "
                        "without ExitProcess/consoleError, performing "
                        "emergency cleanup",
                        g_frameTick,
                        deadRole,
                        static_cast<unsigned long>(deadPid),
                        static_cast<unsigned int>(wdScreen));

                    // Synthesize exit interception so ConsumeRevivalExitInterception
                    // fires on the next title-screen frame and routes to the
                    // netplay menu (with lobby NotifyEndMatch if applicable).
                    InterlockedExchange(&g_revivalExitMode,
                                        static_cast<LONG>(g_localRoleFlag));
                    InterlockedExchange(&g_revivalExitIntercepted, 1);

                    NeutralizeRevivalSessionVtable();

                    // Step 1: Reinstate a live local-play session.
                    const bool initOk = ForceLocalPlayInit();
                    mod::Log(
                        "TICK_HOOK: hard-fallback step 1 ForceLocalPlayInit "
                        "result=%d",
                        initOk ? 1 : 0);

                    // Step 2: Terminate / close the dead helper process.
                    if (g_revivalProcess != nullptr)
                    {
                        const BOOL termOk =
                            TerminateProcess(g_revivalProcess, 0);
                        const DWORD termErr = termOk ? 0 : GetLastError();
                        CloseHandle(g_revivalProcess);
                        g_revivalProcess = nullptr;
                        g_revivalProcessId = 0;
                        mod::Log(
                            "TICK_HOOK: hard-fallback step 2 helper terminated "
                            "(pid=%lu termOk=%d err=%lu)",
                            static_cast<unsigned long>(deadPid),
                            termOk ? 1 : 0,
                            static_cast<unsigned long>(termErr));
                    }
                    else
                    {
                        mod::Log(
                            "TICK_HOOK: hard-fallback step 2 skipped "
                            "(no helper handle)");
                    }

                    // Step 3: Restore DLL Jcc patches.
                    const bool patchOk = RestoreDllExitProcessPatches();
                    mod::Log(
                        "TICK_HOOK: hard-fallback step 3 "
                        "RestoreDllExitProcessPatches result=%d",
                        patchOk ? 1 : 0);

                    // Step 4: Disable stale text overlays.
                    const bool textOk = DisableRevivalTextRendering();
                    mod::Log(
                        "TICK_HOOK: hard-fallback step 4 "
                        "DisableRevivalTextRendering result=%d",
                        textOk ? 1 : 0);

                    // Step 5: Reset crash/validation state.
                    mod::ResetCrashRecoveryState();
                    ResetGameModeValidation();
                    mod::Log(
                        "TICK_HOOK: hard-fallback step 5 "
                        "crash/validation state reset");

                    // Step 6: Force game mode to title screen.
                    const bool modeOk = ForceGameModeToTitle();
                    mod::Log(
                        "TICK_HOOK: hard-fallback step 6 "
                        "ForceGameModeToTitle result=%d",
                        modeOk ? 1 : 0);

                    g_localInitAppliedForSession = false;
                    g_watchdogDeadFrameCount = 0;
                    mod::Log(
                        "TICK_HOOK: hard-fallback recovery complete "
                        "(was role=%d), next title-screen frame will "
                        "consume exit interception",
                        deadRole);
                    LogSessionDiagnosticState("TickHook_hardFallback_exit");

                    return 0;
                }
            }
            else
            {
                // On title screen — existing title-hook mechanisms handle it.
                g_watchdogDeadFrameCount = 0;
            }
        }
        else
        {
            // Process alive, no process, or exit already pending.
            g_watchdogDeadFrameCount = 0;
        }
    }
    else
    {
        // Not in an active session or recovery already in progress.
        g_watchdogDeadFrameCount = 0;
    }

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

    // Pulse a lightweight export tick every frame so that activityPhase,
    // inNetplayMenu, stateSeq, and all other shared-memory fields remain
    // current during loading (screenIdx=2) and battle (screenIdx=3) — screens
    // that have no title/charselect hook calling the full session_bridge::Tick().
    // TickExportOnly() only calls state_export::Update(g_status) under the
    // bridge mutex; it does NOT call takeover::Tick() and is safe here.
    netplay::bridge::TickExportOnly();

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

void ArmOnlineMatchEscGracefulQuit()
{
    InterlockedExchange(&g_onlineMatchEscGracefulQuitArmed, 1);
}

bool ConsumeOnlineMatchEscGracefulQuit()
{
    return InterlockedExchange(&g_onlineMatchEscGracefulQuitArmed, 0) != 0;
}

void ResetOnlineMatchEscGracefulQuit()
{
    InterlockedExchange(&g_onlineMatchEscGracefulQuitArmed, 0);
}

static bool EnsureQuitRingHeader()
{
    if (g_quitRingHeader != nullptr)
    {
        return true;
    }

    HANDLE hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, "Quit");
    if (hMap == nullptr)
    {
        return false;
    }

    auto* header = static_cast<volatile LONG*>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, 8));
    if (header == nullptr)
    {
        CloseHandle(hMap);
        return false;
    }

    g_quitRingHeader = header;
    CloseHandle(hMap);
    return true;
}

static void ReleaseQuitRingHeader()
{
    if (g_quitRingHeader != nullptr)
    {
        UnmapViewOfFile(const_cast<LONG*>(g_quitRingHeader));
        g_quitRingHeader = nullptr;
    }
}

static bool ConsumeGracefulQuitRingSignal(LONG* outHeadBefore, LONG* outTailBefore)
{
    if (outHeadBefore != nullptr)
    {
        *outHeadBefore = 0;
    }
    if (outTailBefore != nullptr)
    {
        *outTailBefore = 0;
    }

    if (!EnsureQuitRingHeader())
    {
        return false;
    }

    auto* headPtr = const_cast<LONG*>(&g_quitRingHeader[0]);
    auto* tailPtr = const_cast<LONG*>(&g_quitRingHeader[1]);
    const LONG headBefore = InterlockedCompareExchange(headPtr, 0, 0);
    const LONG tailBefore = InterlockedCompareExchange(tailPtr, 0, 0);

    if (outHeadBefore != nullptr)
    {
        *outHeadBefore = headBefore;
    }
    if (outTailBefore != nullptr)
    {
        *outTailBefore = tailBefore;
    }

    if (headBefore == tailBefore)
    {
        return false;
    }

    // Consume the pending graceful-quit signal once we decide to replace
    // Revival's patched-out quitMem -> ExitProcess path on the host side.
    InterlockedExchange(headPtr, tailBefore);
    MemoryBarrier();
    return true;
}

static char RecoverFromQuitRingSignal(const char* phaseTag, LONG quitHeadBefore, LONG quitTailBefore)
{
    const int deadRole = g_localRoleFlag;
    const DWORD deadPid = g_revivalProcessId;
    LogSessionDiagnosticState("TickHook_gracefulQuit_entry");
    mod::Log(
        "TICK_HOOK: *** %s GRACEFUL SESSION END *** frameTick=%u "
        "role=%d pid=%lu quitHead=%ld quitTail=%ld — synthesizing exit interception",
        phaseTag != nullptr ? phaseTag : "POST-TICK",
        g_frameTick,
        deadRole,
        static_cast<unsigned long>(deadPid),
        static_cast<long>(quitHeadBefore),
        static_cast<long>(quitTailBefore));

    InterlockedExchange(&g_revivalExitMode, static_cast<LONG>(g_localRoleFlag));
    InterlockedExchange(&g_revivalExitIntercepted, 1);

    NeutralizeRevivalSessionVtable();

    const bool initOk = ForceLocalPlayInit();
    mod::Log(
        "TICK_HOOK: graceful-quit step 1 ForceLocalPlayInit result=%d",
        initOk ? 1 : 0);

    if (g_revivalProcess != nullptr)
    {
        const BOOL termOk = TerminateProcess(g_revivalProcess, 0);
        const DWORD termErr = termOk ? 0 : GetLastError();
        CloseHandle(g_revivalProcess);
        g_revivalProcess = nullptr;
        g_revivalProcessId = 0;
        mod::Log(
            "TICK_HOOK: graceful-quit step 2 helper terminated "
            "(pid=%lu termOk=%d err=%lu)",
            static_cast<unsigned long>(deadPid),
            termOk ? 1 : 0,
            static_cast<unsigned long>(termErr));
    }

    const bool patchOk = RestoreDllExitProcessPatches();
    mod::Log(
        "TICK_HOOK: graceful-quit step 3 RestoreDllExitProcessPatches result=%d",
        patchOk ? 1 : 0);

    const bool textOk = DisableRevivalTextRendering();
    mod::Log(
        "TICK_HOOK: graceful-quit step 4 DisableRevivalTextRendering result=%d",
        textOk ? 1 : 0);

    mod::ResetCrashRecoveryState();
    ResetGameModeValidation();
    mod::Log("TICK_HOOK: graceful-quit step 5 crash/validation state reset");

    const bool modeOk = ForceGameModeToTitle();
    mod::Log(
        "TICK_HOOK: graceful-quit step 6 ForceGameModeToTitle result=%d",
        modeOk ? 1 : 0);

    g_localInitAppliedForSession = false;
    mod::Log(
        "TICK_HOOK: graceful-quit recovery complete (was role=%d), "
        "next title-screen frame will consume exit interception",
        deadRole);
    LogSessionDiagnosticState("TickHook_gracefulQuit_exit");
    return 0;
}

// Reset the per-frame validator state.  Called when a session ends so the
// next session gets fresh validation.
void ResetGameModeValidation()
{
    g_frameTick = 0;
    g_perFrameMismatchLogged = false;
    g_spectateHoldoffLogged = false;
    g_spectateHoldoffWasActive = false;

    // Reset double-speed diagnostic state for the new session.
    g_lastHeartbeatTimeMs = 0;
    g_lastHeartbeatFrameTick = 0;
    g_lastToggleValue = 0xFFFFFFFFu;
    g_toggleSameCount = 0;
    g_toggleDiagLogged = false;
    ResetOnlineMatchEscGracefulQuit();
    ReleaseQuitRingHeader();

    // Increment session number and reset cross-session change-detection state.
    ++g_sessionNumber;
    g_timerBaselineCaptured = false;
    g_baselineTimerScalar   = 0.0;
    g_baselineTimerInterval = 0.0;
    g_baselineTimerPtr      = 0;
    g_initOnceGuardTracked  = false;
    g_lastInitOnceGuard     = 0;
    g_renderCtxTracked      = false;
    g_lastRenderCtxPtr      = 0;
    g_sessionPtrTracked     = false;
    g_lastSessionPtrInTick  = 0;
    g_prevFrameQpcValid     = false;
    memset(&g_prevFrameQpc, 0, sizeof(g_prevFrameQpc));

    mod::Log("ResetGameModeValidation: session #%u starting", g_sessionNumber);

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
    if (g_tickRecoveryPending)
    {
        mod::Log("ResetGameModeValidation: clearing stale g_tickRecoveryPending");
        g_tickRecoveryPending = false;
    }
    if (g_watchdogDeadFrameCount != 0)
    {
        mod::Log("ResetGameModeValidation: clearing stale g_watchdogDeadFrameCount=%u",
                 g_watchdogDeadFrameCount);
        g_watchdogDeadFrameCount = 0;
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
    const uintptr_t frameHookRva = g_activeRevival->frameHookRva;
    uint8_t* const  target  = reinterpret_cast<uint8_t*>(base + frameHookRva);

    // --- Dump bytes at EXE 0x401642 for double-speed investigation ----------
    // Revival's init-time handler patches 8 bytes at 0x401642 in the EXE's
    // main loop via a REPLACEMENT hook (sub_1006FAA0).  If these bytes are
    // corrupted or restored to original, the main loop's screen update would
    // run in addition to Revival's tick → doubled game speed.
    {
        constexpr uintptr_t kHookAddr = 0x00401642;
        uint8_t hookBytes[16] = {};
        __try {
            memcpy(hookBytes, reinterpret_cast<const void*>(kHookAddr), 16);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(hookBytes, 0xCC, sizeof(hookBytes));
        }
        mod::Log(
            "InstallNetplayFrameHook: EXE 0x401642 bytes (16): "
            "[%02X %02X %02X %02X %02X %02X %02X %02X "
            " %02X %02X %02X %02X %02X %02X %02X %02X]",
            hookBytes[0], hookBytes[1], hookBytes[2], hookBytes[3],
            hookBytes[4], hookBytes[5], hookBytes[6], hookBytes[7],
            hookBytes[8], hookBytes[9], hookBytes[10], hookBytes[11],
            hookBytes[12], hookBytes[13], hookBytes[14], hookBytes[15]);
    }

    // Sanity-check: expect push ebp (0x55) as the first byte.
    uint8_t firstByte = 0;
    if (!SafeReadByte(target, &firstByte) || firstByte != 0x55)
    {
        mod::Log(
            "InstallNetplayFrameHook: unexpected byte 0x%02X at RVA 0x%lX — skipping",
            static_cast<unsigned>(firstByte),
            static_cast<unsigned long>(frameHookRva));
        return false;
    }

    // Build trampoline: 6 original bytes + JMP-near back to original+6.
    memcpy(g_frameHookTrampoline, target, 6);
    const uintptr_t origContinue = base + frameHookRva + 6;
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
        static_cast<unsigned long>(frameHookRva),
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
        const uintptr_t perFrameTickRva = g_activeRevival->perFrameTickRva;
        uint8_t* const tickTarget =
            reinterpret_cast<uint8_t*>(base + perFrameTickRva);

        uint8_t tickFirstByte = 0;
        if (!SafeReadByte(tickTarget, &tickFirstByte)
            || (tickFirstByte != 0x55 && tickFirstByte != 0x51))
        {
            mod::Log(
                "InstallNetplayFrameHook: per-frame tick unexpected byte "
                "0x%02X at RVA 0x%lX — skipping",
                static_cast<unsigned>(tickFirstByte),
                static_cast<unsigned long>(perFrameTickRva));
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

            const uintptr_t tickContinue = base + perFrameTickRva + 6;
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
                        static_cast<unsigned long>(perFrameTickRva),
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

    // -----------------------------------------------------------------------
    // Battle resource cleanup — replicate the EFZ battle screen's exit
    // cleanup that we bypass when force-transitioning mid-match.
    //
    // Verified against the EXE's own replay exit path in
    // updateBattleScreenLogic (0x763C20), byte[45]==2 replay branch:
    //   1. safelyCloseFileHandle(gameSys+82564) — close replay file
    //   2. gameSys+82563 = 0                   — clear replay I/O state
    //   3. stopBackgroundMusic(gameSys)         — stop battle BGM
    //   4. cleanupPlayerObject(P1, 1)           — release surfaces/sounds/free
    //   5. null P1 slot
    //   6. cleanupPlayerObject(P2, 1)
    //   7. null P2 slot
    //   8. free(gameSys+4988)                   — free animated stage-bg
    //   9. gameSys+4988 = 0
    //
    // We additionally reset:
    //   - battleObj+0x578 (game speed) back to 3 (default)
    //   - gameSys+82540 (fade controller byte) to 0
    //   - gameSys+82556 (speed override flag) to 0
    //   - battleObj byte[44]=1, byte[45]=0 (screen reinit flags)
    //
    // All addresses verified via capstone disassembly of efz.exe.
    // -----------------------------------------------------------------------
    __try
    {
        if (currentGameMode == 2  // loading screen
            || currentGameMode == 3  // battle screen
            || currentGameMode == 5) // results screen
        {
            constexpr uintptr_t kScreenTable   = 0x00790110;
            constexpr uintptr_t kGameSystemPtr = 0x0079010C;

            const uint32_t battleObj =
                reinterpret_cast<const uint32_t*>(kScreenTable)[3];
            const uint32_t gameSys =
                *reinterpret_cast<const uint32_t*>(kGameSystemPtr);

            if (battleObj != 0 && gameSys != 0)
            {
                // ---- Close replay file if active ---------------------------
                // safelyCloseFileHandle: __thiscall at 0x405F50.
                // Checks [this+4] for a valid handle, calls CloseHandle, nulls.
                const int8_t replayState =
                    *reinterpret_cast<const int8_t*>(gameSys + 82563);
                if (replayState > 0)
                {
                    using CloseFileFn = void*(__thiscall*)(void* fileStruct);
                    constexpr uintptr_t kCloseFileAddr = 0x00405F50;
                    auto const closeFile =
                        reinterpret_cast<CloseFileFn>(kCloseFileAddr);
                    closeFile(reinterpret_cast<void*>(gameSys + 82564));
                    *reinterpret_cast<int8_t*>(gameSys + 82563) = 0;
                    mod::Log(
                        "ForceGameModeToTitle: closed replay file "
                        "(replayState was %d)",
                        static_cast<int>(replayState));
                }

                // ---- Stop battle BGM ---------------------------------------
                // stopBackgroundMusic: __thiscall at 0x406A10.
                using StopBgmFn = int(__thiscall*)(void* gameSys);
                constexpr uintptr_t kStopBgmAddr = 0x00406A10;
                auto const stopBgm =
                    reinterpret_cast<StopBgmFn>(kStopBgmAddr);
                stopBgm(reinterpret_cast<void*>(gameSys));

                // ---- Clean up character objects via the EXE's own function --
                // cleanupPlayerObject: __thiscall at 0x401920.
                //   Calls cleanupCharacterObject (releases DD surfaces, 50
                //   sound buffers, resource arrays), then j__free(this) when
                //   freeMemory & 1.
                using CleanupPlayerFn =
                    void(__thiscall*)(void* playerObj, char freeMemory);
                constexpr uintptr_t kCleanupPlayerAddr = 0x00401920;
                auto const cleanupPlayer =
                    reinterpret_cast<CleanupPlayerFn>(kCleanupPlayerAddr);

                // battleObj+0x14 (20) = P1 slot ptr, +0x18 (24) = P2 slot ptr
                for (int pIdx = 0; pIdx < 2; ++pIdx)
                {
                    const uint32_t slotAddr =
                        *reinterpret_cast<const uint32_t*>(
                            battleObj + 0x14 + static_cast<uint32_t>(pIdx) * 4);
                    if (slotAddr != 0)
                    {
                        const uint32_t playerObj =
                            *reinterpret_cast<const uint32_t*>(slotAddr);
                        if (playerObj != 0)
                        {
                            cleanupPlayer(
                                reinterpret_cast<void*>(playerObj), 1);
                            *reinterpret_cast<uint32_t*>(slotAddr) = 0;
                            mod::Log(
                                "ForceGameModeToTitle: cleaned up P%d "
                                "obj=0x%08lX (slot=0x%08lX)",
                                pIdx + 1,
                                static_cast<unsigned long>(playerObj),
                                static_cast<unsigned long>(slotAddr));
                        }
                    }
                }

                // ---- Free the animated stage-bg handler --------------------
                // Uses j__free at 0x777E20 (the EXE's own CRT free), matching
                // the EXE's own cleanup at 0x763FE4.
                auto* const bgObjPtr =
                    reinterpret_cast<uint32_t*>(gameSys + 4988);
                if (*bgObjPtr != 0)
                {
                    using ExeFreeFn = void(__cdecl*)(void* ptr);
                    constexpr uintptr_t kExeFreeAddr = 0x00777E20;
                    auto const exeFree =
                        reinterpret_cast<ExeFreeFn>(kExeFreeAddr);
                    const uint32_t bgObj = *bgObjPtr;
                    exeFree(reinterpret_cast<void*>(bgObj));
                    *bgObjPtr = 0;
                    mod::Log(
                        "ForceGameModeToTitle: freed animated bg "
                        "obj=0x%08lX via EXE j__free",
                        static_cast<unsigned long>(bgObj));
                }

                // ---- Reset game speed to default ---------------------------
                // battleObj+0x578 (1400) = game speed (frames per update).
                // Default is 3; spectating/practice can change it.
                *reinterpret_cast<uint8_t*>(battleObj + 0x578) = 3;

                // ---- Reset fade & speed-override controllers ---------------
                // gameSys+82540 (byte): 1=lighten, -1=darken, 0=off.
                // gameSys+82556 (dword): speed override pending flag.
                *reinterpret_cast<uint8_t*>(gameSys + 82540) = 0;
                *reinterpret_cast<uint32_t*>(gameSys + 82556) = 0;

                // ---- Reset battle screen init/exit flags -------------------
                *reinterpret_cast<uint8_t*>(battleObj + 44) = 1;  // reinit
                *reinterpret_cast<uint8_t*>(battleObj + 45) = 0;  // clear exit

                mod::Log(
                    "ForceGameModeToTitle: battle resources cleaned up "
                    "(screen=%d battleObj=0x%08lX gameSys=0x%08lX)",
                    currentGameMode,
                    static_cast<unsigned long>(battleObj),
                    static_cast<unsigned long>(gameSys));

                // ---- Black out the hardware palette immediately ------------
                // Between this function returning and EnterNetplayMenu firing
                // on the next title-screen update frame, the EXE's main loop
                // renders at least one frame via HookedTitleRenderImpl.
                // Without blacking out the palette here, that frame displays
                // the stale battle back-buffer with the battle's 8-bit palette
                // → garbled blue/black/white corruption visible to the user.
                //
                // Fix: zero the title screen's software palette buffer (256
                // BGRA entries at screenObj+46) and call the EXE's own
                // setPalette to upload it to the hardware IDirectDrawPalette.
                // This makes any intermediate frame render as solid black.
                const uint32_t titleObj =
                    reinterpret_cast<const uint32_t*>(kScreenTable)[0];
                if (titleObj != 0)
                {
                    // kOffsetPalette = 46, palette is 256 * 4 = 1024 bytes
                    memset(reinterpret_cast<void*>(titleObj + 46), 0, 1024);

                    // kOffsetGraphicsContext = 0x20
                    void** gfxCtx =
                        *reinterpret_cast<void***>(titleObj + 0x20);
                    if (gfxCtx != nullptr)
                    {
                        using SetPaletteFn =
                            int(__thiscall*)(void** ctx, int paletteData);
                        constexpr uintptr_t kSetPaletteAddr = 0x0040BD30;
                        auto const setPal =
                            reinterpret_cast<SetPaletteFn>(kSetPaletteAddr);
                        setPal(gfxCtx, static_cast<int>(titleObj + 46));
                        mod::Log(
                            "ForceGameModeToTitle: blacked out hardware "
                            "palette via title screen obj=0x%08lX gfx=0x%08lX",
                            static_cast<unsigned long>(titleObj),
                            reinterpret_cast<unsigned long>(gfxCtx));
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "ForceGameModeToTitle: SEH exception during battle resource "
            "cleanup (screen=%d)", currentGameMode);
    }

    // Reset the charselect screen's init/exit flags so it properly
    // re-initializes on next entry.  The DLL's init(mode=2) does NOT
    // restore these EXE-side screen flags, so without this the charselect
    // screen skips its initializeCharacterSelectScreen call and renders
    // incorrectly when the user enters any local game mode (e.g. practice)
    // after a netplay disconnect recovery.
    //
    // Screen table at 0x00790110; index 1 = charselect object.
    // Offset +44 = init-required flag (1 = re-init on next frame).
    // Offset +45 = exit flag (0 = cleared, prevents premature exit).
    __try
    {
        constexpr uintptr_t kScreenTable = 0x00790110;
        const uint32_t csObj =
            reinterpret_cast<const uint32_t*>(kScreenTable)[1];
        if (csObj != 0)
        {
            const uint8_t oldInit = *reinterpret_cast<const uint8_t*>(csObj + 44);
            const uint8_t oldExit = *reinterpret_cast<const uint8_t*>(csObj + 45);
            *reinterpret_cast<uint8_t*>(csObj + 44) = 1;  // init required
            *reinterpret_cast<uint8_t*>(csObj + 45) = 0;  // exit cleared
            mod::Log(
                "ForceGameModeToTitle: charselect screen reset "
                "init %u->1 exit %u->0 (obj=0x%08lX)",
                static_cast<unsigned>(oldInit),
                static_cast<unsigned>(oldExit),
                static_cast<unsigned long>(csObj));
        }

        // Also reset the title screen's init byte so the original EFZ
        // update handler (case 1) re-applies setPalette on re-entry.
        // Without this, stale init state from the pre-match title screen
        // can cause palette mismatches after disconnect recovery.
        const uint32_t titleObj =
            reinterpret_cast<const uint32_t*>(kScreenTable)[0];
        if (titleObj != 0)
        {
            const uint8_t oldTitleInit = *reinterpret_cast<const uint8_t*>(titleObj + 44);
            *reinterpret_cast<uint8_t*>(titleObj + 44) = 1;  // init required
            *reinterpret_cast<uint8_t*>(titleObj + 45) = 0;  // exit cleared
            mod::Log(
                "ForceGameModeToTitle: title screen reset "
                "init %u->1 (obj=0x%08lX)",
                static_cast<unsigned>(oldTitleInit),
                static_cast<unsigned long>(titleObj));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log("ForceGameModeToTitle: SEH exception resetting screen flags");
    }

    // Reset game mode bytes (gameSystem + 4964/4965) from battle values
    // (e.g. mode=5, secondary=1) back to safe defaults.  Without this,
    // EFZ subsystems that read the game mode during title-screen updates
    // may behave incorrectly (e.g. the charselect intro sequence, BGM
    // selection, or input routing).
    __try
    {
        constexpr uintptr_t kGameSystemPtr = 0x0079010C;
        const uint32_t gameSys =
            *reinterpret_cast<const uint32_t*>(kGameSystemPtr);
        if (gameSys != 0)
        {
            auto* primaryMode = reinterpret_cast<uint8_t*>(gameSys + 4964);
            auto* secondaryMode = reinterpret_cast<uint8_t*>(gameSys + 4965);
            const uint8_t oldPrimary = *primaryMode;
            const uint8_t oldSecondary = *secondaryMode;
            *primaryMode = 0;    // reset to Arcade/default
            *secondaryMode = 0;  // reset secondary
            mod::Log(
                "ForceGameModeToTitle: game mode reset "
                "primary %u->0 secondary %u->0 (gameSys=0x%08lX)",
                static_cast<unsigned>(oldPrimary),
                static_cast<unsigned>(oldSecondary),
                static_cast<unsigned long>(gameSys));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log("ForceGameModeToTitle: SEH exception resetting game mode bytes");
    }

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
        "DIAG[%s]: S#%u dllExitPatchesSaved=%d tournamentPatchesSaved=%d "
        "renderContextSaved=%d savedRenderCtx=0x%08lX",
        ctx,
        g_sessionNumber,
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

        // Init-once guard, timer pointer, timer scalar — persistent across sessions
        uint16_t initOnceGuard = 0;
        uintptr_t timerPtr = 0;
        int initFlag = -1;
        double timerScalar = 0.0, timerInterval = 0.0;

        (void)SafeReadWord(
            reinterpret_cast<const void*>(base + g_activeRevival->initOnceGuardOffset),
            &initOnceGuard);
        (void)SafeReadPtr(
            reinterpret_cast<const void*>(base + g_activeRevival->timerPtrOffset),
            &timerPtr);
        (void)SafeReadInt(
            reinterpret_cast<const void*>(base + g_activeRevival->initFlagOffset),
            &initFlag);
        if (timerPtr != 0)
        {
            (void)SafeReadDouble(reinterpret_cast<const void*>(timerPtr + 16), &timerInterval);
            (void)SafeReadDouble(reinterpret_cast<const void*>(timerPtr + 32), &timerScalar);
        }
        mod::Log(
            "DIAG[%s]: initOnceGuard=0x%04X initFlag=%d timerPtr=0x%08lX "
            "timerScalar=%.4f timerInterval=%.4f",
            ctx,
            static_cast<unsigned>(initOnceGuard), initFlag,
            static_cast<unsigned long>(timerPtr),
            timerScalar, timerInterval);

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
                "DIAG[%s]: session vtable=0x%08lX vtableRVA=0x%lX initComplete=%d activePlayer=%d",
                ctx,
                static_cast<unsigned long>(vtable),
                (base != 0 && vtable >= base) ? static_cast<unsigned long>(vtable - base) : 0UL,
                initComplete,
                activePlayer);

            // Game mode fields and sentinel
            int prevGameMode = -1, curGameMode = -1, matchStartFrame = -1;
            int advanceCounter = -1, syncFrameCounter = -1;
            uint32_t sentinelVal = 0;
            const uintptr_t gmBase = sessionPtr + g_activeRevival->sessionOffsetGameModeSnapshot;
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase), &prevGameMode);
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 4), &curGameMode);
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 8), &matchStartFrame);
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 12), &advanceCounter);
            (void)SafeReadInt(reinterpret_cast<const void*>(gmBase + 20), &syncFrameCounter);
            (void)SafeReadDword(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetSentinel),
                &sentinelVal);

            mod::Log(
                "DIAG[%s]: prevMode=%d curMode=%d matchStart=%d advCtr=%d syncFrame=%d "
                "sentinel=0x%08lX",
                ctx,
                prevGameMode, curGameMode, matchStartFrame,
                advanceCounter, syncFrameCounter,
                static_cast<unsigned long>(sentinelVal));

            // Ping struct + session fields relevant to double-speed bug.
            int inputDelay = -1, queuePlayer = -1, currentFrame = -1, matchId = -1;
            uint32_t pingStruct[4] = {0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF, 0xDEADBEEF};
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetInputDelay),
                &inputDelay);
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetQueuePlayer),
                &queuePlayer);
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetCurrentFrame),
                &currentFrame);
            (void)SafeReadInt(
                reinterpret_cast<const void*>(sessionPtr + g_activeRevival->sessionOffsetMatchId),
                &matchId);
            const uintptr_t pingBase = sessionPtr + g_activeRevival->sessionOffsetPingStructBase;
            for (int pi = 0; pi < 4; ++pi)
            {
                (void)SafeReadInt(
                    reinterpret_cast<const void*>(pingBase + pi * 4),
                    reinterpret_cast<int*>(&pingStruct[pi]));
            }
            mod::Log(
                "DIAG[%s]: session frame=%d matchId=%d inputDelay=%d queuePlayer=%d "
                "ping[0]=%u ping[1]=%u ping[2]=%u ping[3]=%u",
                ctx,
                currentFrame, matchId, inputDelay, queuePlayer,
                pingStruct[0], pingStruct[1], pingStruct[2], pingStruct[3]);
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
        "INIT_SNAP[%s]: S#%u roleFlag=%d sessionPtr=0x%08lX initFlag=%d initByte=0x%02X "
        "renderCtx=0x%08lX renderCtxBase=0x%08lX initOnceGuard=0x%08X "
        "timerPtr=0x%08lX globalStatePtr=0x%08lX",
        ctx,
        g_sessionNumber,
        roleFlag,
        static_cast<unsigned long>(sessionPtr),
        initFlag,
        static_cast<unsigned>(initByte),
        static_cast<unsigned long>(renderCtx),
        static_cast<unsigned long>(renderCtxBase),
        static_cast<unsigned>(initOnceGuard),
        static_cast<unsigned long>(timerPtr),
        static_cast<unsigned long>(globalStatePtr));

    // ---- Timer scalar/interval (critical for double-speed bug) ----
    // If init() corrupts or re-initializes the timer, this before/after
    // diff will show the change.
    if (timerPtr != 0)
    {
        double snapInterval = 0.0, snapScalar = 0.0;
        (void)SafeReadDouble(
            reinterpret_cast<const void*>(timerPtr + 16), &snapInterval);
        (void)SafeReadDouble(
            reinterpret_cast<const void*>(timerPtr + 32), &snapScalar);
        mod::Log(
            "INIT_SNAP[%s]: S#%u timerScalar=%.6f timerInterval=%.6f "
            "(expect ~64/~15.6 at 64fps)",
            ctx, g_sessionNumber, snapScalar, snapInterval);
    }

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
