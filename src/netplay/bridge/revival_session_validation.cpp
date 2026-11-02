#include "netplay/bridge/takeover_internal.h"

#include <cstring>

namespace netplay::bridge::takeover
{
bool IsSessionPointerByVtable(uintptr_t sessionPtr, uintptr_t revivalImageBase, uintptr_t revivalImageEnd)
{
    if (sessionPtr < 0x00100000u)
    {
        return false;
    }

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

    uintptr_t vtable = 0;
    if (g_activeRevival == nullptr
        || !SafeReadPtr(reinterpret_cast<const void*>(sessionPtr), &vtable)
        || vtable < revivalImageBase || vtable >= revivalImageEnd)
    {
        return false;
    }
    const uintptr_t vtableRva = vtable - revivalImageBase;
    // Local session objects do not contain the Online delay/ping fields.
    if ((g_activeRevival->practiceSessionVtableRva != 0
         && vtableRva == g_activeRevival->practiceSessionVtableRva)
        || (g_activeRevival->tournamentSessionVtableRva != 0
            && vtableRva == g_activeRevival->tournamentSessionVtableRva))
    {
        return true;
    }

    if (g_activeRevival->versionTag != nullptr
        && std::strcmp(g_activeRevival->versionTag, "1.02j") == 0)
    {
        return vtableRva == g_activeRevival->tournamentSessionVtableRva
            || vtableRva == g_activeRevival->onlineSessionVtableRva
            || vtableRva == g_activeRevival->spectatorSessionVtableRva
            || vtableRva == g_activeRevival->replaySessionVtableRva
            || vtableRva == g_activeRevival->practiceSessionVtableRva;
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

}
