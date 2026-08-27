#include "netplay/interop/overlay_ipc.h"

#include <windows.h>

#include <cstring>

#include "logger.h"

namespace netplay::interop::ipc
{
namespace
{
// Versioned name so an ABI change to OverlayIpcBlock cannot alias an old block.
constexpr char kMappingName[] = "Local\\EFZMOD_OverlayIPC_v1";

HANDLE g_mapHandle = nullptr;
OverlayIpcBlock* g_block = nullptr;

// Full barrier between the slot writes and the index publish. x86 total-store
// order would let a plain compiler barrier suffice for this SPSC pattern, but
// MemoryBarrier() is portable across the toolset and the ring is low-frequency
// (UDP-rate), so the extra fence cost is irrelevant.
inline void PublishBarrier()
{
    MemoryBarrier();
}
} // namespace

bool Attach(bool asHelper)
{
    if (g_block != nullptr)
    {
        if (asHelper) g_block->helperAttached = 1u;
        else          g_block->gameAttached = 1u;
        return true;
    }

    // CreateFileMapping opens the existing mapping if it already exists (the
    // other process got here first) and reports ERROR_ALREADY_EXISTS; either way
    // we get a valid handle to the same block.
    g_mapHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(OverlayIpcBlock)), kMappingName);
    if (g_mapHandle == nullptr)
    {
        mod::Log("OverlayIpc: CreateFileMapping failed err=%lu",
                 static_cast<unsigned long>(GetLastError()));
        return false;
    }
    const bool created = (GetLastError() != ERROR_ALREADY_EXISTS);

    g_block = static_cast<OverlayIpcBlock*>(MapViewOfFile(
        g_mapHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OverlayIpcBlock)));
    if (g_block == nullptr)
    {
        mod::Log("OverlayIpc: MapViewOfFile failed err=%lu",
                 static_cast<unsigned long>(GetLastError()));
        CloseHandle(g_mapHandle);
        g_mapHandle = nullptr;
        return false;
    }

    if (created)
    {
        // First attacher zero-inits and stamps the header. MapViewOfFile of a
        // fresh mapping is already zero-filled, but be explicit for clarity.
        std::memset(g_block, 0, sizeof(OverlayIpcBlock));
        g_block->magic = kBlockMagic;
        g_block->version = kBlockVersion;
    }
    else if (g_block->magic != kBlockMagic || g_block->version != kBlockVersion)
    {
        // A stale/foreign block under the same name: refuse to use it rather
        // than scribble. (Name is versioned, so this is defensive only.)
        mod::Log("OverlayIpc: block magic/version mismatch (0x%08lX v%lu) - detaching",
                 static_cast<unsigned long>(g_block->magic),
                 static_cast<unsigned long>(g_block->version));
        UnmapViewOfFile(g_block);
        g_block = nullptr;
        CloseHandle(g_mapHandle);
        g_mapHandle = nullptr;
        return false;
    }

    if (asHelper) g_block->helperAttached = 1u;
    else          g_block->gameAttached = 1u;

    mod::Log("OverlayIpc: attached as %s (created=%d block=%p size=%u)",
             asHelper ? "helper" : "game", created ? 1 : 0,
             static_cast<void*>(g_block),
             static_cast<unsigned>(sizeof(OverlayIpcBlock)));
    return true;
}

void Detach(bool asHelper)
{
    if (g_block != nullptr)
    {
        if (asHelper) g_block->helperAttached = 0u;
        else          g_block->gameAttached = 0u;
        UnmapViewOfFile(g_block);
        g_block = nullptr;
    }
    if (g_mapHandle != nullptr)
    {
        CloseHandle(g_mapHandle);
        g_mapHandle = nullptr;
    }
}

bool IsAttached()
{
    return g_block != nullptr;
}

OverlayIpcBlock* Block()
{
    return g_block;
}

bool Push(Ring& ring, const std::uint8_t* data, std::uint32_t len)
{
    if (data == nullptr || len == 0u || len > kSlotBytes)
    {
        return false;
    }
    const std::uint32_t head = ring.head;
    const std::uint32_t tail = ring.tail;
    // Full when advancing head would collide with tail (leave one slot open so
    // head==tail unambiguously means empty).
    if (((head + 1u) & kRingMask) == (tail & kRingMask))
    {
        return false;   // ring full - drop (loss-tolerant channel)
    }
    Slot& slot = ring.slots[head & kRingMask];
    std::memcpy(slot.data, data, len);
    slot.len = len;
    PublishBarrier();          // slot bytes + len committed before index moves
    ring.head = head + 1u;
    return true;
}

std::uint32_t Pop(Ring& ring, std::uint8_t* out, std::uint32_t cap)
{
    if (out == nullptr || cap == 0u)
    {
        return 0u;
    }
    const std::uint32_t tail = ring.tail;
    const std::uint32_t head = ring.head;
    if ((tail & kRingMask) == (head & kRingMask))
    {
        return 0u;             // empty
    }
    Slot& slot = ring.slots[tail & kRingMask];
    std::uint32_t len = slot.len;
    if (len == 0u || len > kSlotBytes)
    {
        // Corrupt/oversize slot: skip it rather than propagate garbage.
        PublishBarrier();
        ring.tail = tail + 1u;
        return 0u;
    }
    if (len > cap)
    {
        len = cap;             // truncate into caller's buffer
    }
    std::memcpy(out, slot.data, len);
    PublishBarrier();
    ring.tail = tail + 1u;
    return len;
}
} // namespace netplay::interop::ipc
