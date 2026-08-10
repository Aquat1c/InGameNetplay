#include "netplay/hooks/internal/shared.h"
#include "netplay/assets/assets.h"
#include "netplay/bridge/async_hosting.h"
#include "netplay/bridge/frontend_return.h"
#include "netplay/bridge/gameplay_exit_recovery.h"
#include "netplay/bridge/netplay_state_export.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/battle_log_menu.h"
#include "netplay/core/input_utils.h"
#include "netplay/core/mod_settings.h"
#include "netplay/core/network_capability.h"
#include "netplay/core/options_menu.h"
#include "netplay/core/player_rooms_menu.h"
#include "netplay/hooks/debug_overlay.h"

#include "logger.h"

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif

#include <dinput.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace netplay::hooks::internal
{
using namespace netplay::constants;
using NetplayMenuId = netplay::menu::NetplayMenuId;
using NetplayMenuAction = netplay::menu::NetplayMenuAction;
using NetplayMenuEntry = netplay::menu::NetplayMenuEntry;
using netplay::menu::GetDefaultSelectionForMenu;
using netplay::menu::GetMenuEntries;
using netplay::menu::MenuActionToString;
using netplay::menu::MenuIdToString;
using netplay::menu::RebuildLobbyMenuEntries;
using netplay::menu::RowIndexToString;
using InlineEditInputResult = netplay::inline_edit::InputResult;
using NetbridgeRole = netplay::bridge::NetbridgeRole;
using NetbridgePhase = netplay::bridge::NetbridgePhase;
using NetbridgeSpectatePromptKind = netplay::bridge::NetbridgeSpectatePromptKind;
using ReadConfigByteFn = uint8_t(__cdecl*)(uint8_t);
using LoadControlBindingsFn = BOOL(__thiscall*)(uint16_t*);
using LoadControlPresetFn = BOOL(__thiscall*)(uint16_t*, int);

void HandoffConnectedSessionToVsHumanState(uint32_t screenContext);
void HandoffSpectateSession(uint32_t screenContext);

namespace
{
constexpr int kDelaySelectionMin = 0;
constexpr int kDelaySelectionMax = 20;
// FUCK IT WE BALL type fix - I didn't manage to find a case where title screen had more than 17 frames in it
// Maybe some potato PC can produce this issue but at this point we can't do anything about it, got 17 frames even in 600 ping
// Pretty sure it's tied to the clock and not internal frames so we don't really care
constexpr int kVsHumanTitleWarmupFrames = 17;
constexpr int kSpectateTitleWarmupFrames = 17;
int g_pendingGlobalStateTransition = -1;
bool g_charSelectResetPending = false;
constexpr uint32_t kGameSystemOffsetP1WinState = 4920;
constexpr uint32_t kGameSystemOffsetP2WinState = 4924;
constexpr uint32_t kGameSystemOffsetCpuFlagP1 = 4931;
constexpr uint32_t kGameSystemOffsetCpuFlagP2 = 4932;
constexpr uint32_t kGameSystemOffsetRoundsCurrent = 4942;
constexpr uint32_t kGameSystemOffsetRoundsSetting = 4943;
constexpr uint32_t kGameSystemOffsetMatchCounter = 4952;
constexpr uint32_t kGameSystemOffsetMode = 4964;
constexpr uint32_t kGameSystemOffsetSecondaryModeFlag = 4965;
constexpr uint32_t kGameSystemOffsetContinueFlag = 4984;
constexpr uint32_t kGameSystemOffsetStageSelection = 4985;
constexpr uint32_t kGameSystemOffsetStageAnimState = 3884;
constexpr uint32_t kGameSystemOffsetStageAnim = 3888;
constexpr uint32_t kGameSystemOffsetStageCursor = 3890;
constexpr uint32_t kGameSystemOffsetReplaySessionFlag = 82563;
constexpr uintptr_t kVaScreenObjectTable = 0x00790110;
constexpr uint8_t kGameModeVsHuman = 4;
constexpr uint8_t kSecondaryModeFlagVsHuman = 4;
constexpr uint8_t kReplaySessionFlagCleared = 0;
constexpr int kRoleFlagSpectate = 1;            // matches kLocalRoleSpectate in takeover_internal.h
constexpr int kScreenIndexCharSelect = 1;       // EFZ screen table index for character select
constexpr int8_t kMenuSelectionReplay = 4;      // title menu "Replay" entry index
constexpr unsigned short kInvalidSoundBufferIndex = 150;
constexpr int kMenuInputFirstOffset = 12;
constexpr int kMenuInputLastOffset = 23;
constexpr size_t kFilteredMenuInputBytes = 24;
std::vector<int> g_seenLobbyChallengeIds;
unsigned short g_lobbyChallengeAlertBufferIndex = kInvalidSoundBufferIndex;
std::array<uint8_t, kFilteredMenuInputBytes> g_filteredMenuInputs = {};
std::array<uint8_t, kFilteredMenuInputBytes> g_unfocusedHeldMenuInputs = {};
std::array<uint8_t, 256> g_netplayHotkeyDown = {};
std::array<uint8_t, 2> g_joinWaitToSpectateButtonDown = {};
uint64_t g_lastAutoAnsweredSpectatePromptFingerprint = 0;
std::thread g_lobbySessionShutdownThread;
std::atomic<bool> g_lobbySessionShutdownInFlight{false};
std::atomic<bool> g_lobbySessionShutdownCompleted{false};
std::atomic<bool> g_lobbySessionShutdownReturnToPlayerRooms{false};
bool g_deferredLobbyRefreshPending = false;
// Absolute tick (GetTickCount) after which the deferred-refresh gate is
// released unconditionally.  Prevents a wedged bridge phase from leaving
// the lobby list perma-stale with no user-visible escape.
uint32_t g_deferredLobbyRefreshDeadlineTick = 0;
uint32_t g_lastRecoveryNoOverlaySuppressedTick = 0;
constexpr int kRecoveryMenuInputQuarantineFrames = 45;
bool g_recoveryMenuInputQuarantineActive = false;
bool g_recoveryMenuInputQuarantineLoggedSuppress = false;
bool g_recoveryMenuInputQuarantineLoggedHeldAfterWindow = false;
bool g_recoveryMenuInputQuarantineLoggedLeaveSuppress = false;
int g_recoveryMenuInputQuarantineFramesRemaining = 0;
constexpr uint32_t kDeferredLobbyRefreshTimeoutMs = 3000;
bool g_spectateHandoffWarmupActive = false;
int g_spectateHandoffWarmupFrames = 0;
uint32_t g_spectateHandoffWarmupStartTick = 0;
bool g_vsHumanHandoffWarmupActive = false;
int g_vsHumanHandoffWarmupFrames = 0;
uint32_t g_vsHumanHandoffWarmupStartTick = 0;
struct PendingLobbySpectateWait
{
    bool active = false;
    int trackedPlayerId = 0;
    std::string trackedPlayerName;
    int p1Id = 0;
    int p2Id = 0;
    std::string p1Name;
    std::string p2Name;
    std::string hostIp;
};
PendingLobbySpectateWait g_pendingLobbySpectateWait;

enum class HostDiscoveryOwner : uint8_t
{
    Direct = 0,
    LobbyChallenge,
};

struct PendingHostDiscovery
{
    uint64_t generation = 0;
    HostDiscoveryOwner owner = HostDiscoveryOwner::Direct;
    uint16_t port = 0;
    netplay::network::NetworkFamily originalPreferredFamily =
        netplay::network::NetworkFamily::IPv4;
    netplay::network::NetworkFamily discoveryFamily =
        netplay::network::NetworkFamily::IPv4;
    bool allowAlternateFamily = true;
    bool automaticFamilyRetryAttempted = false;
    std::string nickname;
    bool writeNicknameToIni = true;
    int targetPlayerId = 0;
    std::string targetName;
    // Immutable copy of a completed prewarmed local-family scan. The public
    // IP worker may read it, but it never starts or waits for a scan itself.
    bool capabilitySnapshotAvailable = false;
    netplay::network::LocalNetworkCapabilitySnapshot capabilitySnapshot;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> completed{false};
    netplay::lobby::PublicIpDiscoveryResult result;
};

struct PendingLobbyChallengePublish
{
    bool active = false;
    int targetPlayerId = 0;
    std::string targetName;
    std::string publicAddress;
    netplay::network::NetworkFamily preferredFamily =
        netplay::network::NetworkFamily::IPv4;
    netplay::network::NetworkFamily family =
        netplay::network::NetworkFamily::IPv4;
    uint16_t port = 0;
    bool automaticFamilyRetryAttempted = false;
};

std::shared_ptr<PendingHostDiscovery> g_pendingHostDiscovery;
PendingLobbyChallengePublish g_pendingLobbyChallengePublish;
uint64_t g_directHostDiscoveryGeneration = 0;

// Local family capability is collected independently of Host. The cache is
// advisory only: a missing/stale result never delays hosting, and Revival's
// actual listener acknowledgement remains the final authority.
constexpr DWORD kLocalNetworkCapabilityCacheMaxAgeMs = 30000u;

struct PendingLocalNetworkCapabilityScan
{
    std::atomic<bool> completed{false};
    netplay::network::LocalNetworkCapabilitySnapshot snapshot;
};

std::shared_ptr<PendingLocalNetworkCapabilityScan>
    g_pendingLocalNetworkCapabilityScan;
netplay::network::LocalNetworkCapabilitySnapshot
    g_cachedLocalNetworkCapability;
bool g_hasCachedLocalNetworkCapability = false;

struct HostDiscoveryThreadContext
{
    std::shared_ptr<PendingHostDiscovery> task;
    HMODULE moduleReference = nullptr;
};

DWORD WINAPI HostDiscoveryThreadMain(void* rawContext)
{
    // Public-address discovery is control-plane work. It may outlive the menu
    // frame that scheduled it, so always yield to EFZ's normal-priority game
    // thread without changing discovery timeouts or result ordering.
    (void)SetThreadPriority(
        GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    auto* context =
        static_cast<HostDiscoveryThreadContext*>(rawContext);
    HMODULE moduleReference =
        context != nullptr ? context->moduleReference : nullptr;
    std::shared_ptr<PendingHostDiscovery> task;
    if (context != nullptr)
    {
        task = std::move(context->task);
        delete context;
    }

    if (task)
    {
        try
        {
            task->result =
                netplay::lobby::DiscoverPublicIpSynchronously(
                    task->discoveryFamily,
                    &task->cancelled,
                    task->allowAlternateFamily,
                    task->capabilitySnapshotAvailable
                        ? &task->capabilitySnapshot
                        : nullptr);
        }
        catch (...)
        {
            task->result = {};
            task->result.preferredFamily =
                task->discoveryFamily;
            task->result.effectiveFamily =
                task->discoveryFamily;
            mod::Log(
                "PUBLIC_IP_DISCOVERY_WORKER_EXCEPTION "
                "generation=%llu family=%s",
                static_cast<unsigned long long>(
                    task->generation),
                netplay::network::FamilyName(
                    task->discoveryFamily));
        }
        task->completed.store(true, std::memory_order_release);
        task.reset();
    }

    // The worker owns a module reference acquired before CreateThread. Release
    // it atomically with thread exit so normal FreeLibrary cannot unmap this
    // code (or shut down logging) while discovery is still running.
    if (moduleReference != nullptr)
    {
        FreeLibraryAndExitThread(moduleReference, 0);
    }
    return 0;
}

struct LocalNetworkCapabilityThreadContext
{
    std::shared_ptr<PendingLocalNetworkCapabilityScan> task;
    HMODULE moduleReference = nullptr;
};

DWORD WINAPI LocalNetworkCapabilityThreadMain(void* rawContext)
{
    // This advisory scan must never compete at equal priority with EFZ's game
    // thread if it remains in flight across a menu or simulation transition.
    (void)SetThreadPriority(
        GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    auto* context =
        static_cast<LocalNetworkCapabilityThreadContext*>(rawContext);
    HMODULE moduleReference =
        context != nullptr ? context->moduleReference : nullptr;
    std::shared_ptr<PendingLocalNetworkCapabilityScan> task;
    if (context != nullptr)
    {
        task = std::move(context->task);
        delete context;
    }

    if (task)
    {
        try
        {
            task->snapshot =
                netplay::network::ScanLocalNetworkCapabilities();
        }
        catch (...)
        {
            task->snapshot = {};
            task->snapshot.ipv4.family =
                netplay::network::NetworkFamily::IPv4;
            task->snapshot.ipv6.family =
                netplay::network::NetworkFamily::IPv6;
            task->snapshot.completedTick = GetTickCount();
            mod::Log(
                "LOCAL_NETWORK_CAPABILITY_SCAN_EXCEPTION "
                "action=retain_unknown_capability");
        }
        task->completed.store(true, std::memory_order_release);
        task.reset();
    }

    // Keep this module mapped until the worker has finished touching its
    // code and task state. Do not join this worker from menu teardown or
    // DllMain; a completed result is simply consumed on a later menu entry.
    if (moduleReference != nullptr)
    {
        FreeLibraryAndExitThread(moduleReference, 0);
    }
    return 0;
}

static DWORD LocalNetworkCapabilitySnapshotAgeMs(
    const netplay::network::LocalNetworkCapabilitySnapshot& snapshot)
{
    return GetTickCount() - snapshot.completedTick;
}

static bool IsCachedLocalNetworkCapabilityFresh()
{
    return g_hasCachedLocalNetworkCapability
        && LocalNetworkCapabilitySnapshotAgeMs(
               g_cachedLocalNetworkCapability)
            <= kLocalNetworkCapabilityCacheMaxAgeMs;
}

static void LogLocalNetworkCapabilitySnapshot(
    const char* event,
    const netplay::network::LocalNetworkCapabilitySnapshot& snapshot)
{
    const auto LogFamily = [event, &snapshot](
                               const netplay::network::LocalFamilyCapability&
                                   capability) {
        mod::Log(
            "LOCAL_NETWORK_CAPABILITY_SCAN event=%s family=%s state=%s "
            "listenerStage=%s listenerError=%d addresses=%u "
            "strongestScope=%s routeAttempted=%d routeAvailable=%d "
            "routeUnavailable=%d routeError=%d routeSource=%s ageMs=%lu",
            event != nullptr ? event : "",
            netplay::network::FamilyName(capability.family),
            netplay::network::LocalFamilyCapabilityStateName(
                capability.state),
            netplay::network::ProbeStageName(
                capability.listenerProbe.stage),
            capability.listenerProbe.nativeError,
            capability.bindableAddressCount,
            netplay::network::LocalAddressScopeName(
                capability.strongestAddressScope),
            capability.routeProbeAttempted ? 1 : 0,
            capability.routeAvailable ? 1 : 0,
            capability.routeDefinitelyUnavailable ? 1 : 0,
            capability.routeNativeError,
            netplay::network::LocalAddressScopeName(
                capability.routeSourceScope),
            static_cast<unsigned long>(
                LocalNetworkCapabilitySnapshotAgeMs(snapshot)));
    };
    LogFamily(snapshot.ipv4);
    LogFamily(snapshot.ipv6);
}

static void PromoteCompletedLocalNetworkCapabilityScan()
{
    const std::shared_ptr<PendingLocalNetworkCapabilityScan> task =
        g_pendingLocalNetworkCapabilityScan;
    if (!task
        || !task->completed.load(std::memory_order_acquire))
    {
        return;
    }

    g_cachedLocalNetworkCapability = task->snapshot;
    g_hasCachedLocalNetworkCapability = true;
    if (g_pendingLocalNetworkCapabilityScan == task)
    {
        g_pendingLocalNetworkCapabilityScan.reset();
    }
    LogLocalNetworkCapabilitySnapshot(
        "completed", g_cachedLocalNetworkCapability);
}

static bool TryGetFreshLocalNetworkCapabilitySnapshot(
    netplay::network::LocalNetworkCapabilitySnapshot* outSnapshot,
    DWORD* outAgeMs = nullptr)
{
    PromoteCompletedLocalNetworkCapabilityScan();
    if (!g_hasCachedLocalNetworkCapability)
    {
        return false;
    }

    const DWORD ageMs = LocalNetworkCapabilitySnapshotAgeMs(
        g_cachedLocalNetworkCapability);
    if (outAgeMs != nullptr)
    {
        *outAgeMs = ageMs;
    }
    if (ageMs > kLocalNetworkCapabilityCacheMaxAgeMs)
    {
        return false;
    }
    if (outSnapshot != nullptr)
    {
        *outSnapshot = g_cachedLocalNetworkCapability;
    }
    return true;
}

static void EnsureLocalNetworkCapabilityScanScheduled(const char* trigger)
{
    PromoteCompletedLocalNetworkCapabilityScan();
    if (g_pendingLocalNetworkCapabilityScan
        || IsCachedLocalNetworkCapabilityFresh())
    {
        return;
    }

    std::shared_ptr<PendingLocalNetworkCapabilityScan> task;
    try
    {
        task = std::make_shared<PendingLocalNetworkCapabilityScan>();
    }
    catch (...)
    {
        mod::Log(
            "LOCAL_NETWORK_CAPABILITY_SCAN_BEGIN_FAILED trigger=%s "
            "reason=allocation",
            trigger != nullptr ? trigger : "");
        return;
    }

    HMODULE moduleReference = nullptr;
    const BOOL retainedModule = GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCSTR>(&LocalNetworkCapabilityThreadMain),
        &moduleReference);
    auto* context = retainedModule != FALSE
        ? new (std::nothrow) LocalNetworkCapabilityThreadContext()
        : nullptr;
    if (context == nullptr)
    {
        if (moduleReference != nullptr)
        {
            FreeLibrary(moduleReference);
        }
        mod::Log(
            "LOCAL_NETWORK_CAPABILITY_SCAN_BEGIN_FAILED trigger=%s "
            "reason=%s",
            trigger != nullptr ? trigger : "",
            retainedModule == FALSE
                ? "module_reference"
                : "allocation");
        return;
    }

    context->task = task;
    context->moduleReference = moduleReference;
    g_pendingLocalNetworkCapabilityScan = task;
    HANDLE worker = CreateThread(
        nullptr,
        0,
        &LocalNetworkCapabilityThreadMain,
        context,
        0,
        nullptr);
    if (worker == nullptr)
    {
        const DWORD error = GetLastError();
        delete context;
        g_pendingLocalNetworkCapabilityScan.reset();
        FreeLibrary(moduleReference);
        mod::Log(
            "LOCAL_NETWORK_CAPABILITY_SCAN_BEGIN_FAILED trigger=%s "
            "reason=CreateThread error=%lu",
            trigger != nullptr ? trigger : "",
            static_cast<unsigned long>(error));
        return;
    }
    CloseHandle(worker);
    mod::Log(
        "LOCAL_NETWORK_CAPABILITY_SCAN_BEGIN trigger=%s cacheAgeMs=%lu",
        trigger != nullptr ? trigger : "",
        g_hasCachedLocalNetworkCapability
            ? static_cast<unsigned long>(
                LocalNetworkCapabilitySnapshotAgeMs(
                    g_cachedLocalNetworkCapability))
            : 0ul);
}

void PlayLobbyChallengeAlert(uint32_t screenContext);

struct MenuControlBinding
{
    uint8_t device = 0;
    uint8_t code = 0;
};

struct MenuControlProfile
{
    uint8_t profileIndex = 0;
    std::array<std::array<MenuControlBinding, 8>, 2> players = {};
    std::array<uint8_t, 2> povMasks = {0, 0};
    bool hasRuntimeOnlyPadBindings = false;
    bool hasPovDirectionalOverrides = false;
};

MenuControlProfile g_menuControlProfile = {};
bool g_menuControlProfileLoaded = false;
bool g_menuControlRuntimeCompatActive = false;
InputSnapshot g_lastCompatInputSnapshot = {};
bool g_hasLoggedCompatInputSnapshot = false;
bool g_menuControlSyncAttempted = false;
uint8_t g_lastMenuControlSyncProfileIndex = 0xFFu;
std::array<uintptr_t, 2> g_menuLiveAxisRangeNormalizedDevicePtrs = {};
FILETIME g_lastIgcrOverrideWriteTime = {};
bool g_hasLastIgcrOverrideWriteTime = false;
DWORD g_nextMenuControlRefreshTick = 0;

constexpr uint8_t kMenuPollMaskRight = 1u << 0u;
constexpr uint8_t kMenuPollMaskLeft = 1u << 1u;
constexpr uint8_t kMenuPollMaskDown = 1u << 2u;
constexpr uint8_t kMenuPollMaskUp = 1u << 3u;
constexpr uint8_t kMenuDirectionalRuntimeMask =
    static_cast<uint8_t>(kMenuPollMaskRight | kMenuPollMaskLeft | kMenuPollMaskDown | kMenuPollMaskUp);
constexpr uint8_t kMenuNativePadButtonMinCode = 0x01u;
constexpr uint8_t kMenuNativePadButtonMaxCode = 0x10u;
constexpr uint8_t kMenuNativePadAxisUpCode = 0x11u;
constexpr uint8_t kMenuNativePadAxisDownCode = 0x12u;
constexpr uint8_t kMenuNativePadAxisLeftCode = 0x13u;
constexpr uint8_t kMenuNativePadAxisRightCode = 0x14u;
constexpr uint8_t kMenuExtendedPadButtonBaseCode = 0x21u;
constexpr uint8_t kMenuExtendedPadButtonCount = 16u;
constexpr uint8_t kMenuRightStickUpCode = 0x31u;
constexpr uint8_t kMenuRightStickDownCode = 0x32u;
constexpr uint8_t kMenuRightStickLeftCode = 0x33u;
constexpr uint8_t kMenuRightStickRightCode = 0x34u;
constexpr uint8_t kMenuZAxisNegativeCode = 0x35u;
constexpr uint8_t kMenuZAxisPositiveCode = 0x36u;
constexpr uint8_t kMenuRzAxisNegativeCode = 0x37u;
constexpr uint8_t kMenuRzAxisPositiveCode = 0x38u;
constexpr uint8_t kMenuSlider0NegativeCode = 0x39u;
constexpr uint8_t kMenuSlider0PositiveCode = 0x3Au;
constexpr uint8_t kMenuSlider1NegativeCode = 0x3Bu;
constexpr uint8_t kMenuSlider1PositiveCode = 0x3Cu;
constexpr uintptr_t kGameSystemInputManagerOffset = sizeof(uintptr_t);
constexpr uintptr_t kInputManagerMappingOffsetBytes = 448u;
constexpr uintptr_t kInputManagerPad1DeviceOffset = 492u;
constexpr uintptr_t kInputManagerPad2DeviceOffset = 496u;
constexpr uintptr_t kPad1StateOffset = 288u;
constexpr uintptr_t kPad2StateOffset = 368u;
constexpr uintptr_t kJoyStateZOffset = 8u;
constexpr uintptr_t kJoyStateRxOffset = 12u;
constexpr uintptr_t kJoyStateRyOffset = 16u;
constexpr uintptr_t kJoyStateRzOffset = 20u;
constexpr uintptr_t kJoyStateSlider0Offset = 24u;
constexpr uintptr_t kJoyStateSlider1Offset = 28u;
constexpr uintptr_t kPov0OffsetInJoyState = 32u;
constexpr uintptr_t kJoyStateButton0Offset = 48u;
constexpr LONG kMenuPadAxisThreshold = 500;
constexpr uint8_t kSelectedControlProfileConfigByte = 0x9Cu;
constexpr size_t kIgcrProfileCount = 6u;
constexpr size_t kIgcrSlotBytes = 4u + (2u * 8u * 2u);
constexpr size_t kIgcrHeaderBytes = 10u;
constexpr size_t kIgcrExpectedFileBytes = kIgcrHeaderBytes + (kIgcrProfileCount * kIgcrSlotBytes);
constexpr std::array<uint8_t, 8> kIgcrOverrideMagic = {'I', 'G', 'C', 'R', 'O', 'V', 'R', '1'};
constexpr std::array<int, 8> kMenuActionToRuntimeWordIndex = {3, 2, 1, 0, 4, 5, 6, 7};

uint8_t ClampMenuControlProfileIndex(uint8_t profile)
{
    return profile <= 5u ? profile : 0u;
}

bool IsMenuDirectionalAction(int action)
{
    return action >= 0 && action < 4;
}

uint8_t MenuActionRuntimeBit(int action)
{
    if (action < 0 || action >= static_cast<int>(kMenuActionToRuntimeWordIndex.size()))
    {
        return 0u;
    }

    const int runtimeIndex = kMenuActionToRuntimeWordIndex[static_cast<size_t>(action)];
    return (runtimeIndex >= 0 && runtimeIndex < 8) ? static_cast<uint8_t>(1u << runtimeIndex) : 0u;
}

uint8_t MenuDirectionalRuntimeBit(int action)
{
    return IsMenuDirectionalAction(action) ? MenuActionRuntimeBit(action) : 0u;
}

bool IsMenuNativePadButtonCode(uint8_t code)
{
    return code >= kMenuNativePadButtonMinCode && code <= kMenuNativePadButtonMaxCode;
}

bool IsMenuNativePadAxisCode(uint8_t code)
{
    return code >= kMenuNativePadAxisUpCode && code <= kMenuNativePadAxisRightCode;
}

bool IsMenuNativePadBinding(const MenuControlBinding& binding)
{
    return binding.device >= 1u && binding.device <= 2u &&
        (IsMenuNativePadButtonCode(binding.code) || IsMenuNativePadAxisCode(binding.code));
}

bool IsMenuExtendedPadButtonCode(uint8_t code)
{
    return code >= kMenuExtendedPadButtonBaseCode &&
        code < static_cast<uint8_t>(kMenuExtendedPadButtonBaseCode + kMenuExtendedPadButtonCount);
}

int MenuExtendedPadButtonIndexFromCode(uint8_t code)
{
    if (!IsMenuExtendedPadButtonCode(code))
    {
        return -1;
    }
    return 16 + static_cast<int>(code - kMenuExtendedPadButtonBaseCode);
}

bool IsMenuKnownExtendedPadCode(uint8_t code)
{
    if (IsMenuExtendedPadButtonCode(code))
    {
        return true;
    }

    switch (code)
    {
    case kMenuRightStickUpCode:
    case kMenuRightStickDownCode:
    case kMenuRightStickLeftCode:
    case kMenuRightStickRightCode:
    case kMenuZAxisNegativeCode:
    case kMenuZAxisPositiveCode:
    case kMenuRzAxisNegativeCode:
    case kMenuRzAxisPositiveCode:
    case kMenuSlider0NegativeCode:
    case kMenuSlider0PositiveCode:
    case kMenuSlider1NegativeCode:
    case kMenuSlider1PositiveCode:
        return true;
    default:
        return false;
    }
}

bool IsMenuRuntimeOnlyPadBinding(const MenuControlBinding& binding)
{
    return binding.device >= 1u && binding.device <= 2u && binding.code != 0u &&
        !IsMenuNativePadBinding(binding);
}

bool ProfileUsesExtendedPadAxisBindings(const MenuControlProfile& profile)
{
    for (int player = 0; player < 2; ++player)
    {
        for (int action = 0; action < 8; ++action)
        {
            switch (profile.players[static_cast<size_t>(player)][static_cast<size_t>(action)].code)
            {
            case kMenuRightStickUpCode:
            case kMenuRightStickDownCode:
            case kMenuRightStickLeftCode:
            case kMenuRightStickRightCode:
            case kMenuZAxisNegativeCode:
            case kMenuZAxisPositiveCode:
            case kMenuRzAxisNegativeCode:
            case kMenuRzAxisPositiveCode:
            case kMenuSlider0NegativeCode:
            case kMenuSlider0PositiveCode:
            case kMenuSlider1NegativeCode:
            case kMenuSlider1PositiveCode:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}

uintptr_t TryResolveMenuInputManager(int gameSystem)
{
    if (gameSystem == 0)
    {
        return 0;
    }

    __try
    {
        return *reinterpret_cast<volatile uintptr_t*>(static_cast<uintptr_t>(gameSystem) + kGameSystemInputManagerOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

uintptr_t ReadMenuInputManagerPointerField(const uint8_t* inputManagerBytes, uintptr_t offset)
{
    if (inputManagerBytes == nullptr)
    {
        return 0;
    }

    __try
    {
        return *reinterpret_cast<volatile const uintptr_t*>(inputManagerBytes + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

BOOL CALLBACK NormalizeMenuLivePadAxisRangeCallback(const DIDEVICEOBJECTINSTANCEA* objectInstance, VOID* context)
{
    if (objectInstance == nullptr || context == nullptr)
    {
        return DIENUM_CONTINUE;
    }

    auto* const device = static_cast<IDirectInputDevice8A*>(context);
    DIPROPRANGE range = {};
    range.lMin = -1000;
    range.lMax = 1000;
    range.diph.dwSize = sizeof(DIPROPRANGE);
    range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    range.diph.dwHow = DIPH_BYID;
    range.diph.dwObj = objectInstance->dwType;
    device->SetProperty(DIPROP_RANGE, &range.diph);
    return DIENUM_CONTINUE;
}

void MaybeNormalizeMenuLivePadAxisRanges(uintptr_t inputManager)
{
    if (inputManager == 0)
    {
        return;
    }

    const auto* const inputManagerBytes = reinterpret_cast<const uint8_t*>(inputManager);
    const std::array<uintptr_t, 2> deviceOffsets = {
        kInputManagerPad1DeviceOffset,
        kInputManagerPad2DeviceOffset,
    };

    for (size_t padIndex = 0; padIndex < deviceOffsets.size(); ++padIndex)
    {
        const uintptr_t devicePtr = ReadMenuInputManagerPointerField(inputManagerBytes, deviceOffsets[padIndex]);
        if (devicePtr == 0 || g_menuLiveAxisRangeNormalizedDevicePtrs[padIndex] == devicePtr)
        {
            continue;
        }

        auto* const device = reinterpret_cast<IDirectInputDevice8A*>(devicePtr);
        const HRESULT hr = device->EnumObjects(&NormalizeMenuLivePadAxisRangeCallback, device, DIDFT_AXIS);
        if (FAILED(hr))
        {
            mod::Log(
                "MenuControls: live pad axis-range normalization warning PAD%u device=0x%08lX hr=0x%08lX",
                static_cast<unsigned>(padIndex + 1u),
                static_cast<unsigned long>(devicePtr),
                static_cast<unsigned long>(hr));
            continue;
        }

        g_menuLiveAxisRangeNormalizedDevicePtrs[padIndex] = devicePtr;
        mod::Log(
            "MenuControls: live pad axis-range normalization applied PAD%u device=0x%08lX",
            static_cast<unsigned>(padIndex + 1u),
            static_cast<unsigned long>(devicePtr));
    }
}

uint8_t MenuPovMaskFromRaw(DWORD raw)
{
    if (LOWORD(raw) == 0xFFFFu)
    {
        return 0u;
    }

    const unsigned angle = LOWORD(raw) % 36000u;
    if (angle < 2250u || angle >= 33750u)
    {
        return kMenuPollMaskUp;
    }
    if (angle < 6750u)
    {
        return static_cast<uint8_t>(kMenuPollMaskUp | kMenuPollMaskRight);
    }
    if (angle < 11250u)
    {
        return kMenuPollMaskRight;
    }
    if (angle < 15750u)
    {
        return static_cast<uint8_t>(kMenuPollMaskRight | kMenuPollMaskDown);
    }
    if (angle < 20250u)
    {
        return kMenuPollMaskDown;
    }
    if (angle < 24750u)
    {
        return static_cast<uint8_t>(kMenuPollMaskDown | kMenuPollMaskLeft);
    }
    if (angle < 29250u)
    {
        return kMenuPollMaskLeft;
    }
    return static_cast<uint8_t>(kMenuPollMaskLeft | kMenuPollMaskUp);
}

uint8_t MenuAxisCodeToPollMaskBit(uint8_t code)
{
    switch (code)
    {
    case kMenuNativePadAxisUpCode:
        return kMenuPollMaskUp;
    case kMenuNativePadAxisDownCode:
        return kMenuPollMaskDown;
    case kMenuNativePadAxisLeftCode:
        return kMenuPollMaskLeft;
    case kMenuNativePadAxisRightCode:
        return kMenuPollMaskRight;
    default:
        return 0u;
    }
}

uint8_t ReadMenuPadPovMask(const uint8_t* inputManagerBytes, uintptr_t padStateOffset, bool padPresent)
{
    if (inputManagerBytes == nullptr || !padPresent)
    {
        return 0u;
    }

    __try
    {
        const DWORD raw = *reinterpret_cast<volatile const DWORD*>(inputManagerBytes + padStateOffset + kPov0OffsetInJoyState);
        return MenuPovMaskFromRaw(raw);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0u;
    }
}

uint16_t EncodeMenuBindingWord(const MenuControlBinding& binding)
{
    if (binding.code == 0u)
    {
        return 0u;
    }
    if (binding.device == 0u)
    {
        return static_cast<uint16_t>(binding.code);
    }
    if (!IsMenuNativePadBinding(binding))
    {
        return 0u;
    }
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(binding.device) << 8u) | static_cast<uint16_t>(binding.code - 1u));
}

std::string BuildIgcrOverrideStorePath()
{
    char exePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return {};
    }

    char* const lastSlash = std::strrchr(exePath, '\\');
    if (lastSlash == nullptr)
    {
        return {};
    }

    *lastSlash = '\0';
    std::string path(exePath);
    path += "\\mods\\InGameControlsRebind\\IGCRProfileOverrides.bin";
    return path;
}

bool GetIgcrOverrideStoreWriteTime(FILETIME* outWriteTime)
{
    if (outWriteTime == nullptr)
    {
        return false;
    }

    const std::string path = BuildIgcrOverrideStorePath();
    if (path.empty())
    {
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attributes) == FALSE)
    {
        return false;
    }

    *outWriteTime = attributes.ftLastWriteTime;
    return true;
}

void UpdateIgcrOverrideWriteTimeCache()
{
    FILETIME writeTime = {};
    if (GetIgcrOverrideStoreWriteTime(&writeTime))
    {
        g_lastIgcrOverrideWriteTime = writeTime;
        g_hasLastIgcrOverrideWriteTime = true;
        return;
    }

    g_lastIgcrOverrideWriteTime = {};
    g_hasLastIgcrOverrideWriteTime = false;
}

bool LoadIgcrOverrideProfile(uint8_t profileIndex, MenuControlProfile* outProfile, std::string* outError)
{
    if (outProfile == nullptr)
    {
        return false;
    }

    const std::string path = BuildIgcrOverrideStorePath();
    if (path.empty())
    {
        if (outError != nullptr)
        {
            *outError = "unable to resolve IGCR override store path";
        }
        return false;
    }

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open())
    {
        return false;
    }

    const std::streamoff endPos = input.tellg();
    if (endPos <= 0)
    {
        if (outError != nullptr)
        {
            *outError = "IGCR override store is empty";
        }
        return false;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(endPos), 0u);
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input.good())
    {
        if (outError != nullptr)
        {
            *outError = "failed to read IGCR override store";
        }
        return false;
    }

    if (bytes.size() != kIgcrExpectedFileBytes)
    {
        if (outError != nullptr)
        {
            char buffer[128] = {};
            std::snprintf(
                buffer,
                sizeof(buffer),
                "unexpected IGCR override store size %zu (expected %zu)",
                bytes.size(),
                kIgcrExpectedFileBytes);
            *outError = buffer;
        }
        return false;
    }

    size_t cursor = 0;
    for (uint8_t expected : kIgcrOverrideMagic)
    {
        if (bytes[cursor++] != expected)
        {
            if (outError != nullptr)
            {
                *outError = "IGCR override store magic mismatch";
            }
            return false;
        }
    }

    const uint8_t version = bytes[cursor++];
    const uint8_t count = bytes[cursor++];
    if (version != 1u || count != static_cast<uint8_t>(kIgcrProfileCount))
    {
        if (outError != nullptr)
        {
            *outError = "IGCR override store version/count mismatch";
        }
        return false;
    }

    const size_t slotIndex = static_cast<size_t>(ClampMenuControlProfileIndex(profileIndex));
    cursor = kIgcrHeaderBytes + (slotIndex * kIgcrSlotBytes);

    if (bytes[cursor++] == 0u)
    {
        return false;
    }

    MenuControlProfile profile = {};
    profile.profileIndex = static_cast<uint8_t>(slotIndex);
    profile.povMasks[0] = static_cast<uint8_t>(bytes[cursor++] & kMenuDirectionalRuntimeMask);
    profile.povMasks[1] = static_cast<uint8_t>(bytes[cursor++] & kMenuDirectionalRuntimeMask);
    ++cursor;

    for (int player = 0; player < 2; ++player)
    {
        for (int action = 0; action < 8; ++action)
        {
            MenuControlBinding binding = {};
            binding.device = bytes[cursor++];
            binding.code = bytes[cursor++];
            if (binding.device > 2u ||
                (!IsMenuNativePadBinding(binding) && binding.device != 0u &&
                    binding.code != 0u && !IsMenuKnownExtendedPadCode(binding.code)))
            {
                binding = {};
            }

            profile.players[static_cast<size_t>(player)][static_cast<size_t>(action)] = binding;
            profile.hasRuntimeOnlyPadBindings = profile.hasRuntimeOnlyPadBindings || IsMenuRuntimeOnlyPadBinding(binding);
        }
    }

    profile.hasPovDirectionalOverrides =
        (profile.povMasks[0] & kMenuDirectionalRuntimeMask) != 0u ||
        (profile.povMasks[1] & kMenuDirectionalRuntimeMask) != 0u;
    *outProfile = profile;
    return true;
}

bool WriteMenuControlProfileLiveMappings(uintptr_t inputManager, const MenuControlProfile& profile)
{
    if (inputManager == 0)
    {
        return false;
    }

    __try
    {
        auto* const mappingWords = reinterpret_cast<volatile uint16_t*>(inputManager + kInputManagerMappingOffsetBytes);
        for (int player = 0; player < 2; ++player)
        {
            for (int action = 0; action < 8; ++action)
            {
                const int runtimeIndex = kMenuActionToRuntimeWordIndex[static_cast<size_t>(action)];
                if (runtimeIndex < 0 || runtimeIndex >= 8)
                {
                    continue;
                }
                mappingWords[(player * 8) + runtimeIndex] =
                    EncodeMenuBindingWord(profile.players[static_cast<size_t>(player)][static_cast<size_t>(action)]);
            }
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadMenuControlProfileIndex(uint8_t* outProfile)
{
    if (outProfile == nullptr)
    {
        return false;
    }

    auto const readConfigByte = reinterpret_cast<ReadConfigByteFn>(RuntimeAddress(kVaReadConfigByte));
    if (readConfigByte == nullptr)
    {
        return false;
    }

    __try
    {
        *outProfile = ClampMenuControlProfileIndex(readConfigByte(kSelectedControlProfileConfigByte));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool IsMenuPadBindingActiveFromJoyState(const uint8_t* inputManagerBytes, const MenuControlBinding& binding)
{
    if (inputManagerBytes == nullptr || binding.device < 1u || binding.device > 2u || binding.code == 0u)
    {
        return false;
    }

    const uintptr_t deviceOffset = (binding.device == 1u) ? kInputManagerPad1DeviceOffset : kInputManagerPad2DeviceOffset;
    if (ReadMenuInputManagerPointerField(inputManagerBytes, deviceOffset) == 0u)
    {
        return false;
    }

    const uintptr_t padStateOffset = (binding.device == 1u) ? kPad1StateOffset : kPad2StateOffset;
    auto readAxis = [&](uintptr_t axisOffset, int32_t* outValue) -> bool
    {
        if (outValue == nullptr)
        {
            return false;
        }

        __try
        {
            *outValue = *reinterpret_cast<volatile const int32_t*>(inputManagerBytes + padStateOffset + axisOffset);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    };

    const int buttonIndex = MenuExtendedPadButtonIndexFromCode(binding.code);
    if (buttonIndex >= 0)
    {
        __try
        {
            const uint8_t value = *reinterpret_cast<volatile const uint8_t*>(
                inputManagerBytes + padStateOffset + kJoyStateButton0Offset + static_cast<uintptr_t>(buttonIndex));
            return (value & 0x80u) != 0u;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    int32_t axisValue = 0;
    switch (binding.code)
    {
    case kMenuRightStickUpCode:
        return readAxis(kJoyStateRyOffset, &axisValue) && axisValue < -kMenuPadAxisThreshold;
    case kMenuRightStickDownCode:
        return readAxis(kJoyStateRyOffset, &axisValue) && axisValue > kMenuPadAxisThreshold;
    case kMenuRightStickLeftCode:
        return readAxis(kJoyStateRxOffset, &axisValue) && axisValue < -kMenuPadAxisThreshold;
    case kMenuRightStickRightCode:
        return readAxis(kJoyStateRxOffset, &axisValue) && axisValue > kMenuPadAxisThreshold;
    case kMenuZAxisNegativeCode:
        return readAxis(kJoyStateZOffset, &axisValue) && axisValue < -kMenuPadAxisThreshold;
    case kMenuZAxisPositiveCode:
        return readAxis(kJoyStateZOffset, &axisValue) && axisValue > kMenuPadAxisThreshold;
    case kMenuRzAxisNegativeCode:
        return readAxis(kJoyStateRzOffset, &axisValue) && axisValue < -kMenuPadAxisThreshold;
    case kMenuRzAxisPositiveCode:
        return readAxis(kJoyStateRzOffset, &axisValue) && axisValue > kMenuPadAxisThreshold;
    case kMenuSlider0NegativeCode:
        return readAxis(kJoyStateSlider0Offset, &axisValue) && axisValue < -kMenuPadAxisThreshold;
    case kMenuSlider0PositiveCode:
        return readAxis(kJoyStateSlider0Offset, &axisValue) && axisValue > kMenuPadAxisThreshold;
    case kMenuSlider1NegativeCode:
        return readAxis(kJoyStateSlider1Offset, &axisValue) && axisValue < -kMenuPadAxisThreshold;
    case kMenuSlider1PositiveCode:
        return readAxis(kJoyStateSlider1Offset, &axisValue) && axisValue > kMenuPadAxisThreshold;
    default:
        return false;
    }
}

bool GetMenuActionStateFromBytes(const uint8_t* inputBytes, int player, int action)
{
    if (inputBytes == nullptr || player < 0 || player > 1)
    {
        return false;
    }

    const int playerOffset = player;
    switch (action)
    {
    case 0:
        return static_cast<int8_t>(inputBytes[14 + playerOffset]) < 0;
    case 1:
        return static_cast<int8_t>(inputBytes[14 + playerOffset]) > 0;
    case 2:
        return static_cast<int8_t>(inputBytes[12 + playerOffset]) < 0;
    case 3:
        return static_cast<int8_t>(inputBytes[12 + playerOffset]) > 0;
    case 4:
        return inputBytes[16 + playerOffset] != 0;
    case 5:
        return inputBytes[18 + playerOffset] != 0;
    case 6:
        return inputBytes[20 + playerOffset] != 0;
    case 7:
        return inputBytes[22 + playerOffset] != 0;
    default:
        return false;
    }
}

void WriteMenuActionsToBytes(uint8_t* inputBytes, int player, const std::array<bool, 8>& actions)
{
    if (inputBytes == nullptr || player < 0 || player > 1)
    {
        return;
    }

    const int playerOffset = player;
    int8_t horizontal = 0;
    if (actions[3] && !actions[2])
    {
        horizontal = 1;
    }
    else if (actions[2] && !actions[3])
    {
        horizontal = -1;
    }

    int8_t vertical = 0;
    if (actions[1] && !actions[0])
    {
        vertical = 1;
    }
    else if (actions[0] && !actions[1])
    {
        vertical = -1;
    }

    inputBytes[12 + playerOffset] = static_cast<uint8_t>(horizontal);
    inputBytes[14 + playerOffset] = static_cast<uint8_t>(vertical);
    inputBytes[16 + playerOffset] = actions[4] ? 1u : 0u;
    inputBytes[18 + playerOffset] = actions[5] ? 1u : 0u;
    inputBytes[20 + playerOffset] = actions[6] ? 1u : 0u;
    inputBytes[22 + playerOffset] = actions[7] ? 1u : 0u;
}

void ApplyIgcrMenuControlCompatibility(int gameSystem, uint8_t* inputBytes)
{
    if (!g_menuControlRuntimeCompatActive || inputBytes == nullptr)
    {
        return;
    }

    uint8_t currentProfile = 0;
    if (ReadMenuControlProfileIndex(&currentProfile) && currentProfile != g_menuControlProfile.profileIndex)
    {
        return;
    }

    const uintptr_t inputManager = TryResolveMenuInputManager(gameSystem);
    if (inputManager == 0)
    {
        return;
    }

    if (ProfileUsesExtendedPadAxisBindings(g_menuControlProfile))
    {
        MaybeNormalizeMenuLivePadAxisRanges(inputManager);
    }

    const auto* const inputManagerBytes = reinterpret_cast<const uint8_t*>(inputManager);
    bool anyChanged = false;
    for (int player = 0; player < 2; ++player)
    {
        std::array<bool, 8> actions = {};
        for (int action = 0; action < 8; ++action)
        {
            actions[static_cast<size_t>(action)] = GetMenuActionStateFromBytes(inputBytes, player, action);
        }

        const uint8_t playerPovMask = g_menuControlProfile.povMasks[static_cast<size_t>(player)];
        const bool padPresent = ReadMenuInputManagerPointerField(
            inputManagerBytes,
            (player == 0) ? kInputManagerPad1DeviceOffset : kInputManagerPad2DeviceOffset) != 0u;
        const uint8_t livePovMask = ReadMenuPadPovMask(
            inputManagerBytes,
            (player == 0) ? kPad1StateOffset : kPad2StateOffset,
            padPresent);

        std::array<bool, 8> rewritten = actions;
        for (int action = 0; action < 8; ++action)
        {
            const MenuControlBinding binding =
                g_menuControlProfile.players[static_cast<size_t>(player)][static_cast<size_t>(action)];
            if (IsMenuRuntimeOnlyPadBinding(binding))
            {
                rewritten[static_cast<size_t>(action)] =
                    IsMenuPadBindingActiveFromJoyState(inputManagerBytes, binding);
                continue;
            }

            if (!IsMenuDirectionalAction(action) || !IsMenuNativePadAxisCode(binding.code))
            {
                continue;
            }

            const uint8_t directionBit = MenuDirectionalRuntimeBit(action);
            if ((playerPovMask & directionBit) == 0u)
            {
                continue;
            }

            const uint8_t pollMask = MenuAxisCodeToPollMaskBit(binding.code);
            rewritten[static_cast<size_t>(action)] = (livePovMask & pollMask) != 0u;
        }

        if (rewritten != actions)
        {
            WriteMenuActionsToBytes(inputBytes, player, rewritten);
            anyChanged = true;
        }
    }

    if (!anyChanged)
    {
        return;
    }

    InputSnapshot compatSnapshot = {
        static_cast<int8_t>(inputBytes[12]),
        static_cast<int8_t>(inputBytes[14]),
        inputBytes[16],
        inputBytes[18],
        static_cast<int8_t>(inputBytes[13]),
        static_cast<int8_t>(inputBytes[15]),
        inputBytes[17],
        inputBytes[19],
    };
    if (!g_hasLoggedCompatInputSnapshot ||
        std::memcmp(&compatSnapshot, &g_lastCompatInputSnapshot, sizeof(InputSnapshot)) != 0)
    {
        mod::Log(
            "MenuControls: IGCR compatibility rewrote menu input profile=%u P1(h=%d v=%d a=%u b=%u) P2(h=%d v=%d a=%u b=%u)",
            static_cast<unsigned>(g_menuControlProfile.profileIndex),
            static_cast<int>(compatSnapshot.p1Horizontal),
            static_cast<int>(compatSnapshot.p1Vertical),
            static_cast<unsigned>(compatSnapshot.p1Confirm),
            static_cast<unsigned>(compatSnapshot.p1Cancel),
            static_cast<int>(compatSnapshot.p2Horizontal),
            static_cast<int>(compatSnapshot.p2Vertical),
            static_cast<unsigned>(compatSnapshot.p2Confirm),
            static_cast<unsigned>(compatSnapshot.p2Cancel));
        g_lastCompatInputSnapshot = compatSnapshot;
        g_hasLoggedCompatInputSnapshot = true;
    }
}

bool ReloadNativeMenuControlBindings(uint32_t screenContext)
{
    const int gameSystem = GetGameSystem(screenContext);
    if (gameSystem == 0)
    {
        mod::Log("MenuControls: skipped live control refresh (gameSystem unavailable)");
        return false;
    }

    auto const readConfigByte =
        reinterpret_cast<ReadConfigByteFn>(RuntimeAddress(kVaReadConfigByte));
    auto const loadControlBindings =
        reinterpret_cast<LoadControlBindingsFn>(RuntimeAddress(kVaLoadControlBindings));
    auto const loadControlPreset =
        reinterpret_cast<LoadControlPresetFn>(RuntimeAddress(kVaLoadControlPreset));
    if (readConfigByte == nullptr || loadControlBindings == nullptr || loadControlPreset == nullptr)
    {
        mod::Log(
            "MenuControls: skipped live control refresh (functions unavailable read=0x%08lX load=0x%08lX preset=0x%08lX)",
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(readConfigByte)),
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(loadControlBindings)),
            static_cast<unsigned long>(reinterpret_cast<uintptr_t>(loadControlPreset)));
        return false;
    }

    const uintptr_t inputManager = TryResolveMenuInputManager(gameSystem);

    if (inputManager == 0)
    {
        mod::Log(
            "MenuControls: skipped live control refresh (input manager missing gameSystem=0x%08lX)",
            static_cast<unsigned long>(gameSystem));
        return false;
    }

    uint8_t profile = 0;
    if (!ReadMenuControlProfileIndex(&profile))
    {
        mod::Log("MenuControls: exception reading selected control profile");
        return false;
    }

    if (profile > 5u)
    {
        mod::Log(
            "MenuControls: selected control profile out of range (%u), falling back to custom profile",
            static_cast<unsigned>(profile));
        profile = 0;
    }

    BOOL ok = FALSE;
    const char* reloadPath = (profile == 0u) ? "loadControlBindings" : "loadControlPreset";
    __try
    {
        ok = (profile == 0u)
            ? loadControlBindings(reinterpret_cast<uint16_t*>(inputManager))
            : loadControlPreset(reinterpret_cast<uint16_t*>(inputManager), static_cast<int>(profile));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log(
            "MenuControls: exception during live control refresh path=%s profile=%u inputManager=0x%08lX",
            reloadPath,
            static_cast<unsigned>(profile),
            static_cast<unsigned long>(inputManager));
        return false;
    }

    mod::Log(
        "MenuControls: live control refresh path=%s profile=%u inputManager=0x%08lX ok=%d",
        reloadPath,
        static_cast<unsigned>(profile),
        static_cast<unsigned long>(inputManager),
        ok ? 1 : 0);
    return ok == TRUE;
}

bool SyncMenuControlBindings(uint32_t screenContext)
{
    g_menuControlProfile = {};
    g_menuControlProfileLoaded = false;
    g_menuControlRuntimeCompatActive = false;
    g_hasLoggedCompatInputSnapshot = false;
    g_menuControlSyncAttempted = true;
    UpdateIgcrOverrideWriteTimeCache();

    const int gameSystem = GetGameSystem(screenContext);
    if (gameSystem == 0)
    {
        mod::Log("MenuControls: sync skipped (gameSystem unavailable)");
        return false;
    }

    const uintptr_t inputManager = TryResolveMenuInputManager(gameSystem);
    if (inputManager == 0)
    {
        mod::Log(
            "MenuControls: sync skipped (input manager unavailable gameSystem=0x%08lX)",
            static_cast<unsigned long>(gameSystem));
        return false;
    }

    uint8_t profileIndex = 0;
    if (!ReadMenuControlProfileIndex(&profileIndex))
    {
        mod::Log("MenuControls: sync failed (selected control profile unavailable)");
        return ReloadNativeMenuControlBindings(screenContext);
    }
    g_lastMenuControlSyncProfileIndex = profileIndex;

    MenuControlProfile effectiveProfile = {};
    std::string overrideError;
    if (!LoadIgcrOverrideProfile(profileIndex, &effectiveProfile, &overrideError))
    {
        if (!overrideError.empty())
        {
            mod::Log(
                "MenuControls: IGCR override load failed profile=%u reason=%s",
                static_cast<unsigned>(profileIndex),
                overrideError.c_str());
        }
        const bool ok = ReloadNativeMenuControlBindings(screenContext);
        mod::Log(
            "MenuControls: sync fallback native reload profile=%u ok=%d",
            static_cast<unsigned>(profileIndex),
            ok ? 1 : 0);
        return ok;
    }

    const bool writeOk = WriteMenuControlProfileLiveMappings(inputManager, effectiveProfile);
    if (ProfileUsesExtendedPadAxisBindings(effectiveProfile))
    {
        MaybeNormalizeMenuLivePadAxisRanges(inputManager);
    }
    if (!writeOk)
    {
        mod::Log(
            "MenuControls: IGCR live mapping apply failed profile=%u inputManager=0x%08lX; falling back to native reload",
            static_cast<unsigned>(effectiveProfile.profileIndex),
            static_cast<unsigned long>(inputManager));
        const bool nativeOk = ReloadNativeMenuControlBindings(screenContext);
        g_menuControlProfile = effectiveProfile;
        g_menuControlProfileLoaded = true;
        g_menuControlRuntimeCompatActive =
            effectiveProfile.hasRuntimeOnlyPadBindings || effectiveProfile.hasPovDirectionalOverrides;
        return nativeOk;
    }

    g_menuControlProfile = effectiveProfile;
    g_menuControlProfileLoaded = true;
    g_menuControlRuntimeCompatActive =
        effectiveProfile.hasRuntimeOnlyPadBindings || effectiveProfile.hasPovDirectionalOverrides;
    mod::Log(
        "MenuControls: IGCR profile sync applied profile=%u inputManager=0x%08lX runtimeOnly=%d povMaskP1=0x%02X povMaskP2=0x%02X extAxis=%d compat=%d",
        static_cast<unsigned>(effectiveProfile.profileIndex),
        static_cast<unsigned long>(inputManager),
        effectiveProfile.hasRuntimeOnlyPadBindings ? 1 : 0,
        static_cast<unsigned>(effectiveProfile.povMasks[0]),
        static_cast<unsigned>(effectiveProfile.povMasks[1]),
        ProfileUsesExtendedPadAxisBindings(effectiveProfile) ? 1 : 0,
        g_menuControlRuntimeCompatActive ? 1 : 0);
    return true;
}

void MaybeRefreshMenuControlBindings(uint32_t screenContext)
{
    const DWORD now = GetTickCount();
    if (now < g_nextMenuControlRefreshTick)
    {
        return;
    }
    g_nextMenuControlRefreshTick = now + 500u;

    bool needsSync = !g_menuControlSyncAttempted;

    uint8_t profileIndex = 0;
    if (ReadMenuControlProfileIndex(&profileIndex))
    {
        if (!g_menuControlSyncAttempted || profileIndex != g_lastMenuControlSyncProfileIndex)
        {
            needsSync = true;
        }
    }

    FILETIME currentWriteTime = {};
    const bool hasCurrentWriteTime = GetIgcrOverrideStoreWriteTime(&currentWriteTime);
    if (hasCurrentWriteTime != g_hasLastIgcrOverrideWriteTime ||
        (hasCurrentWriteTime && CompareFileTime(&currentWriteTime, &g_lastIgcrOverrideWriteTime) != 0))
    {
        needsSync = true;
    }

    if (!needsSync)
    {
        return;
    }

    (void)SyncMenuControlBindings(screenContext);
}

void ResetMenuControlCompatibilityState()
{
    g_menuControlProfile = {};
    g_menuControlProfileLoaded = false;
    g_menuControlRuntimeCompatActive = false;
    g_hasLoggedCompatInputSnapshot = false;
    g_menuControlSyncAttempted = false;
    g_lastMenuControlSyncProfileIndex = 0xFFu;
    g_menuLiveAxisRangeNormalizedDevicePtrs = {};
    g_lastIgcrOverrideWriteTime = {};
    g_hasLastIgcrOverrideWriteTime = false;
    g_nextMenuControlRefreshTick = 0;
}

void ReenterNetplayMenuAfterSessionAbort(uint32_t screenContext, const char* reason)
{
    const bool skipFadeOut = (g_lobbySession != nullptr);
    mod::Log(
        "NetplayReturn: re-entering menu after session abort reason='%s' existingLobby=%d skipFadeOut=%d",
        reason != nullptr ? reason : "",
        g_lobbySession ? 1 : 0,
        skipFadeOut ? 1 : 0);
    EnterNetplayMenu(screenContext, skipFadeOut);
}

void ClearPendingLobbySpectateWait(const char* reason)
{
    if (g_pendingLobbySpectateWait.active)
    {
        mod::Log(
            "LobbySpectateWait: cleared reason='%s' hostIp=%s trackedPlayer='%s' pair='%s' vs '%s'",
            reason != nullptr ? reason : "",
            g_pendingLobbySpectateWait.hostIp.c_str(),
            g_pendingLobbySpectateWait.trackedPlayerName.c_str(),
            g_pendingLobbySpectateWait.p1Name.c_str(),
            g_pendingLobbySpectateWait.p2Name.c_str());
    }
    g_pendingLobbySpectateWait = {};
}

bool ShouldWriteNicknameToRevivalIniForSessionStart()
{
    const bool shouldWrite = ShouldWriteNicknameToRevivalIni();
    if (!shouldWrite)
    {
        mod::Log(
            "NetplayNickname: preserving existing EfzRevival.ini Network.Name (current='%s' source=%s)",
            g_netplayMenuState.nickname.c_str(),
            NetplayNicknameSourceToString(g_netplayMenuState.nicknameSource));
    }
    return shouldWrite;
}

void ArmPendingLobbySpectateWait(
    const netplay::lobby::LobbyPlayingPair* pair,
    int trackedPlayerId,
    const std::string& trackedPlayerName,
    const std::string& hostIp)
{
    g_pendingLobbySpectateWait = {};
    g_pendingLobbySpectateWait.active = true;
    g_pendingLobbySpectateWait.trackedPlayerId = trackedPlayerId;
    g_pendingLobbySpectateWait.trackedPlayerName = trackedPlayerName;
    g_pendingLobbySpectateWait.hostIp = hostIp;
    if (pair != nullptr)
    {
        g_pendingLobbySpectateWait.p1Id = pair->p1Id;
        g_pendingLobbySpectateWait.p2Id = pair->p2Id;
        g_pendingLobbySpectateWait.p1Name = pair->p1Name;
        g_pendingLobbySpectateWait.p2Name = pair->p2Name;
        if (g_pendingLobbySpectateWait.hostIp.empty())
        {
            g_pendingLobbySpectateWait.hostIp = pair->hostIp;
        }
    }

    mod::Log(
        "LobbySpectateWait: armed hostIp=%s trackedPlayerId=%d trackedPlayer='%s' pair='%s'(%d) vs '%s'(%d)",
        g_pendingLobbySpectateWait.hostIp.c_str(),
        g_pendingLobbySpectateWait.trackedPlayerId,
        g_pendingLobbySpectateWait.trackedPlayerName.c_str(),
        g_pendingLobbySpectateWait.p1Name.c_str(),
        g_pendingLobbySpectateWait.p1Id,
        g_pendingLobbySpectateWait.p2Name.c_str(),
        g_pendingLobbySpectateWait.p2Id);
}

bool PairContainsTrackedPlayer(
    const netplay::lobby::LobbyPlayingPair& pair,
    const PendingLobbySpectateWait& pending)
{
    if (pending.trackedPlayerId != 0
        && (pair.p1Id == pending.trackedPlayerId || pair.p2Id == pending.trackedPlayerId))
    {
        return true;
    }
    if (!pending.trackedPlayerName.empty()
        && (pair.p1Name == pending.trackedPlayerName || pair.p2Name == pending.trackedPlayerName))
    {
        return true;
    }
    return false;
}

bool PairMatchesPendingLobbySpectateWait(
    const netplay::lobby::LobbyPlayingPair& pair,
    const PendingLobbySpectateWait& pending)
{
    if (!pending.hostIp.empty() && pair.hostIp != pending.hostIp)
    {
        return false;
    }

    if (pending.p1Id != 0 && pending.p2Id != 0)
    {
        const bool sameOrder = pair.p1Id == pending.p1Id && pair.p2Id == pending.p2Id;
        const bool swappedOrder = pair.p1Id == pending.p2Id && pair.p2Id == pending.p1Id;
        if (sameOrder || swappedOrder)
        {
            return true;
        }
    }

    if (!pending.p1Name.empty() && !pending.p2Name.empty())
    {
        const bool sameOrder = pair.p1Name == pending.p1Name && pair.p2Name == pending.p2Name;
        const bool swappedOrder = pair.p1Name == pending.p2Name && pair.p2Name == pending.p1Name;
        if (sameOrder || swappedOrder)
        {
            return true;
        }
    }

    return PairContainsTrackedPlayer(pair, pending);
}

bool IsPendingLobbySpectateWaitStillValid()
{
    if (!g_pendingLobbySpectateWait.active || !g_lobbySession)
    {
        return false;
    }

    const auto status = g_lobbySession->GetStatus();
    for (const auto& pair : status.playing)
    {
        if (PairMatchesPendingLobbySpectateWait(pair, g_pendingLobbySpectateWait))
        {
            return true;
        }
    }
    return false;
}

bool TryResolvePlayingPairForLobbyEntry(
    const netplay::lobby::LobbyStatus& status,
    const netplay::lobby::LobbyDisplayEntry& entry,
    netplay::lobby::LobbyPlayingPair* outPair)
{
    if (outPair == nullptr)
    {
        return false;
    }

    for (const auto& pair : status.playing)
    {
        if (!entry.spectateIp.empty() && pair.hostIp != entry.spectateIp)
        {
            continue;
        }

        const bool playerIdMatches =
            entry.playerId != 0 && (pair.p1Id == entry.playerId || pair.p2Id == entry.playerId);
        const bool playerNameMatches =
            !entry.name.empty() && (pair.p1Name == entry.name || pair.p2Name == entry.name);
        if (!playerIdMatches && !playerNameMatches)
        {
            continue;
        }

        *outPair = pair;
        return true;
    }

    return false;
}

void PumpLobbySessionShutdown()
{
    if (!g_lobbySessionShutdownCompleted.load())
    {
        return;
    }

    if (g_lobbySessionShutdownThread.joinable())
    {
        g_lobbySessionShutdownThread.join();
    }

    const bool returnToPlayerRooms = g_lobbySessionShutdownReturnToPlayerRooms.exchange(false);
    g_lobbySessionShutdownCompleted.store(false);
    g_lobbySessionShutdownInFlight.store(false);

    mod::Log(
        "LobbySessionShutdown: completed returnToPlayerRooms=%d",
        returnToPlayerRooms ? 1 : 0);

    if (returnToPlayerRooms)
    {
        netplay::player_rooms::NotifyLobbySessionShutdownCompleted();
    }
}

void BeginLobbySessionShutdown(bool returnToPlayerRooms)
{
    PumpLobbySessionShutdown();

    if (!g_lobbySession)
    {
        return;
    }

    if (g_lobbySessionShutdownThread.joinable())
    {
        g_lobbySessionShutdownThread.join();
    }

    g_lobbySessionShutdownInFlight.store(true);
    g_lobbySessionShutdownCompleted.store(false);
    g_lobbySessionShutdownReturnToPlayerRooms.store(returnToPlayerRooms);

    if (returnToPlayerRooms)
    {
        netplay::player_rooms::NotifyLobbySessionShutdownStarted();
    }

    mod::Log(
        "LobbySessionShutdown: begin returnToPlayerRooms=%d",
        returnToPlayerRooms ? 1 : 0);

    g_lobbySessionShutdownThread = std::thread([session = std::move(g_lobbySession)]() mutable {
        session.reset();
        g_lobbySessionShutdownCompleted.store(true);
    });
}

bool ShutdownLobbySessionForProcessExitImpl(bool emergency, const char* reason)
{
    const char* shutdownReason = reason != nullptr
        ? reason
        : (emergency ? "process_exit_emergency" : "process_exit");

    PumpLobbySessionShutdown();

    if (g_lobbySessionShutdownThread.joinable())
    {
        mod::Log(
            "LobbySessionShutdown: waiting for in-flight shutdown reason='%s' emergency=%d",
            shutdownReason,
            emergency ? 1 : 0);
        g_lobbySessionShutdownThread.join();
        g_lobbySessionShutdownCompleted.store(false);
        g_lobbySessionShutdownInFlight.store(false);
        g_lobbySessionShutdownReturnToPlayerRooms.store(false);
        netplay::player_rooms::NotifyLobbySessionShutdownCompleted();
    }

    if (!g_lobbySession)
    {
        return false;
    }

    const auto status = g_lobbySession->GetStatus();
    mod::Log(
        "LobbySessionShutdown: process-exit begin reason='%s' emergency=%d roomCode='%s' origin=%d inBattle=%d pollState=%d",
        shutdownReason,
        emergency ? 1 : 0,
        g_lobbySession->GetRoomCode().c_str(),
        static_cast<int>(g_lobbySession->GetOrigin()),
        status.inBattle ? 1 : 0,
        static_cast<int>(status.pollState));

    std::unique_ptr<netplay::lobby::LobbySession> session = std::move(g_lobbySession);
    session.reset();

    g_lobbySessionShutdownCompleted.store(false);
    g_lobbySessionShutdownInFlight.store(false);
    g_lobbySessionShutdownReturnToPlayerRooms.store(false);
    mod::Log(
        "LobbySessionShutdown: process-exit complete reason='%s' emergency=%d",
        shutdownReason,
        emergency ? 1 : 0);
    return true;
}

void ResetWindowFocusInputSuppression()
{
    g_filteredMenuInputs.fill(0);
    g_unfocusedHeldMenuInputs.fill(0);
    g_netplayHotkeyDown.fill(0);
}

void ClearTitleInputLatches(uint32_t screenContext)
{
    __try
    {
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void ClearLocalMenuControlState(uint32_t screenContext)
{
    ClearTitleInputLatches(screenContext);
    g_filteredMenuInputs.fill(0);
    g_unfocusedHeldMenuInputs.fill(0);
    g_netplayHotkeyDown.fill(0);
    g_joinWaitToSpectateButtonDown = {};
}

void ResetSpectateHandoffWarmup(const char* reason)
{
    if (g_spectateHandoffWarmupActive && reason != nullptr)
    {
        mod::Log(
            "SpectateHandoffWarmup: reset reason=%s frames=%d target=%d",
            reason,
            g_spectateHandoffWarmupFrames,
            kSpectateTitleWarmupFrames);
    }
    g_spectateHandoffWarmupActive = false;
    g_spectateHandoffWarmupFrames = 0;
    g_spectateHandoffWarmupStartTick = 0;
}

void ResetVsHumanHandoffWarmup(const char* reason)
{
    if (g_vsHumanHandoffWarmupActive && reason != nullptr)
    {
        mod::Log(
            "VsHumanHandoffWarmup: reset reason=%s frames=%d target=%d",
            reason,
            g_vsHumanHandoffWarmupFrames,
            kVsHumanTitleWarmupFrames);
    }
    g_vsHumanHandoffWarmupActive = false;
    g_vsHumanHandoffWarmupFrames = 0;
    g_vsHumanHandoffWarmupStartTick = 0;
}

bool AdvanceVsHumanHandoffWarmup(
    uint32_t screenContext,
    const netplay::bridge::NetbridgeStatus& bridgeStatus,
    NetbridgePhase bridgePhase,
    const char* source,
    uint32_t* inactivityCounter)
{
    if (!g_vsHumanHandoffWarmupActive)
    {
        g_vsHumanHandoffWarmupActive = true;
        g_vsHumanHandoffWarmupFrames = 0;
        g_vsHumanHandoffWarmupStartTick = GetTickCount();
        mod::Log(
            "VsHumanHandoffWarmup: armed targetFrames=%d source=%s role=%d roleFlag=%d init=%d phase=%s "
            "sync(mode=%d flag1084=%d session=%d flags=%d/%d) screen=%d menuSel=%d peerAlive=%d",
            kVsHumanTitleWarmupFrames,
            source != nullptr ? source : "unknown",
            bridgeStatus.role,
            bridgeStatus.roleFlag,
            bridgeStatus.localInitApplied,
            netplay::bridge::PhaseToString(bridgePhase),
            bridgeStatus.syncGameMode,
            bridgeStatus.syncMode0Flag1084,
            bridgeStatus.syncSessionByte,
            bridgeStatus.syncGlobalFlag4964,
            bridgeStatus.syncGlobalFlag4965,
            *reinterpret_cast<const int*>(kVaCurrentScreenIndex),
            static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)),
            netplay::bridge::IsPeerProcessAlive() ? 1 : 0);
    }

    ++g_vsHumanHandoffWarmupFrames;
    if (g_vsHumanHandoffWarmupFrames < kVsHumanTitleWarmupFrames)
    {
        ClearLocalMenuControlState(screenContext);
        if (inactivityCounter != nullptr)
        {
            ++(*inactivityCounter);
        }
        return false;
    }

    const uint32_t elapsedMs =
        g_vsHumanHandoffWarmupStartTick != 0
            ? GetTickCount() - g_vsHumanHandoffWarmupStartTick
            : 0;
    mod::Log(
        "VsHumanHandoffWarmup: complete frames=%d target=%d elapsedMs=%lu "
        "screen=%d menuSel=%d",
        g_vsHumanHandoffWarmupFrames,
        kVsHumanTitleWarmupFrames,
        static_cast<unsigned long>(elapsedMs),
        *reinterpret_cast<const int*>(kVaCurrentScreenIndex),
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
    HandoffConnectedSessionToVsHumanState(screenContext);
    return g_pendingGlobalStateTransition >= 0;
}

bool HasRecoveryMenuActionInput(const uint8_t* inputBytes)
{
    if (inputBytes == nullptr)
    {
        return false;
    }
    return inputBytes[16] != 0
        || inputBytes[17] != 0
        || inputBytes[18] != 0
        || inputBytes[19] != 0
        || inputBytes[20] != 0
        || inputBytes[21] != 0
        || inputBytes[22] != 0
        || inputBytes[23] != 0;
}

bool HasRecoveryMenuAxisInput(const uint8_t* inputBytes)
{
    if (inputBytes == nullptr)
    {
        return false;
    }
    return inputBytes[12] != 0
        || inputBytes[13] != 0
        || inputBytes[14] != 0
        || inputBytes[15] != 0;
}

void BeginRecoveryMenuInputQuarantine(uint32_t screenContext, const char* origin)
{
    g_recoveryMenuInputQuarantineActive = true;
    g_recoveryMenuInputQuarantineLoggedSuppress = false;
    g_recoveryMenuInputQuarantineLoggedHeldAfterWindow = false;
    g_recoveryMenuInputQuarantineLoggedLeaveSuppress = false;
    g_recoveryMenuInputQuarantineFramesRemaining = kRecoveryMenuInputQuarantineFrames;
    g_netplayEscapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    g_joinWaitToSpectateButtonDown = {};
    ClearTitleInputLatches(screenContext);
    mod::Log(
        "RECOVERY_MENU_INPUT_QUARANTINE_BEGIN screenContext=0x%08X frames=%d origin=%s",
        screenContext,
        g_recoveryMenuInputQuarantineFramesRemaining,
        origin != nullptr ? origin : "unknown");
}

bool SuppressRecoveryMenuInputIfNeeded(
    uint32_t screenContext,
    const uint8_t* inputBytes,
    uint32_t* inactivityCounter)
{
    if (!g_recoveryMenuInputQuarantineActive)
    {
        return false;
    }

    const bool escapeHeld = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool actionHeld = HasRecoveryMenuActionInput(inputBytes);
    const bool axisHeld = HasRecoveryMenuAxisInput(inputBytes);
    const bool cancelHeld = inputBytes != nullptr
        && (inputBytes[18] != 0 || inputBytes[19] != 0);
    const bool suppress =
        g_recoveryMenuInputQuarantineFramesRemaining > 0
        || actionHeld
        || escapeHeld;

    if (!suppress)
    {
        g_recoveryMenuInputQuarantineActive = false;
        g_recoveryMenuInputQuarantineLoggedSuppress = false;
        g_recoveryMenuInputQuarantineLoggedHeldAfterWindow = false;
        g_recoveryMenuInputQuarantineLoggedLeaveSuppress = false;
        mod::Log("RECOVERY_MENU_INPUT_QUARANTINE_END reason=released");
        return false;
    }

    if (g_recoveryMenuInputQuarantineFramesRemaining > 0)
    {
        --g_recoveryMenuInputQuarantineFramesRemaining;
    }

    ClearTitleInputLatches(screenContext);
    g_netplayEscapeDown = escapeHeld;
    if (inputBytes != nullptr)
    {
        for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
        {
            g_joinWaitToSpectateButtonDown[static_cast<size_t>(playerIndex)] =
                inputBytes[playerIndex + 22] != 0 ? 1u : 0u;
        }
    }

    const bool heldAfterWindow =
        (actionHeld || escapeHeld)
        && g_recoveryMenuInputQuarantineFramesRemaining == 0;
    if (!g_recoveryMenuInputQuarantineLoggedSuppress
        || (heldAfterWindow && !g_recoveryMenuInputQuarantineLoggedHeldAfterWindow))
    {
        mod::Log(
            "RECOVERY_MENU_INPUT_QUARANTINE_SUPPRESS framesRemaining=%d actionHeld=%d axisHeld=%d escapeHeld=%d "
            "p1(h=%d v=%d c=%u b=%u d=%u) p2(h=%d v=%d c=%u b=%u d=%u) menu=%s selection=%d",
            g_recoveryMenuInputQuarantineFramesRemaining,
            actionHeld ? 1 : 0,
            axisHeld ? 1 : 0,
            escapeHeld ? 1 : 0,
            inputBytes != nullptr ? static_cast<int>(static_cast<int8_t>(inputBytes[12])) : 0,
            inputBytes != nullptr ? static_cast<int>(static_cast<int8_t>(inputBytes[14])) : 0,
            inputBytes != nullptr ? static_cast<unsigned>(inputBytes[16]) : 0u,
            inputBytes != nullptr ? static_cast<unsigned>(inputBytes[18]) : 0u,
            inputBytes != nullptr ? static_cast<unsigned>(inputBytes[22]) : 0u,
            inputBytes != nullptr ? static_cast<int>(static_cast<int8_t>(inputBytes[13])) : 0,
            inputBytes != nullptr ? static_cast<int>(static_cast<int8_t>(inputBytes[15])) : 0,
            inputBytes != nullptr ? static_cast<unsigned>(inputBytes[17]) : 0u,
            inputBytes != nullptr ? static_cast<unsigned>(inputBytes[19]) : 0u,
            inputBytes != nullptr ? static_cast<unsigned>(inputBytes[23]) : 0u,
            MenuIdToString(g_netplayMenuState.menuId),
            static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
        g_recoveryMenuInputQuarantineLoggedSuppress = true;
        if (heldAfterWindow)
        {
            g_recoveryMenuInputQuarantineLoggedHeldAfterWindow = true;
        }
    }

    if ((cancelHeld || escapeHeld) && !g_recoveryMenuInputQuarantineLoggedLeaveSuppress)
    {
        mod::Log(
            "RECOVERY_MENU_LEAVE_SUPPRESSED reason=input_quarantine cancelHeld=%d escapeHeld=%d framesRemaining=%d",
            cancelHeld ? 1 : 0,
            escapeHeld ? 1 : 0,
            g_recoveryMenuInputQuarantineFramesRemaining);
        g_recoveryMenuInputQuarantineLoggedLeaveSuppress = true;
    }

    if (inactivityCounter != nullptr)
    {
        ++(*inactivityCounter);
    }
    return true;
}

const uint8_t* FilterMenuInputsForWindowFocus(const uint8_t* rawInputBytes, bool windowFocused)
{
    g_filteredMenuInputs.fill(0);
    if (rawInputBytes == nullptr)
    {
        return g_filteredMenuInputs.data();
    }

    if (!windowFocused)
    {
        for (int offset = kMenuInputFirstOffset; offset <= kMenuInputLastOffset; ++offset)
        {
            if (rawInputBytes[offset] != 0)
            {
                g_unfocusedHeldMenuInputs[static_cast<size_t>(offset)] = 1;
            }
        }
        return g_filteredMenuInputs.data();
    }

    std::memcpy(g_filteredMenuInputs.data(), rawInputBytes, kFilteredMenuInputBytes);
    for (int offset = kMenuInputFirstOffset; offset <= kMenuInputLastOffset; ++offset)
    {
        const size_t index = static_cast<size_t>(offset);
        if (g_unfocusedHeldMenuInputs[index] == 0)
        {
            continue;
        }
        if (rawInputBytes[offset] == 0)
        {
            g_unfocusedHeldMenuInputs[index] = 0;
            continue;
        }
        g_filteredMenuInputs[index] = 0;
    }

    return g_filteredMenuInputs.data();
}

bool ConsumeWindowFocusedHotkeyEdge(bool windowFocused, int virtualKey)
{
    if (virtualKey < 0 || virtualKey >= 256)
    {
        return false;
    }

    const bool down = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    const size_t index = static_cast<size_t>(virtualKey);
    const bool pressed = windowFocused && down && g_netplayHotkeyDown[index] == 0;
    g_netplayHotkeyDown[index] = down ? 1u : 0u;
    return pressed;
}

bool ConsumeJoinWaitToSpectateHotkeyEdge(const uint8_t* inputBytes)
{
    if (inputBytes == nullptr)
    {
        g_joinWaitToSpectateButtonDown = {};
        return false;
    }

    bool pressed = false;
    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        const bool down = inputBytes[playerIndex + 22] != 0;
        if (down && g_joinWaitToSpectateButtonDown[static_cast<size_t>(playerIndex)] == 0)
        {
            pressed = true;
        }
        g_joinWaitToSpectateButtonDown[static_cast<size_t>(playerIndex)] = down ? 1u : 0u;
    }

    return pressed;
}

bool IsTransientNetplayOverlayActive()
{
    return g_joiningOverlay.active
        || g_hostingOverlay.active
        || g_delaySetupOverlay.active
        || g_spectateConfirmOverlay.active;
}

void ResetLobbyChallengeNotificationState()
{
    g_seenLobbyChallengeIds.clear();
}

void UpdateLobbyChallengeNotificationState(
    uint32_t screenContext,
    const netplay::lobby::LobbyStatus& status)
{
    std::vector<int> currentIds;
    currentIds.reserve(status.challenges.size());
    bool hasNewChallenge = false;

    for (const auto& challenge : status.challenges)
    {
        if (challenge.playerId == 0)
        {
            continue;
        }

        currentIds.push_back(challenge.playerId);
        if (std::find(g_seenLobbyChallengeIds.begin(), g_seenLobbyChallengeIds.end(), challenge.playerId)
            == g_seenLobbyChallengeIds.end())
        {
            hasNewChallenge = true;
        }
    }

    if (hasNewChallenge)
    {
        PlayLobbyChallengeAlert(screenContext);
        mod::Log(
            "LobbyChallengeNotify: played alert for %zu incoming challenge(s)",
            status.challenges.size());
    }

    g_seenLobbyChallengeIds = std::move(currentIds);
}

bool PlayBackgroundMusicFromAbsolutePath(
    int gameSystem,
    const std::string& bgmPath,
    unsigned short trackNumber)
{
    auto const stopSoundBuffer =
        reinterpret_cast<StopSoundBufferFn>(RuntimeAddress(kVaStopSoundBuffer));
    auto const playSoundBuffer =
        reinterpret_cast<PlaySoundBufferFn>(RuntimeAddress(kVaPlaySoundBuffer));
    auto const releaseSoundBufferAndMemory =
        reinterpret_cast<ReleaseSoundBufferAndMemoryFn>(RuntimeAddress(kVaReleaseSoundBufferAndMemory));
    auto const loadWaveFile =
        reinterpret_cast<LoadWaveFileFn>(RuntimeAddress(kVaLoadWaveFile));
    auto const loadAudioTimingData =
        reinterpret_cast<LoadAudioTimingDataFn>(RuntimeAddress(kVaLoadAudioTimingData));

    auto* const soundManager =
        *reinterpret_cast<uint32_t**>(gameSystem + kOffsetWindowHandle);
    auto* const bgmBufferIndex =
        reinterpret_cast<uint16_t*>(gameSystem + 3878);
    if (soundManager == nullptr || bgmBufferIndex == nullptr)
    {
        mod::Log("PlayNetplayBgm: absolute load aborted (soundManager unavailable)");
        return false;
    }

    if (*bgmBufferIndex != 150)
    {
        stopSoundBuffer(soundManager, *bgmBufferIndex);
        releaseSoundBufferAndMemory(soundManager, *bgmBufferIndex);
        *bgmBufferIndex = 150;
    }

    char* const mutablePath = const_cast<char*>(bgmPath.c_str());
    *bgmBufferIndex = loadWaveFile(soundManager, mutablePath);
    loadAudioTimingData(soundManager, bgmPath.c_str());

    if (*bgmBufferIndex == 150)
    {
        mod::Log("PlayNetplayBgm: absolute load FAILED path='%s'", bgmPath.c_str());
        return false;
    }

    const bool isNonLoopingTrack =
        trackNumber == 2 || trackNumber == 3 || trackNumber == 4 || trackNumber == 9;
    playSoundBuffer(soundManager, *bgmBufferIndex, isNonLoopingTrack ? 0 : 1);

    const unsigned long dataBytes = static_cast<unsigned long>(soundManager[*bgmBufferIndex + 154]);
    mod::Log(
        "PlayNetplayBgm: absolute load OK path='%s' buffer=%u dataBytes=%lu",
        bgmPath.c_str(),
        static_cast<unsigned>(*bgmBufferIndex),
        dataBytes);
    return true;
}

void ReleaseLoadedSoundBuffer(
    int gameSystem,
    unsigned short* ioBufferIndex,
    const char* reason)
{
    if (ioBufferIndex == nullptr || *ioBufferIndex == kInvalidSoundBufferIndex)
    {
        return;
    }

    auto const stopSoundBuffer =
        reinterpret_cast<StopSoundBufferFn>(RuntimeAddress(kVaStopSoundBuffer));
    auto const releaseSoundBufferAndMemory =
        reinterpret_cast<ReleaseSoundBufferAndMemoryFn>(RuntimeAddress(kVaReleaseSoundBufferAndMemory));

    auto* const soundManager =
        *reinterpret_cast<uint32_t**>(gameSystem + kOffsetWindowHandle);
    if (soundManager != nullptr)
    {
        stopSoundBuffer(soundManager, *ioBufferIndex);
        releaseSoundBufferAndMemory(soundManager, *ioBufferIndex);
        mod::Log(
            "ReleaseLoadedSoundBuffer: released buffer=%u reason=%s",
            static_cast<unsigned>(*ioBufferIndex),
            reason != nullptr ? reason : "");
    }

    *ioBufferIndex = kInvalidSoundBufferIndex;
}

bool PlayOneShotWaveFromAbsolutePath(
    int gameSystem,
    const std::string& wavePath,
    unsigned short* ioBufferIndex)
{
    auto const playSoundBuffer =
        reinterpret_cast<PlaySoundBufferFn>(RuntimeAddress(kVaPlaySoundBuffer));
    auto const loadWaveFile =
        reinterpret_cast<LoadWaveFileFn>(RuntimeAddress(kVaLoadWaveFile));

    auto* const soundManager =
        *reinterpret_cast<uint32_t**>(gameSystem + kOffsetWindowHandle);
    if (soundManager == nullptr || ioBufferIndex == nullptr)
    {
        mod::Log("PlayOneShotWaveFromAbsolutePath: aborted (soundManager unavailable)");
        return false;
    }

    ReleaseLoadedSoundBuffer(gameSystem, ioBufferIndex, "reload_one_shot");

    char* const mutablePath = const_cast<char*>(wavePath.c_str());
    *ioBufferIndex = loadWaveFile(soundManager, mutablePath);
    if (*ioBufferIndex == kInvalidSoundBufferIndex)
    {
        mod::Log("PlayOneShotWaveFromAbsolutePath: load FAILED path='%s'", wavePath.c_str());
        return false;
    }

    playSoundBuffer(soundManager, *ioBufferIndex, 0);
    mod::Log(
        "PlayOneShotWaveFromAbsolutePath: load OK path='%s' buffer=%u",
        wavePath.c_str(),
        static_cast<unsigned>(*ioBufferIndex));
    return true;
}

void PlayLobbyChallengeAlert(uint32_t screenContext)
{
    const int gameSystem = GetGameSystem(screenContext);
    const std::string alertPath =
        netplay::assets::ResolveChallengeAlertPath(g_moduleDirectory);
    if (!alertPath.empty()
        && PlayOneShotWaveFromAbsolutePath(
            gameSystem,
            alertPath,
            &g_lobbyChallengeAlertBufferIndex))
    {
        return;
    }

    mod::Log("PlayLobbyChallengeAlert: using fallback UI SFX");
    PlayUiSound(screenContext, kSfxConfirm);
}

void PlayNetplayBgm(uint32_t screenContext)
{
    auto const playBackgroundMusic =
        reinterpret_cast<PlayBackgroundMusicFn>(RuntimeAddress(kVaPlayBackgroundMusic));
    const int gameSystem = GetGameSystem(screenContext);

    const std::string bgmBaseDirectory =
        netplay::assets::ResolveNetplayBgmBaseDirectory(g_moduleDirectory);
    if (bgmBaseDirectory.empty())
    {
        mod::Log("PlayNetplayBgm: using vanilla path 'wave\\bgm\\bgm08.wav'");
        playBackgroundMusic(gameSystem, kNetplayBgmTrack);
        return;
    }

    const std::string bgmPath =
        netplay::assets::JoinPath(bgmBaseDirectory, "wave\\bgm\\bgm08.wav");
    mod::Log("PlayNetplayBgm: override file '%s'", bgmPath.c_str());
    if (!PlayBackgroundMusicFromAbsolutePath(gameSystem, bgmPath, kNetplayBgmTrack))
    {
        mod::Log("PlayNetplayBgm: absolute override failed; falling back to vanilla path");
        playBackgroundMusic(gameSystem, kNetplayBgmTrack);
    }
}

void CopyBoundedText(char* dst, size_t dstSize, const char* src)
{
    if (dst == nullptr || dstSize == 0)
    {
        return;
    }
    if (src == nullptr)
    {
        dst[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(dst, dstSize, src, _TRUNCATE);
#else
    std::snprintf(dst, dstSize, "%s", src);
#endif
}

void ResetDelaySetupOverlayState()
{
    ResetVsHumanHandoffWarmup(nullptr);
    g_delaySetupOverlay = {};
}

void ResetSpectateConfirmOverlayState()
{
    g_spectateConfirmOverlay = {};
}

} // namespace (close anonymous to expose hosting overlay functions)

// ---------------------------------------------------------------------------
// Hosting overlay - discovers a family-matched public address before starting
// the Revival netplay session. The worker owns only its task result; the menu
// thread is the sole writer of overlay and bridge state.
// ---------------------------------------------------------------------------
void ResetHostingOverlayState()
{
    ++g_directHostDiscoveryGeneration;
    if (g_pendingHostDiscovery)
    {
        g_pendingHostDiscovery->cancelled.store(true);
        g_pendingHostDiscovery.reset();
    }
    g_pendingLobbyChallengePublish = {};
    g_hostingOverlay = {};
}

static void BeginHostDiscovery(
    uint16_t port,
    netplay::network::NetworkFamily originalPreferredFamily,
    netplay::network::NetworkFamily discoveryFamily,
    bool allowAlternateFamily,
    bool automaticFamilyRetryAttempted,
    const char* nickname,
    bool writeNicknameToIni,
    HostDiscoveryOwner owner,
    int targetPlayerId,
    const char* targetName)
{
    // Scheduling only creates a worker; it never runs a local socket probe on
    // the game thread. Host consumes a completed fresh result if one exists,
    // otherwise it starts its normal global-address discovery immediately.
    EnsureLocalNetworkCapabilityScanScheduled("host_requested");
    netplay::network::LocalNetworkCapabilitySnapshot capabilitySnapshot;
    DWORD capabilitySnapshotAgeMs = 0;
    const bool capabilitySnapshotAvailable =
        TryGetFreshLocalNetworkCapabilitySnapshot(
            &capabilitySnapshot, &capabilitySnapshotAgeMs);

    ResetHostingOverlayState();
    g_hostingOverlay.active = true;
    g_hostingOverlay.port = port;
    g_hostingOverlay.challengeMode =
        owner == HostDiscoveryOwner::LobbyChallenge;
    g_hostingOverlay.preferredFamily = originalPreferredFamily;
    g_hostingOverlay.effectiveFamily = discoveryFamily;
    g_hostingOverlay.usedFamilyFallback =
        originalPreferredFamily != discoveryFamily;
    g_hostingOverlay.discoveryInProgress = true;
    g_hostingOverlay.familyRetryAllowed = true;
    g_hostingOverlay.automaticFamilyRetryAttempted =
        automaticFamilyRetryAttempted;
    g_hostingOverlay.writeNicknameToIni = writeNicknameToIni;
    CopyBoundedText(
        g_hostingOverlay.hostNickname,
        sizeof(g_hostingOverlay.hostNickname),
        nickname);
    CopyBoundedText(
        g_hostingOverlay.targetName,
        sizeof(g_hostingOverlay.targetName),
        targetName);

    auto task = std::make_shared<PendingHostDiscovery>();
    task->generation = g_directHostDiscoveryGeneration;
    task->owner = owner;
    task->port = port;
    task->originalPreferredFamily = originalPreferredFamily;
    task->discoveryFamily = discoveryFamily;
    task->allowAlternateFamily = allowAlternateFamily;
    task->automaticFamilyRetryAttempted =
        automaticFamilyRetryAttempted;
    task->nickname = nickname != nullptr ? nickname : "";
    task->writeNicknameToIni = writeNicknameToIni;
    task->targetPlayerId = targetPlayerId;
    task->targetName = targetName != nullptr ? targetName : "";
    task->capabilitySnapshotAvailable = capabilitySnapshotAvailable;
    if (capabilitySnapshotAvailable)
    {
        task->capabilitySnapshot = capabilitySnapshot;
    }
    g_pendingHostDiscovery = task;

    mod::Log(
        "PUBLIC_IP_DISCOVERY_BEGIN owner=%s generation=%llu "
        "originalPreferred=%s discoveryFamily=%s allowAlternate=%d "
        "automaticRetry=%d port=%u targetId=%d capabilityCache=%s "
        "capabilityAgeMs=%lu ipv4=%s ipv6=%s",
        owner == HostDiscoveryOwner::LobbyChallenge
            ? "lobby_challenge"
            : "direct_host",
        static_cast<unsigned long long>(task->generation),
        netplay::network::FamilyName(originalPreferredFamily),
        netplay::network::FamilyName(discoveryFamily),
        allowAlternateFamily ? 1 : 0,
        automaticFamilyRetryAttempted ? 1 : 0,
        static_cast<unsigned>(port),
        targetPlayerId,
        capabilitySnapshotAvailable ? "completed" : "unavailable",
        static_cast<unsigned long>(capabilitySnapshotAgeMs),
        capabilitySnapshotAvailable
            ? netplay::network::LocalFamilyCapabilityStateName(
                capabilitySnapshot.ipv4.state)
            : "not_ready",
        capabilitySnapshotAvailable
            ? netplay::network::LocalFamilyCapabilityStateName(
                capabilitySnapshot.ipv6.state)
            : "not_ready");

    HMODULE moduleReference = nullptr;
    const BOOL retainedModule = GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCSTR>(&HostDiscoveryThreadMain),
        &moduleReference);
    auto* context = retainedModule != FALSE
        ? new (std::nothrow) HostDiscoveryThreadContext()
        : nullptr;
    if (context == nullptr)
    {
        if (moduleReference != nullptr)
        {
            FreeLibrary(moduleReference);
        }
        task->result = {};
        task->result.preferredFamily = discoveryFamily;
        task->result.effectiveFamily = discoveryFamily;
        task->completed.store(true, std::memory_order_release);
        mod::Log(
            "PUBLIC_IP_DISCOVERY_BEGIN_FAILED generation=%llu "
            "family=%s reason=%s",
            static_cast<unsigned long long>(task->generation),
            netplay::network::FamilyName(discoveryFamily),
            retainedModule == FALSE
                ? "module_reference"
                : "allocation");
        return;
    }

    context->task = task;
    context->moduleReference = moduleReference;
    HANDLE worker = CreateThread(
        nullptr,
        0,
        &HostDiscoveryThreadMain,
        context,
        0,
        nullptr);
    if (worker == nullptr)
    {
        const DWORD error = GetLastError();
        delete context;
        FreeLibrary(moduleReference);
        task->result = {};
        task->result.preferredFamily = discoveryFamily;
        task->result.effectiveFamily = discoveryFamily;
        task->completed.store(true, std::memory_order_release);
        mod::Log(
            "PUBLIC_IP_DISCOVERY_BEGIN_FAILED generation=%llu "
            "family=%s reason=CreateThread error=%lu",
            static_cast<unsigned long long>(task->generation),
            netplay::network::FamilyName(discoveryFamily),
            static_cast<unsigned long>(error));
        return;
    }
    CloseHandle(worker);
}

static void BeginDirectHostDiscovery(
    uint16_t port,
    netplay::network::NetworkFamily preferredFamily,
    const char* nickname,
    bool writeNicknameToIni)
{
    BeginHostDiscovery(
        port,
        preferredFamily,
        preferredFamily,
        true,
        false,
        nickname,
        writeNicknameToIni,
        HostDiscoveryOwner::Direct,
        0,
        nullptr);
}

void ActivateChallengeHostingOverlay(
    const char* targetName,
    uint16_t port,
    netplay::network::NetworkFamily preferredFamily,
    netplay::network::NetworkFamily effectiveFamily,
    bool usedFamilyFallback,
    const char* publicIp)
{
    ResetHostingOverlayState();
    g_hostingOverlay.active = true;
    g_hostingOverlay.port = port;
    g_hostingOverlay.challengeMode = true;
    g_hostingOverlay.preferredFamily = preferredFamily;
    g_hostingOverlay.effectiveFamily = effectiveFamily;
    g_hostingOverlay.usedFamilyFallback = usedFamilyFallback;
    g_hostingOverlay.ipFetchDone = true;
    g_hostingOverlay.ipFetchFailed = publicIp == nullptr || publicIp[0] == '\0';
    g_hostingOverlay.sessionQueued = true;
    g_hostingOverlay.familyRetryAllowed = true;
    g_hostingOverlay.listenerWaitStartTick = GetTickCount();
    CopyBoundedText(
        g_hostingOverlay.publicIp,
        sizeof(g_hostingOverlay.publicIp),
        publicIp);
    strncpy_s(g_hostingOverlay.targetName, sizeof(g_hostingOverlay.targetName), targetName, _TRUNCATE);
    g_hostingOverlay.targetName[sizeof(g_hostingOverlay.targetName) - 1] = '\0';
    mod::Log(
        "HostingOverlay: activated challenge target='%s' port=%u preferred=%s effective=%s fallback=%d",
        g_hostingOverlay.targetName,
        static_cast<unsigned>(port),
        netplay::network::FamilyName(preferredFamily),
        netplay::network::FamilyName(effectiveFamily),
        usedFamilyFallback ? 1 : 0);
}

static void ShowHostStartupFailure(
    const char* errorText,
    bool needsBridgeCancel)
{
    const std::string error =
        errorText != nullptr && errorText[0] != '\0'
            ? errorText
            : "Unknown Host startup error.";
    ResetHostingOverlayState();
    ResetJoiningOverlayState();
    g_hostingOverlay.active = true;
    g_hostingOverlay.failed = true;
    g_hostingOverlay.failureNeedsBridgeCancel =
        needsBridgeCancel;
    CopyBoundedText(
        g_hostingOverlay.errorText,
        sizeof(g_hostingOverlay.errorText),
        error.c_str());
    mod::Log(
        "REVIVAL_NETPLAY_SESSION_START_FAILED owner=host "
        "overlay=hosting needsBridgeCancel=%d error='%s'",
        needsBridgeCancel ? 1 : 0,
        error.c_str());
}

static bool HostFailureNeedsBridgeCancel()
{
    const auto phase =
        static_cast<netplay::bridge::NetbridgePhase>(
            netplay::bridge::GetStatus().phase);
    return phase != netplay::bridge::NetbridgePhase::Idle;
}

static netplay::network::NetworkFamily AlternateHostFamily(
    netplay::network::NetworkFamily family)
{
    return family == netplay::network::NetworkFamily::IPv6
        ? netplay::network::NetworkFamily::IPv4
        : netplay::network::NetworkFamily::IPv6;
}

static bool IsLobbyChallengeTargetStillIdle(int targetPlayerId)
{
    if (!g_lobbySession || targetPlayerId == 0)
    {
        return false;
    }

    const netplay::lobby::LobbyStatus status =
        g_lobbySession->GetStatus();
    if (status.pollState != netplay::lobby::PollState::Polling)
    {
        return false;
    }

    for (const netplay::lobby::LobbyPlayer& player :
         status.idlePlayers)
    {
        if (player.playerId == targetPlayerId)
        {
            return true;
        }
    }
    return false;
}

static void PumpDirectHostDiscovery()
{
    const std::shared_ptr<PendingHostDiscovery> task =
        g_pendingHostDiscovery;
    if (!task
        || !task->completed.load(std::memory_order_acquire))
    {
        return;
    }

    if (task->automaticFamilyRetryAttempted
        && netplay::bridge::IsSessionStartInProgress())
    {
        return;
    }

    if (task->cancelled.load()
        || task->generation != g_directHostDiscoveryGeneration
        || !g_hostingOverlay.active)
    {
        mod::Log(
            "PUBLIC_IP_DISCOVERY_RESULT owner=%s generation=%llu "
            "discarded=1 cancelled=%d currentGeneration=%llu",
            task->owner == HostDiscoveryOwner::LobbyChallenge
                ? "lobby_challenge"
                : "direct_host",
            static_cast<unsigned long long>(task->generation),
            task->cancelled.load() ? 1 : 0,
            static_cast<unsigned long long>(
                g_directHostDiscoveryGeneration));
        if (g_pendingHostDiscovery == task)
        {
            g_pendingHostDiscovery.reset();
        }
        return;
    }

    const netplay::lobby::PublicIpDiscoveryResult result = task->result;
    g_pendingHostDiscovery.reset();
    g_hostingOverlay.discoveryInProgress = false;
    g_hostingOverlay.ipFetchDone = true;
    g_hostingOverlay.ipFetchFailed = result.publicIp.empty();
    g_hostingOverlay.preferredFamily =
        task->originalPreferredFamily;
    g_hostingOverlay.effectiveFamily = result.effectiveFamily;
    g_hostingOverlay.usedFamilyFallback =
        task->originalPreferredFamily != result.effectiveFamily;
    g_hostingOverlay.automaticFamilyRetryAttempted =
        task->automaticFamilyRetryAttempted;
    g_hostingOverlay.writeNicknameToIni =
        task->writeNicknameToIni;
    CopyBoundedText(
        g_hostingOverlay.hostNickname,
        sizeof(g_hostingOverlay.hostNickname),
        task->nickname.c_str());
    CopyBoundedText(
        g_hostingOverlay.publicIp,
        sizeof(g_hostingOverlay.publicIp),
        result.publicIp.c_str());

    const auto& preferredDiagnostics =
        result.preferredFamily == netplay::network::NetworkFamily::IPv6
            ? result.ipv6
            : result.ipv4;
    const auto& alternateDiagnostics =
        result.preferredFamily == netplay::network::NetworkFamily::IPv6
            ? result.ipv4
            : result.ipv6;
    g_hostingOverlay.preferredSourceAttempts =
        preferredDiagnostics.sourceAttempts;
    g_hostingOverlay.preferredTransportFailures =
        preferredDiagnostics.transportFailures;
    g_hostingOverlay.preferredParseFailures =
        preferredDiagnostics.parseFailures;
    g_hostingOverlay.alternateSourceAttempts =
        alternateDiagnostics.sourceAttempts;
    g_hostingOverlay.alternateTransportFailures =
        alternateDiagnostics.transportFailures;
    g_hostingOverlay.alternateParseFailures =
        alternateDiagnostics.parseFailures;

    mod::Log(
        "PUBLIC_IP_DISCOVERY_RESULT owner=%s generation=%llu "
        "originalPreferred=%s discoveryFamily=%s effective=%s fallback=%d "
        "automaticRetry=%d address='%s' "
        "preferredAttempts=%u preferredTransport=%u preferredParse=%u "
        "alternateAttempts=%u alternateTransport=%u alternateParse=%u",
        task->owner == HostDiscoveryOwner::LobbyChallenge
            ? "lobby_challenge"
            : "direct_host",
        static_cast<unsigned long long>(task->generation),
        netplay::network::FamilyName(
            task->originalPreferredFamily),
        netplay::network::FamilyName(task->discoveryFamily),
        netplay::network::FamilyName(result.effectiveFamily),
        task->originalPreferredFamily != result.effectiveFamily
            ? 1
            : 0,
        task->automaticFamilyRetryAttempted ? 1 : 0,
        result.publicIp.empty() ? "(none)" : result.publicIp.c_str(),
        preferredDiagnostics.sourceAttempts,
        preferredDiagnostics.transportFailures,
        preferredDiagnostics.parseFailures,
        alternateDiagnostics.sourceAttempts,
        alternateDiagnostics.transportFailures,
        alternateDiagnostics.parseFailures);

    if (task->owner == HostDiscoveryOwner::LobbyChallenge)
    {
        if (!IsLobbyChallengeTargetStillIdle(
                task->targetPlayerId))
        {
            mod::Log(
                "LOBBY_ENDPOINT_PUBLISH result=cancelled "
                "reason=target_not_idle_before_retry_start target='%s' id=%d",
                task->targetName.c_str(),
                task->targetPlayerId);
            ShowHostStartupFailure(
                "That player is no longer available.",
                HostFailureNeedsBridgeCancel());
            return;
        }
        if (result.publicIp.empty())
        {
            mod::Log(
                "LOBBY_ENDPOINT_PUBLISH result=cancelled "
                "reason=public_address_unavailable family=%s target='%s' id=%d",
                netplay::network::FamilyName(result.effectiveFamily),
                task->targetName.c_str(),
                task->targetPlayerId);
            ShowHostStartupFailure(
                "A public address could not be detected for the available connection type.",
                HostFailureNeedsBridgeCancel());
            return;
        }
    }

    netplay::bridge::HostSessionNetworkConfig networkConfig;
    networkConfig.preferredFamily =
        task->originalPreferredFamily;
    networkConfig.effectiveFamily = result.effectiveFamily;
    networkConfig.publicAddress = result.publicIp;
    networkConfig.automaticFamilyRetryAttempted =
        task->automaticFamilyRetryAttempted;
    netplay::bridge::HostStartFailure startFailure =
        netplay::bridge::HostStartFailure::None;
    const bool started = netplay::bridge::StartHostSession(
        task->port,
        task->nickname.c_str(),
        networkConfig,
        task->writeNicknameToIni,
        &startFailure);
    if (!started)
    {
        if (startFailure
                == netplay::bridge::HostStartFailure::FamilyUnavailable
            && !task->automaticFamilyRetryAttempted)
        {
            const netplay::network::NetworkFamily retryFamily =
                AlternateHostFamily(result.effectiveFamily);
            mod::Log(
                "REVIVAL_HOST_FAMILY_RETRY trigger=sync_preflight "
                "from=%s to=%s owner=%s port=%u",
                netplay::network::FamilyName(result.effectiveFamily),
                netplay::network::FamilyName(retryFamily),
                task->owner == HostDiscoveryOwner::LobbyChallenge
                    ? "lobby_challenge"
                    : "direct_host",
                static_cast<unsigned>(task->port));
            netplay::bridge::CancelSession(
                "host_sync_preflight_retry_ack");
            BeginHostDiscovery(
                task->port,
                task->originalPreferredFamily,
                retryFamily,
                false,
                true,
                task->nickname.c_str(),
                task->writeNicknameToIni,
                task->owner,
                task->targetPlayerId,
                task->targetName.c_str());
            return;
        }

        const netplay::bridge::NetbridgeStatus status =
            netplay::bridge::GetStatus();
        ShowHostStartupFailure(
            status.errorMsg[0] != '\0'
                ? status.errorMsg
                : "Could not start the Revival netplay session.",
            true);
        return;
    }

    if (task->owner == HostDiscoveryOwner::LobbyChallenge)
    {
        ActivateChallengeHostingOverlay(
            task->targetName.c_str(),
            task->port,
            networkConfig.preferredFamily,
            networkConfig.effectiveFamily,
            networkConfig.preferredFamily
                != networkConfig.effectiveFamily,
            networkConfig.publicAddress.c_str());
        g_hostingOverlay.automaticFamilyRetryAttempted =
            task->automaticFamilyRetryAttempted;
        g_hostingOverlay.writeNicknameToIni =
            task->writeNicknameToIni;
        CopyBoundedText(
            g_hostingOverlay.hostNickname,
            sizeof(g_hostingOverlay.hostNickname),
            task->nickname.c_str());

        g_pendingLobbyChallengePublish.active = true;
        g_pendingLobbyChallengePublish.targetPlayerId =
            task->targetPlayerId;
        g_pendingLobbyChallengePublish.targetName =
            task->targetName;
        g_pendingLobbyChallengePublish.publicAddress =
            networkConfig.publicAddress;
        g_pendingLobbyChallengePublish.preferredFamily =
            networkConfig.preferredFamily;
        g_pendingLobbyChallengePublish.family =
            networkConfig.effectiveFamily;
        g_pendingLobbyChallengePublish.port = task->port;
        g_pendingLobbyChallengePublish
            .automaticFamilyRetryAttempted =
            task->automaticFamilyRetryAttempted;
    }
    else
    {
        g_hostingOverlay.sessionQueued = true;
        netplay::bridge::async_host::OnHostStarted(
            task->port,
            task->nickname.c_str(),
            networkConfig);
    }
    netplay::battle_log::EnsureGameplayOverlayHook();
    mod::Log(
        "REVIVAL_NETPLAY_SESSION_START_QUEUED owner=%s preferred=%s "
        "effective=%s fallback=%d automaticRetry=%d port=%u "
        "publicAddress='%s'",
        task->owner == HostDiscoveryOwner::LobbyChallenge
            ? "lobby_challenge"
            : "direct_host",
        netplay::network::FamilyName(networkConfig.preferredFamily),
        netplay::network::FamilyName(networkConfig.effectiveFamily),
        networkConfig.preferredFamily != networkConfig.effectiveFamily
            ? 1
            : 0,
        task->automaticFamilyRetryAttempted ? 1 : 0,
        static_cast<unsigned>(task->port),
        networkConfig.publicAddress.empty()
            ? "(none)"
            : networkConfig.publicAddress.c_str());
}

static bool BeginAutomaticHostFamilyRetry(
    netplay::network::NetworkFamily retryFamily,
    const char* trigger,
    uint32_t listenerSerial)
{
    if (!g_hostingOverlay.active
        || !g_hostingOverlay.familyRetryAllowed
        || g_hostingOverlay.automaticFamilyRetryAttempted)
    {
        mod::Log(
            "HOST_FAMILY_FALLBACK_SKIPPED reason=%s "
            "trigger=%s from=%s requested=%s listenerSerial=%u",
            !g_hostingOverlay.familyRetryAllowed
                ? "retry_not_allowed_for_rehost"
                : "already_attempted",
            trigger != nullptr ? trigger : "",
            netplay::network::FamilyName(
                g_hostingOverlay.effectiveFamily),
            netplay::network::FamilyName(retryFamily),
            listenerSerial);
        return false;
    }

    const bool lobbyChallenge =
        g_hostingOverlay.challengeMode;
    if (lobbyChallenge
        && (!g_pendingLobbyChallengePublish.active
            || !IsLobbyChallengeTargetStillIdle(
                g_pendingLobbyChallengePublish.targetPlayerId)))
    {
        mod::Log(
            "HOST_FAMILY_FALLBACK_SKIPPED reason=lobby_target_not_idle "
            "trigger=%s listenerSerial=%u",
            trigger != nullptr ? trigger : "",
            listenerSerial);
        netplay::bridge::async_host::Reset();
        netplay::bridge::CancelSession(
            "host_family_retry_target_gone");
        ShowHostStartupFailure(
            "That player is no longer available.",
            true);
        return true;
    }

    const uint16_t port = g_hostingOverlay.port;
    const netplay::network::NetworkFamily originalPreferred =
        g_hostingOverlay.preferredFamily;
    const netplay::network::NetworkFamily previousFamily =
        g_hostingOverlay.effectiveFamily;
    const std::string nickname =
        g_hostingOverlay.hostNickname;
    const bool writeNicknameToIni =
        g_hostingOverlay.writeNicknameToIni;
    const int targetPlayerId = lobbyChallenge
        ? g_pendingLobbyChallengePublish.targetPlayerId
        : 0;
    const std::string targetName = lobbyChallenge
        ? g_pendingLobbyChallengePublish.targetName
        : std::string();

    mod::Log(
        "HOST_FAMILY_FALLBACK_BEGIN owner=%s trigger=%s attempt=2 "
        "originalPreferred=%s from=%s to=%s port=%u "
        "listenerSerial=%u",
        lobbyChallenge ? "lobby_challenge" : "direct_host",
        trigger != nullptr ? trigger : "",
        netplay::network::FamilyName(originalPreferred),
        netplay::network::FamilyName(previousFamily),
        netplay::network::FamilyName(retryFamily),
        static_cast<unsigned>(port),
        listenerSerial);

    // The helper exists on listener/terminal retry paths. Reset async tracking
    // without asking it to cancel, then perform exactly one bridge teardown.
    netplay::bridge::async_host::Reset();
    netplay::bridge::CancelSession("host_family_retry");
    BeginHostDiscovery(
        port,
        originalPreferred,
        retryFamily,
        false,
        true,
        nickname.c_str(),
        writeNicknameToIni,
        lobbyChallenge
            ? HostDiscoveryOwner::LobbyChallenge
            : HostDiscoveryOwner::Direct,
        targetPlayerId,
        targetName.c_str());
    return true;
}

static bool TryBeginTerminalHostFamilyRetry(
    const netplay::bridge::NetbridgeStatus& status,
    netplay::bridge::NetbridgePhase phase)
{
    if (!g_hostingOverlay.active
        || !g_hostingOverlay.sessionQueued
        || g_hostingOverlay.listenerReady
        || g_hostingOverlay.automaticFamilyRetryAttempted
        || (phase != netplay::bridge::NetbridgePhase::Failed
            && phase
                != netplay::bridge::NetbridgePhase::SessionEnded))
    {
        return false;
    }

    // Never run a socket probe from this game-thread failure path. A fresh
    // background result can justify one controlled retry; otherwise preserve
    // the native failure and let the user see Revival's actual error.
    EnsureLocalNetworkCapabilityScanScheduled("terminal_host_failure");
    netplay::network::LocalNetworkCapabilitySnapshot capabilitySnapshot;
    DWORD capabilityAgeMs = 0;
    if (!TryGetFreshLocalNetworkCapabilitySnapshot(
            &capabilitySnapshot, &capabilityAgeMs))
    {
        mod::Log(
            "HOST_FAMILY_FALLBACK_SKIPPED reason=capability_cache_unavailable "
            "phase=%s family=%s cacheAgeMs=%lu error='%s'",
            netplay::bridge::PhaseToString(phase),
            netplay::network::FamilyName(
                g_hostingOverlay.effectiveFamily),
            static_cast<unsigned long>(capabilityAgeMs),
            status.errorMsg);
        return false;
    }

    const netplay::network::NetworkFamily retryFamily =
        AlternateHostFamily(g_hostingOverlay.effectiveFamily);
    const netplay::network::LocalFamilyCapability& currentCapability =
        netplay::network::GetFamilyCapability(
            capabilitySnapshot, g_hostingOverlay.effectiveFamily);
    const netplay::network::LocalFamilyCapability& retryCapability =
        netplay::network::GetFamilyCapability(
            capabilitySnapshot, retryFamily);
    const bool currentHardUnavailable =
        currentCapability.state
        == netplay::network::LocalFamilyCapabilityState::HardUnavailable;
    const bool currentLocalOnly =
        currentCapability.state
        == netplay::network::LocalFamilyCapabilityState::LocalOnly;
    const bool retryHardUnavailable =
        retryCapability.state
        == netplay::network::LocalFamilyCapabilityState::HardUnavailable;
    const bool retryIsGloballyEligible =
        netplay::network::CanAttemptGlobalPublicDiscovery(retryCapability);
    if ((!currentHardUnavailable
         && !(currentLocalOnly && retryIsGloballyEligible))
        || retryHardUnavailable)
    {
        mod::Log(
            "HOST_FAMILY_FALLBACK_SKIPPED reason=terminal_failure_not_cache_eligible "
            "phase=%s family=%s familyState=%s alternate=%s alternateState=%s "
            "alternateGlobalEligible=%d cacheAgeMs=%lu error='%s'",
            netplay::bridge::PhaseToString(phase),
            netplay::network::FamilyName(
                g_hostingOverlay.effectiveFamily),
            netplay::network::LocalFamilyCapabilityStateName(
                currentCapability.state),
            netplay::network::FamilyName(retryFamily),
            netplay::network::LocalFamilyCapabilityStateName(
                retryCapability.state),
            retryIsGloballyEligible ? 1 : 0,
            static_cast<unsigned long>(capabilityAgeMs),
            status.errorMsg);
        return false;
    }

    return BeginAutomaticHostFamilyRetry(
        retryFamily,
        currentHardUnavailable
            ? "terminal_cached_hard_unavailable"
            : "terminal_cached_local_only",
        0);
}

static void PumpHostListenerObservation()
{
    if (g_hostingOverlay.active
        && g_hostingOverlay.sessionQueued
        && g_hostingOverlay.listenerReady
        && netplay::bridge::async_host::IsActive()
        && !netplay::bridge::async_host::IsHostListenerReady())
    {
        g_hostingOverlay.listenerReady = false;
        g_hostingOverlay.listenerMismatch = false;
        mod::Log(
            "REVIVAL_HOST_ACK_RESET reason=async_rehost_waiting "
            "family=%s port=%u",
            netplay::network::FamilyName(
                g_hostingOverlay.effectiveFamily),
            static_cast<unsigned>(g_hostingOverlay.port));
    }

    if (!g_hostingOverlay.active
        || !g_hostingOverlay.sessionQueued
        || g_hostingOverlay.listenerReady)
    {
        return;
    }

    constexpr DWORD kChallengeListenerAckTimeoutMs = 15000u;
    if (g_hostingOverlay.challengeMode
        && g_hostingOverlay.listenerWaitStartTick != 0
        && GetTickCount() - g_hostingOverlay.listenerWaitStartTick
            >= kChallengeListenerAckTimeoutMs)
    {
        const netplay::bridge::NetbridgeStatus status =
            netplay::bridge::GetStatus();
        mod::Log(
            "LOBBY_HOST_LISTENER_TIMEOUT elapsedMs=%lu phase=%s "
            "helperPid=%lu family=%s port=%u action=cancel",
            static_cast<unsigned long>(
                GetTickCount()
                - g_hostingOverlay.listenerWaitStartTick),
            netplay::bridge::PhaseToString(
                static_cast<netplay::bridge::NetbridgePhase>(
                    status.phase)),
            static_cast<unsigned long>(status.processId),
            netplay::network::FamilyName(
                g_hostingOverlay.effectiveFamily),
            static_cast<unsigned>(g_hostingOverlay.port));
        netplay::bridge::async_host::Reset();
        netplay::bridge::CancelSession(
            "lobby_host_listener_ack_timeout");
        ShowHostStartupFailure(
            "Hosting did not start in time. Please try again.",
            false);
        return;
    }

    netplay::bridge::HostListenerObservation observation;
    if (!netplay::bridge::GetHostListenerObservation(
            &observation)
        || !observation.available)
    {
        return;
    }

    const netplay::bridge::NetbridgeStatus bridgeStatus =
        netplay::bridge::GetStatus();
    const auto bridgePhase =
        static_cast<netplay::bridge::NetbridgePhase>(
            bridgeStatus.phase);
    const bool peerAlive =
        netplay::bridge::IsPeerProcessAlive();
    if (bridgePhase == netplay::bridge::NetbridgePhase::Failed
        || bridgePhase == netplay::bridge::NetbridgePhase::SessionEnded
        || bridgePhase == netplay::bridge::NetbridgePhase::Idle
        || !peerAlive)
    {
        mod::Log(
            "REVIVAL_HOST_ACK_IGNORED reason=session_not_live phase=%s "
            "peerAlive=%d observedFamily=%s observedPort=%u serial=%u",
            netplay::bridge::PhaseToString(bridgePhase),
            peerAlive ? 1 : 0,
            netplay::network::FamilyName(observation.family),
            static_cast<unsigned>(observation.port),
            observation.serial);
        return;
    }

    const DWORD listenerCandidateTick = GetTickCount();
    if (g_hostingOverlay.listenerMismatchSerial
            != observation.serial)
    {
        g_hostingOverlay.listenerMismatchSerial =
            observation.serial;
        g_hostingOverlay.listenerMismatchPort =
            observation.port;
        g_hostingOverlay.listenerMismatchFirstTick =
            listenerCandidateTick;
        mod::Log(
            "REVIVAL_HOST_LISTENER_CANDIDATE family=%s port=%u "
            "serial=%u debounceMs=100",
            netplay::network::FamilyName(observation.family),
            static_cast<unsigned>(observation.port),
            observation.serial);
        return;
    }
    if (listenerCandidateTick
            - g_hostingOverlay.listenerMismatchFirstTick
        < 100u)
    {
        return;
    }

    if (!observation.portMatches)
    {
        g_hostingOverlay.listenerMismatch = true;
        mod::Log(
            "REVIVAL_HOST_LISTENER_MISMATCH observedFamily=%s observedPort=%u "
            "expectedFamily=%s expectedPort=%u familyMatches=%d portMatches=%d "
            "serial=%u action=cancel reason=port_mismatch",
            netplay::network::FamilyName(observation.family),
            static_cast<unsigned>(observation.port),
            netplay::network::FamilyName(
                g_hostingOverlay.effectiveFamily),
            static_cast<unsigned>(g_hostingOverlay.port),
            observation.familyMatches ? 1 : 0,
            observation.portMatches ? 1 : 0,
            observation.serial);
        netplay::bridge::async_host::Reset();
        netplay::bridge::CancelSession(
            "host_listener_family_or_port_mismatch");
        ShowHostStartupFailure(
            "Hosting started on an unexpected port. Please try again.",
            true);
        return;
    }

    g_hostingOverlay.listenerMismatch = false;
    g_hostingOverlay.listenerMismatchSerial = 0;
    g_hostingOverlay.listenerMismatchPort = 0;
    g_hostingOverlay.listenerMismatchFirstTick = 0;

    if (observation.expectedFamilyKnown
        && !observation.familyMatches)
    {
        g_hostingOverlay.listenerMismatch = true;
        mod::Log(
            "REVIVAL_HOST_LISTENER_MISMATCH observedFamily=%s "
            "observedPort=%u expectedFamily=%s expectedPort=%u "
            "serial=%u automaticRetryUsed=%d",
            netplay::network::FamilyName(observation.family),
            static_cast<unsigned>(observation.port),
            netplay::network::FamilyName(
                g_hostingOverlay.effectiveFamily),
            static_cast<unsigned>(g_hostingOverlay.port),
            observation.serial,
            g_hostingOverlay.automaticFamilyRetryAttempted
                ? 1
                : 0);
        if (BeginAutomaticHostFamilyRetry(
                observation.family,
                "listener_family_mismatch",
                observation.serial))
        {
            return;
        }

        netplay::bridge::async_host::Reset();
        netplay::bridge::CancelSession(
            "host_listener_family_mismatch_after_retry");
        ShowHostStartupFailure(
            "Hosting could not start with either connection type.",
            true);
        return;
    }

    std::string challengeEndpoint;
    if (g_pendingLobbyChallengePublish.active)
    {
        if (!g_lobbySession
            || !IsLobbyChallengeTargetStillIdle(
                g_pendingLobbyChallengePublish.targetPlayerId))
        {
            mod::Log(
                "LOBBY_ENDPOINT_PUBLISH result=cancelled "
                "reason=target_not_idle_after_listener_ack target='%s' "
                "id=%d listenerSerial=%u",
                g_pendingLobbyChallengePublish.targetName.c_str(),
                g_pendingLobbyChallengePublish.targetPlayerId,
                observation.serial);
            netplay::bridge::async_host::Reset();
            netplay::bridge::CancelSession(
                "lobby_target_gone_before_endpoint_publish");
            ShowHostStartupFailure(
                "That player is no longer available.",
                true);
            return;
        }

        netplay::network::NetworkEndpoint endpoint;
        endpoint.family = observation.family;
        endpoint.host =
            g_pendingLobbyChallengePublish.publicAddress;
        endpoint.port = observation.port;
        if (!netplay::network::FormatEndpoint(
                endpoint,
                &challengeEndpoint))
        {
            mod::Log(
                "LOBBY_ENDPOINT_PUBLISH result=cancelled "
                "reason=final_endpoint_invalid family=%s address='%s' "
                "port=%u listenerSerial=%u",
                netplay::network::FamilyName(observation.family),
                g_pendingLobbyChallengePublish.publicAddress.c_str(),
                static_cast<unsigned>(observation.port),
                observation.serial);
            netplay::bridge::async_host::Reset();
            netplay::bridge::CancelSession(
                "invalid_lobby_endpoint_after_listener_ack");
            ShowHostStartupFailure(
                "The public address could not be prepared for this connection.",
                true);
            return;
        }
    }

    if (g_hostingOverlay.usedFamilyFallback
        && !g_hostingOverlay.familyFallbackNoticeStarted)
    {
        g_hostingOverlay.familyFallbackNoticeStarted = true;
        g_hostingOverlay.familyFallbackNoticeStartTick =
            GetTickCount();
    }
    g_hostingOverlay.listenerReady = true;
    g_hostingOverlay.familyRetryAllowed = false;
    g_hostingOverlay.port = observation.port;
    if (netplay::bridge::async_host::IsActive())
    {
        (void)netplay::bridge::async_host::NotifyHostListenerReady(
            observation);
    }
    mod::Log(
        "REVIVAL_HOST_ACK family=%s port=%u serial=%u publicAddress='%s'",
        netplay::network::FamilyName(observation.family),
        static_cast<unsigned>(observation.port),
        observation.serial,
        g_hostingOverlay.publicIp[0] != '\0'
            ? g_hostingOverlay.publicIp
            : "(none)");

    if (g_pendingLobbyChallengePublish.active)
    {
        g_lobbySession->SendChallenge(
            g_pendingLobbyChallengePublish.targetPlayerId,
            g_pendingLobbyChallengePublish.targetName,
            challengeEndpoint);
        mod::Log(
            "LOBBY_ENDPOINT_PUBLISH result=sent_after_listener_ack target='%s' "
            "id=%d originalPreferred=%s family=%s fallback=%d "
            "automaticRetry=%d endpoint='%s' listenerSerial=%u",
            g_pendingLobbyChallengePublish.targetName.c_str(),
            g_pendingLobbyChallengePublish.targetPlayerId,
            netplay::network::FamilyName(
                g_pendingLobbyChallengePublish.preferredFamily),
            netplay::network::FamilyName(
                g_pendingLobbyChallengePublish.family),
            g_pendingLobbyChallengePublish.preferredFamily
                    != g_pendingLobbyChallengePublish.family
                ? 1
                : 0,
            g_pendingLobbyChallengePublish
                    .automaticFamilyRetryAttempted
                ? 1
                : 0,
            challengeEndpoint.c_str(),
            observation.serial);
        g_pendingLobbyChallengePublish = {};
    }
}

static void RestoreHostingOverlayFromAsyncHost()
{
    g_hostingOverlay = {};
    g_hostingOverlay.active = true;
    g_hostingOverlay.port =
        netplay::bridge::async_host::HostPort();
    g_hostingOverlay.sessionQueued = true;
    g_hostingOverlay.familyRetryAllowed = false;
    g_hostingOverlay.listenerReady =
        netplay::bridge::async_host::IsHostListenerReady();
    g_hostingOverlay.ipFetchDone = true;
    g_hostingOverlay.writeNicknameToIni = false;
    CopyBoundedText(
        g_hostingOverlay.hostNickname,
        sizeof(g_hostingOverlay.hostNickname),
        netplay::bridge::async_host::HostNickname());

    netplay::bridge::HostSessionNetworkConfig networkConfig;
    if (netplay::bridge::async_host::GetHostNetworkConfig(
            &networkConfig))
    {
        g_hostingOverlay.preferredFamily =
            networkConfig.preferredFamily;
        g_hostingOverlay.effectiveFamily =
            networkConfig.effectiveFamily;
        g_hostingOverlay.usedFamilyFallback =
            networkConfig.preferredFamily
            != networkConfig.effectiveFamily;
        g_hostingOverlay.automaticFamilyRetryAttempted =
            networkConfig.automaticFamilyRetryAttempted;
        g_hostingOverlay.ipFetchFailed =
            networkConfig.publicAddress.empty();
        CopyBoundedText(
            g_hostingOverlay.publicIp,
            sizeof(g_hostingOverlay.publicIp),
            networkConfig.publicAddress.c_str());

    }
    else
    {
        // Legacy Host entry points have no immutable discovery snapshot.
        // Preserve the listener and show an honest "public IP unavailable"
        // state instead of starting a new discovery that could select a
        // different family during the same hosting session.
        g_hostingOverlay.preferredFamily =
            g_netplayMenuState.hostFamily;
        g_hostingOverlay.effectiveFamily =
            g_netplayMenuState.hostFamily;
        g_hostingOverlay.ipFetchFailed = true;
    }

    mod::Log(
        "ASYNC_REHOST_NETWORK_SNAPSHOT restored=1 preferred=%s effective=%s "
        "fallback=%d automaticRetry=%d port=%u listenerReady=%d "
        "publicAddress='%s'",
        netplay::network::FamilyName(
            g_hostingOverlay.preferredFamily),
        netplay::network::FamilyName(
            g_hostingOverlay.effectiveFamily),
        g_hostingOverlay.usedFamilyFallback ? 1 : 0,
        g_hostingOverlay.automaticFamilyRetryAttempted
            ? 1
            : 0,
        static_cast<unsigned>(g_hostingOverlay.port),
        g_hostingOverlay.listenerReady ? 1 : 0,
        g_hostingOverlay.publicIp[0] != '\0'
            ? g_hostingOverlay.publicIp
            : "(none)");
}

static void LoadSerializedNetplayMenuSettingsFromIni()
{
    netplay::bridge::HostProtocolOverrideState protocolOverrideAtLoad;
    const bool hasSerializedIniRead =
        netplay::bridge::BeginOptionsIniAccess(
            false,
            &protocolOverrideAtLoad);
    LoadNetplayMenuSettingsFromIni();
    if (hasSerializedIniRead
        && protocolOverrideAtLoad.active
        && g_netplayMenuState.hostFamily
            != protocolOverrideAtLoad.originalFamily)
    {
        mod::Log(
            "NETPLAY_MENU_PROTOCOL_TRANSIENT_IGNORED loaded=%s "
            "effective=%s restoredPreference=%s",
            netplay::network::FamilyName(
                g_netplayMenuState.hostFamily),
            netplay::network::FamilyName(
                protocolOverrideAtLoad.effectiveFamily),
            netplay::network::FamilyName(
                protocolOverrideAtLoad.originalFamily));
        g_netplayMenuState.hostFamily =
            protocolOverrideAtLoad.originalFamily;
    }
    if (hasSerializedIniRead)
    {
        netplay::bridge::EndOptionsIniAccess();
    }
    else
    {
        mod::Log(
            "EnterNetplayMenu: serialized INI read access unavailable");
    }
}

// ---------------------------------------------------------------------------
// Joining overlay - shows "Connecting to IP:PORT ..." while connecting.
// Transitions to delay setup on success, or shows the error on failure.
// ---------------------------------------------------------------------------
void ResetJoiningOverlayState()
{
    g_joiningOverlay = {};
}

static std::string FormatNetworkEndpoint(
    netplay::network::NetworkFamily family,
    const char* host,
    uint16_t port)
{
    netplay::network::NetworkEndpoint endpoint;
    endpoint.family = family;
    endpoint.host = host != nullptr ? host : "";
    endpoint.port = port;
    std::string formatted;
    if (!netplay::network::FormatEndpoint(endpoint, &formatted))
    {
        return {};
    }
    return formatted;
}

void ActivateJoiningOverlay(const char* address, uint16_t port)
{
    ResetJoiningOverlayState();
    g_joiningOverlay.active = true;
    g_joiningOverlay.port = port;
    netplay::network::NetworkHost parsedHost;
    bool parsed =
        address != nullptr
        && netplay::network::ParseBareHost(address, &parsedHost);
    if (!parsed)
    {
        // A direct Join/Spectate address may be a hostname. StartSession
        // resolves it synchronously and snapshots the exact numeric endpoint
        // before returning, so use that immutable selection for display and
        // for a possible Spectate -> Join restart.
        const netplay::bridge::NetbridgeStatus status =
            netplay::bridge::GetStatus();
        parsed =
            status.port == port
            && netplay::network::ParseBareHost(
                status.address,
                &parsedHost);
    }
    if (parsed)
    {
        g_joiningOverlay.family = parsedHost.family;
    }
    const char* normalizedAddress =
        parsed ? parsedHost.host.c_str() : address;
    strncpy_s(
        g_joiningOverlay.address,
        sizeof(g_joiningOverlay.address),
        normalizedAddress != nullptr ? normalizedAddress : "",
        _TRUNCATE);
    g_joiningOverlay.address[sizeof(g_joiningOverlay.address) - 1] = '\0';
    const std::string endpoint = FormatNetworkEndpoint(
        g_joiningOverlay.family,
        g_joiningOverlay.address,
        port);
    mod::Log(
        "JoiningOverlay: activated target=%s family=%s",
        endpoint.empty() ? "(invalid)" : endpoint.c_str(),
        netplay::network::FamilyName(g_joiningOverlay.family));
}

void ActivateChallengeJoiningOverlay(const char* targetName, const char* address, uint16_t port)
{
    ResetJoiningOverlayState();
    g_joiningOverlay.active = true;
    g_joiningOverlay.port = port;
    g_joiningOverlay.displayTargetName = true;
    netplay::network::NetworkHost parsedHost;
    const bool parsed =
        address != nullptr
        && netplay::network::ParseBareHost(address, &parsedHost);
    if (parsed)
    {
        g_joiningOverlay.family = parsedHost.family;
    }
    const char* normalizedAddress = parsed ? parsedHost.host.c_str() : address;
    strncpy_s(
        g_joiningOverlay.address,
        sizeof(g_joiningOverlay.address),
        normalizedAddress != nullptr ? normalizedAddress : "",
        _TRUNCATE);
    g_joiningOverlay.address[sizeof(g_joiningOverlay.address) - 1] = '\0';
    strncpy_s(g_joiningOverlay.targetName, sizeof(g_joiningOverlay.targetName), targetName, _TRUNCATE);
    g_joiningOverlay.targetName[sizeof(g_joiningOverlay.targetName) - 1] = '\0';
    const std::string endpoint = FormatNetworkEndpoint(
        g_joiningOverlay.family,
        g_joiningOverlay.address,
        port);
    mod::Log(
        "JoiningOverlay: activated challenge target='%s' endpoint=%s family=%s",
        g_joiningOverlay.targetName,
        endpoint.empty() ? "(invalid)" : endpoint.c_str(),
        netplay::network::FamilyName(g_joiningOverlay.family));
}

bool TryStartWaitToSpectateFromJoinSettings(uint32_t screenContext, std::string* outErrorMessage)
{
    (void)screenContext;
    ClearPendingLobbySpectateWait("join_settings_wait_to_spectate");

    if (outErrorMessage != nullptr)
    {
        outErrorMessage->clear();
    }

    const auto& menuState = g_netplayMenuState;
    if (menuState.joinAddress.empty() || menuState.joinPort == 0)
    {
        if (outErrorMessage != nullptr)
        {
            *outErrorMessage = "Set the join address and port first.";
        }
        mod::Log("WaitToSpectate: missing join address/port");
        return false;
    }

    const bool started = netplay::bridge::StartSession(
        netplay::bridge::NetbridgeRole::Spectate,
        menuState.joinPort,
        menuState.joinAddress.c_str(),
        "");
    if (started)
    {
        ActivateJoiningOverlay(menuState.joinAddress.c_str(), menuState.joinPort);
        g_joiningOverlay.spectateMode = true;
        const std::string endpoint = FormatNetworkEndpoint(
            g_joiningOverlay.family,
            g_joiningOverlay.address,
            g_joiningOverlay.port);
        mod::Log(
            "WaitToSpectate: started spectate session endpoint=%s family=%s",
            endpoint.empty() ? "(invalid)" : endpoint.c_str(),
            netplay::network::FamilyName(g_joiningOverlay.family));
        return true;
    }

    const netplay::bridge::NetbridgeStatus status = netplay::bridge::GetStatus();
    if (outErrorMessage != nullptr)
    {
        *outErrorMessage =
            status.errorMsg[0] != '\0'
                ? status.errorMsg
                : "Unknown error";
    }
    mod::Log(
        "WaitToSpectate: StartSession failed -> %s",
        status.errorMsg[0] != '\0' ? status.errorMsg : "Unknown error");
    return false;
}

bool TryStartWaitToSpectateAtAddress(
    const char* address,
    uint16_t port,
    std::string* outErrorMessage)
{
    ClearPendingLobbySpectateWait("start_wait_to_spectate");

    if (outErrorMessage != nullptr)
    {
        outErrorMessage->clear();
    }

    if (address == nullptr || address[0] == '\0' || port == 0)
    {
        if (outErrorMessage != nullptr)
        {
            *outErrorMessage = "No active match host information is available for spectating yet.";
        }
        mod::Log("WaitToSpectate: missing address/port");
        return false;
    }

    const bool started = netplay::bridge::StartSession(
        netplay::bridge::NetbridgeRole::Spectate,
        port,
        address,
        "");
    if (started)
    {
        ActivateJoiningOverlay(address, port);
        g_joiningOverlay.spectateMode = true;
        const std::string endpoint = FormatNetworkEndpoint(
            g_joiningOverlay.family,
            g_joiningOverlay.address,
            g_joiningOverlay.port);
        mod::Log(
            "WaitToSpectate: started spectate session endpoint=%s family=%s",
            endpoint.empty() ? "(invalid)" : endpoint.c_str(),
            netplay::network::FamilyName(g_joiningOverlay.family));
        return true;
    }

    const netplay::bridge::NetbridgeStatus status = netplay::bridge::GetStatus();
    if (outErrorMessage != nullptr)
    {
        *outErrorMessage =
            status.errorMsg[0] != '\0'
                ? status.errorMsg
                : "Unknown error";
    }
    mod::Log(
        "WaitToSpectate: StartSession failed -> %s",
        status.errorMsg[0] != '\0' ? status.errorMsg : "Unknown error");
    return false;
}

namespace  // reopen anonymous namespace
{

bool IsHostNotYetPlayingSpectatePrompt()
{
    return g_spectateConfirmOverlay.promptKind
        == static_cast<int>(NetbridgeSpectatePromptKind::HostNotYetPlaying);
}

void ActivateSpectateConfirmOverlay(int promptKind)
{
    ResetSpectateConfirmOverlayState();
    g_spectateConfirmOverlay.active = true;
    g_spectateConfirmOverlay.promptKind = promptKind;
    if (promptKind == static_cast<int>(NetbridgeSpectatePromptKind::HostNotYetPlaying))
    {
        g_spectateConfirmOverlay.optionCount = 3;
        g_spectateConfirmOverlay.selectedOption = 1; // default to Wait
    }
    else
    {
        g_spectateConfirmOverlay.optionCount = 2;
        g_spectateConfirmOverlay.selectedOption = 0; // default to Yes
    }
    mod::Log(
        "SpectateConfirmOverlay: activated promptKind=%d optionCount=%d defaultSelection=%d",
        g_spectateConfirmOverlay.promptKind,
        g_spectateConfirmOverlay.optionCount,
        g_spectateConfirmOverlay.selectedOption);
}

void CancelPendingSpectateConfirmSession(const char* reasonTag)
{
    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
    ResetHostingOverlayState();
    ResetJoiningOverlayState();
    ClearPendingLobbySpectateWait(reasonTag);
    netplay::bridge::CancelSession("user_cancel");
    mod::Log(
        "SpectateConfirmOverlay: canceled pending spectate reason='%s' -> SessionBridge user_cancel",
        reasonTag != nullptr ? reasonTag : "");
}

bool RestartPendingSpectateSessionAsJoin(uint32_t screenContext)
{
    (void)screenContext;
    const std::string address = g_joiningOverlay.address;
    const uint16_t port = g_joiningOverlay.port;
    const netplay::network::NetworkFamily family = g_joiningOverlay.family;
    const bool displayTargetName = g_joiningOverlay.displayTargetName;
    const std::string targetName = g_joiningOverlay.targetName;
    const std::string formattedEndpoint =
        FormatNetworkEndpoint(family, address.c_str(), port);

    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
    ResetHostingOverlayState();
    ResetJoiningOverlayState();
    ClearPendingLobbySpectateWait("spectate_prompt_join");
    netplay::bridge::CancelSession("user_cancel");

    const bool writeNicknameToIni = ShouldWriteNicknameToRevivalIniForSessionStart();
    const bool started = netplay::bridge::StartSession(
        NetbridgeRole::Join,
        port,
        address.c_str(),
        g_netplayMenuState.nickname.c_str(),
        writeNicknameToIni);
    if (started)
    {
        ActivateJoiningOverlay(address.c_str(), port);
        if (displayTargetName && !targetName.empty())
        {
            g_joiningOverlay.displayTargetName = true;
            strncpy_s(g_joiningOverlay.targetName, sizeof(g_joiningOverlay.targetName), targetName.c_str(), _TRUNCATE);
            g_joiningOverlay.targetName[sizeof(g_joiningOverlay.targetName) - 1] = '\0';
        }
        mod::Log(
            "SpectateConfirmOverlay: restarting pending spectate as Join endpoint=%s family=%s started=1",
            formattedEndpoint.empty() ? "(invalid)" : formattedEndpoint.c_str(),
            netplay::network::FamilyName(family));
        return true;
    }

    const auto status = netplay::bridge::GetStatus();
    char text[256] = {};
    std::snprintf(
        text,
        sizeof(text),
        "Join start failed.\n\n%s",
        status.errorMsg[0] != '\0' ? status.errorMsg : "Unknown error");
    SetNetplayStatusMessage(text);
    mod::Log(
        "SpectateConfirmOverlay: restarting pending spectate as Join failed endpoint=%s family=%s error='%s'",
        formattedEndpoint.empty() ? "(invalid)" : formattedEndpoint.c_str(),
        netplay::network::FamilyName(family),
        status.errorMsg[0] != '\0' ? status.errorMsg : "Unknown error");
    return false;
}

bool HandleSpectateConfirmOverlayInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter)
{
    if (!g_spectateConfirmOverlay.active || inputBytes == nullptr || inactivityCounter == nullptr)
    {
        return false;
    }

    bool cancelRequested = ConsumeNetplayEscapeEdge();
    bool confirmRequested = false;

    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch = reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1 + playerIndex);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);

        int step = 0;
        if (vertical > 0)
        {
            step = 1;
        }
        else if (vertical < 0)
        {
            step = -1;
        }

        if (step != 0)
        {
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                const int oldOption = g_spectateConfirmOverlay.selectedOption;
                int newOption = oldOption + step;
                const int optionCount = (g_spectateConfirmOverlay.optionCount > 0)
                    ? g_spectateConfirmOverlay.optionCount
                    : 2;
                if (newOption < 0) newOption = optionCount - 1;
                if (newOption >= optionCount) newOption = 0;
                if (newOption != oldOption)
                {
                    g_spectateConfirmOverlay.selectedOption = newOption;
                    PlayUiSound(screenContext, kSfxMove);
                }
                *inputLatch = 1;
            }
        }
        else
        {
            *inputLatch = 0;
        }

        if (inputBytes[playerIndex + 16] == 1)
        {
            confirmRequested = true;
        }
        if (inputBytes[playerIndex + 18] == 1)
        {
            cancelRequested = true;
        }
    }

    ++(*inactivityCounter);

    if (cancelRequested)
    {
        const int promptKind = g_spectateConfirmOverlay.promptKind;
        PlayUiSound(screenContext, kSfxConfirm);
        CancelPendingSpectateConfirmSession("spectate_declined_escape");
        mod::Log(
            "SpectateConfirmOverlay: canceled via escape promptKind=%d",
            promptKind);
        return true;
    }

    if (confirmRequested)
    {
        PlayUiSound(screenContext, kSfxConfirm);

        if (IsHostNotYetPlayingSpectatePrompt())
        {
            const int selection = g_spectateConfirmOverlay.selectedOption;
            if (selection == 0)
            {
                (void)RestartPendingSpectateSessionAsJoin(screenContext);
                return true;
            }
            if (selection == 2)
            {
                CancelPendingSpectateConfirmSession("spectate_host_not_playing_cancel");
                mod::Log("SpectateConfirmOverlay: canceled via menu choice=Cancel");
                return true;
            }

            const int choice = 3; // Wait as spectator for the game to begin.
            const bool answered = netplay::bridge::AnswerSpectatePromptChoice(choice);
            if (!answered)
            {
                const auto status = netplay::bridge::GetStatus();
                if (status.errorMsg[0] != '\0')
                {
                    std::snprintf(
                        g_spectateConfirmOverlay.errorMessage,
                        sizeof(g_spectateConfirmOverlay.errorMessage),
                        "%s",
                        status.errorMsg);
                }
                else
                {
                    std::snprintf(
                        g_spectateConfirmOverlay.errorMessage,
                        sizeof(g_spectateConfirmOverlay.errorMessage),
                        "Failed to send answer");
                }
                mod::Log("SpectateConfirmOverlay: answer failed choice=%d promptKind=%d", choice, g_spectateConfirmOverlay.promptKind);
                return true;
            }

            mod::Log("SpectateConfirmOverlay: answered choice=%d promptKind=%d", choice, g_spectateConfirmOverlay.promptKind);
            ResetSpectateConfirmOverlayState();
            return true;
        }

        const int choice = (g_spectateConfirmOverlay.selectedOption == 0) ? 1 : 2;

        const bool answered = netplay::bridge::AnswerSpectatePromptChoice(choice);
        if (!answered)
        {
            const auto status = netplay::bridge::GetStatus();
            if (status.errorMsg[0] != '\0')
            {
                std::snprintf(
                    g_spectateConfirmOverlay.errorMessage,
                    sizeof(g_spectateConfirmOverlay.errorMessage),
                    "%s",
                    status.errorMsg);
            }
            else
            {
                std::snprintf(
                    g_spectateConfirmOverlay.errorMessage,
                    sizeof(g_spectateConfirmOverlay.errorMessage),
                    "Failed to send answer");
            }
            mod::Log("SpectateConfirmOverlay: answer failed choice=%d", choice);
            return true;
        }

        mod::Log("SpectateConfirmOverlay: answered choice=%d", choice);
        ResetSpectateConfirmOverlayState();

        if (choice == 2)
        {
            CancelPendingSpectateConfirmSession("spectate_declined_confirm_no");
        }
        // If accepted, the session continues - Revival will proceed to delay
        // setup or straight to game. The normal delay/connected flow handles it.
        return true;
    }

    return true;
}

int ClampDelaySelection(int value)
{
    if (value < kDelaySelectionMin)
    {
        return kDelaySelectionMin;
    }
    if (value > kDelaySelectionMax)
    {
        return kDelaySelectionMax;
    }
    return value;
}

// Mirrors Revival's CalculateDelayDelta.
int CalculateDelayDeltaLikeRevival(float rttMs, float currentDelayFrames)
{
    if (rttMs < 0.0f)
    {
        rttMs = 0.0f;
    }

    const double frameTimeMs = 1000.0 / 64.0;
    const double roundTripInFrames = static_cast<double>(rttMs) / (frameTimeMs * 2.0);
    const double framesNeeded = std::ceil(roundTripInFrames);
    const double delta = framesNeeded - static_cast<double>(currentDelayFrames);
    const double clamped = (delta > 0.0) ? delta : 0.0;
    return static_cast<int>(clamped);
}

// Mirrors Revival's CalculateRecommendedDelay.
int CalculateRecommendedDelayLikeRevival(int delayFloor, float rttMs, float currentDelayFrames)
{
    if (rttMs < 0.0f)
    {
        rttMs = 0.0f;
    }

    const float minDelay = (std::min)(3.0f, currentDelayFrames);
    const double roundTripMs = (1000.0 / 64.0) * 2.0;
    const double rttInFrames = static_cast<double>(rttMs) / roundTripMs;
    const double framesNeeded = std::ceil(rttInFrames);
    const double rawDelta = std::ceil(framesNeeded - static_cast<double>(minDelay));
    const int delta = static_cast<int>(rawDelta);
    int best = (std::max)(delayFloor, delta);
    if (currentDelayFrames == minDelay)
    {
        ++best;
    }
    return best;
}

void ActivateDelaySetupOverlay(const netplay::bridge::NetbridgeStatus& bridgeStatus)
{
    ResetHostingOverlayState();  // dismiss hosting panel before showing delay setup
    ResetJoiningOverlayState();   // dismiss joining panel before showing delay setup
    ResetDelaySetupOverlayState();
    g_delaySetupOverlay.active = true;
    g_delaySetupOverlay.waitingForRuntimeReady = false;
    g_delaySetupOverlay.maxDelay = kDelaySelectionMax;
    g_delaySetupOverlay.pingMs = bridgeStatus.pingMs;

    const int currentDelay = ClampDelaySelection((bridgeStatus.rollbackFrames >= 0) ? bridgeStatus.rollbackFrames : 0);
    g_delaySetupOverlay.currentDelay = currentDelay;

    const netplay::bridge::DelayPromptMetrics promptMetrics = netplay::bridge::GetDelayPromptMetrics();
    int minDelay = 0;
    int recommendedDelay = currentDelay;
    int maxDelay = kDelaySelectionMax;
    if (promptMetrics.serial > 0)
    {
        if (promptMetrics.averagePingMs >= 0)
        {
            g_delaySetupOverlay.pingMs = promptMetrics.averagePingMs;
        }
        if (promptMetrics.minDelay >= kDelaySelectionMin && promptMetrics.minDelay <= kDelaySelectionMax)
        {
            minDelay = promptMetrics.minDelay;
        }
        if (promptMetrics.maxDelay >= kDelaySelectionMin && promptMetrics.maxDelay <= kDelaySelectionMax)
        {
            maxDelay = promptMetrics.maxDelay;
        }
        if (promptMetrics.recommendedDelay >= kDelaySelectionMin && promptMetrics.recommendedDelay <= kDelaySelectionMax)
        {
            recommendedDelay = promptMetrics.recommendedDelay;
        }
    }
    if (bridgeStatus.pingMs >= 0)
    {
        const int fallbackMin = CalculateDelayDeltaLikeRevival(static_cast<float>(bridgeStatus.pingMs), static_cast<float>(currentDelay));
        const int fallbackRecommended =
            CalculateRecommendedDelayLikeRevival(fallbackMin, static_cast<float>(bridgeStatus.pingMs), static_cast<float>(currentDelay));
        if (promptMetrics.serial <= 0)
        {
            minDelay = fallbackMin;
            recommendedDelay = fallbackRecommended;
        }
    }

    if (maxDelay < minDelay)
    {
        maxDelay = minDelay;
    }

    g_delaySetupOverlay.minDelay = ClampDelaySelection(minDelay);
    g_delaySetupOverlay.maxDelay = ClampDelaySelection(maxDelay);
    g_delaySetupOverlay.recommendedDelay =
        (std::min)(g_delaySetupOverlay.maxDelay, ClampDelaySelection((std::max)(recommendedDelay, g_delaySetupOverlay.minDelay)));
    g_delaySetupOverlay.selectedDelay = g_delaySetupOverlay.recommendedDelay;

    // Guard against a spiked ping measurement. Async hosting measures RTT while
    // the delay prompt is held during local gameplay - where EFZ is not
    // servicing the netplay connection - so the helper's ping can inflate to
    // absurd values (e.g. ~2000ms) and pin the recommendation to max delay. The
    // ping is measured once and never re-pings, so the only sane workaround is to
    // distrust it: when the ping is implausibly high, default the selection (and
    // shown recommendation) to a reasonable value instead of dumping the user at
    // max. The full range stays available for manual adjustment, and Revival
    // allows further tuning the delay live during the match.
    constexpr int kImplausiblePingMs = 500;
    constexpr int kSpikeFallbackDelay = 3;
    if (g_delaySetupOverlay.pingMs > kImplausiblePingMs)
    {
        const int fallback = (std::min)(
            g_delaySetupOverlay.maxDelay,
            (std::max)(g_delaySetupOverlay.minDelay, kSpikeFallbackDelay));
        mod::Log(
            "DelayOverlay: implausible ping=%d (held-prompt spike) -> default delay %d (was recommended=%d)",
            g_delaySetupOverlay.pingMs, fallback, g_delaySetupOverlay.recommendedDelay);
        g_delaySetupOverlay.recommendedDelay = fallback;
        g_delaySetupOverlay.selectedDelay = fallback;
    }

    if (bridgeStatus.p1Name[0] != '\0' && bridgeStatus.p2Name[0] != '\0')
    {
        CopyBoundedText(g_delaySetupOverlay.p1Name, sizeof(g_delaySetupOverlay.p1Name), bridgeStatus.p1Name);
        CopyBoundedText(g_delaySetupOverlay.p2Name, sizeof(g_delaySetupOverlay.p2Name), bridgeStatus.p2Name);
    }
    else
    {
        g_delaySetupOverlay.p1Name[0] = '\0';
        g_delaySetupOverlay.p2Name[0] = '\0';
    }

    mod::Log(
        "DelayOverlay: activated promptSerial=%d servedSerial=%d ping=%d current=%d min=%d max=%d recommended=%d names='%s' vs '%s'",
        promptMetrics.serial,
        bridgeStatus.delayPromptServedSerial,
        g_delaySetupOverlay.pingMs,
        g_delaySetupOverlay.currentDelay,
        g_delaySetupOverlay.minDelay,
        g_delaySetupOverlay.maxDelay,
        g_delaySetupOverlay.recommendedDelay,
        g_delaySetupOverlay.p1Name,
        g_delaySetupOverlay.p2Name);
}

bool HandleDelaySetupOverlayInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter)
{
    if (!g_delaySetupOverlay.active || inputBytes == nullptr || inactivityCounter == nullptr)
    {
        return false;
    }

    if (g_delaySetupOverlay.waitingForRuntimeReady)
    {
        const auto statusWhileWaiting = netplay::bridge::GetStatus();
        const DWORD nowTick = GetTickCount();
        if (statusWhileWaiting.localInitApplied != 0
            && nowTick >= g_delaySetupOverlay.nextHandoffRetryTick
            && (!g_delaySetupOverlay.vsHumanSyncArmed || statusWhileWaiting.roleFlag != 2))
        {
            const bool wasArmed = g_delaySetupOverlay.vsHumanSyncArmed;
            const bool prepared = netplay::bridge::PrepareVsHumanHandoff();
            if (prepared)
            {
                g_delaySetupOverlay.vsHumanSyncArmed = true;
                if (!wasArmed)
                {
                    mod::Log(
                        "DelayOverlay: handoff armed localInit=%d role=%d phase=%s",
                        statusWhileWaiting.localInitApplied,
                        statusWhileWaiting.roleFlag,
                        netplay::bridge::PhaseToString(static_cast<NetbridgePhase>(statusWhileWaiting.phase)));
                }
            }
            g_delaySetupOverlay.nextHandoffRetryTick = nowTick + 1000;
        }

        bool cancelRequested = ConsumeNetplayEscapeEdge();
        for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
        {
            if (inputBytes[playerIndex + 18] == 1)
            {
                cancelRequested = true;
                break;
            }
        }
        if (cancelRequested)
        {
            PlayUiSound(screenContext, kSfxConfirm);
            ResetDelaySetupOverlayState();
            ResetSpectateConfirmOverlayState();
            ResetHostingOverlayState();
            ResetJoiningOverlayState();
            netplay::bridge::CancelSession("user_cancel");
            if (g_lobbySession)
            {
                g_lobbySession->NotifyEndMatch();
            }
            mod::Log("DelayOverlay: canceled while waiting for runtime sync");
            return true;
        }

        ++(*inactivityCounter);
        return true;
    }

    bool cancelRequested = ConsumeNetplayEscapeEdge();
    bool confirmRequested = false;
    int confirmPlayer = -1;
    bool moved = false;

    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch = reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1 + playerIndex);
        const int8_t horizontal = static_cast<int8_t>(inputBytes[playerIndex + 12]);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);

        int step = 0;
        if (horizontal > 0 || vertical > 0)
        {
            step = 1;
        }
        else if (horizontal < 0 || vertical < 0)
        {
            step = -1;
        }

        if (step != 0)
        {
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                const int oldValue = g_delaySetupOverlay.selectedDelay;
                const int nextValue = oldValue + step;
                g_delaySetupOverlay.selectedDelay =
                    (std::max)(g_delaySetupOverlay.minDelay, (std::min)(g_delaySetupOverlay.maxDelay, nextValue));
                if (g_delaySetupOverlay.selectedDelay != oldValue)
                {
                    g_delaySetupOverlay.errorMessage[0] = '\0';
                    PlayUiSound(screenContext, kSfxMove);
                    moved = true;
                    mod::Log(
                        "DelayOverlay: selection player=%d from=%d to=%d step=%d",
                        playerIndex,
                        oldValue,
                        g_delaySetupOverlay.selectedDelay,
                        step);
                }
                *inputLatch = 1;
            }
        }
        else
        {
            *inputLatch = 0;
        }

        if (inputBytes[playerIndex + 16] == 1)
        {
            confirmRequested = true;
            confirmPlayer = playerIndex;
        }
        if (inputBytes[playerIndex + 18] == 1)
        {
            cancelRequested = true;
        }
    }

    if (!moved)
    {
        ++(*inactivityCounter);
    }

    if (cancelRequested)
    {
        PlayUiSound(screenContext, kSfxConfirm);
        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        ResetHostingOverlayState();
        ResetJoiningOverlayState();
        netplay::bridge::CancelSession("user_cancel");
        if (g_lobbySession)
        {
            g_lobbySession->NotifyEndMatch();
        }
        mod::Log("DelayOverlay: canceled");
        return true;
    }

    if (confirmRequested)
    {
        const int selectedDelay = g_delaySetupOverlay.selectedDelay;
        const bool applied = netplay::bridge::ApplyInputDelay(selectedDelay);
        if (!applied)
        {
            PlayUiSound(screenContext, kSfxMove);
            const auto status = netplay::bridge::GetStatus();
            if (status.errorMsg[0] != '\0')
            {
                std::snprintf(
                    g_delaySetupOverlay.errorMessage,
                    sizeof(g_delaySetupOverlay.errorMessage),
                    "Delay apply failed: %s",
                    status.errorMsg);
            }
            else
            {
                std::snprintf(
                    g_delaySetupOverlay.errorMessage,
                    sizeof(g_delaySetupOverlay.errorMessage),
                    "Delay apply failed");
            }
            mod::Log("DelayOverlay: confirm failed selected=%d", selectedDelay);
            return true;
        }

        PlayUiSound(screenContext, kSfxConfirm);
        const auto statusAfterApply = netplay::bridge::GetStatus();
        const auto phaseAfterApply = static_cast<NetbridgePhase>(statusAfterApply.phase);
        const bool strictNativeSyncAfterApply =
            netplay::bridge::RequiresNativeVsHumanSyncForHandoff();
        mod::Log(
            "DelayOverlay: confirmed player=%d selected=%d recommended=%d min=%d max=%d ping=%d phase=%s syncReady=%d strictNativeSync=%d",
            confirmPlayer,
            selectedDelay,
            g_delaySetupOverlay.recommendedDelay,
            g_delaySetupOverlay.minDelay,
            g_delaySetupOverlay.maxDelay,
            g_delaySetupOverlay.pingMs,
            netplay::bridge::PhaseToString(phaseAfterApply),
            statusAfterApply.vsHumanSyncReady,
            strictNativeSyncAfterApply ? 1 : 0);

        const bool readyForHandoff =
            statusAfterApply.vsHumanSyncReady != 0
            || (phaseAfterApply == NetbridgePhase::Connected && !strictNativeSyncAfterApply);
        g_delaySetupOverlay.waitingForRuntimeReady = true;
        g_delaySetupOverlay.vsHumanSyncArmed = false;
        g_delaySetupOverlay.connectedHandoffDelayActive = false;
        g_delaySetupOverlay.connectedHandoffDelayFramesRemaining = 0;
        g_delaySetupOverlay.nextHandoffRetryTick = 0;
        g_delaySetupOverlay.errorMessage[0] = '\0';
        ResetVsHumanHandoffWarmup("delay_confirmed");
        mod::Log(
            "DelayOverlay: waiting for runtime sync/title warmup after delay selection phase=%s ready=%d prompt=%d/%d",
            netplay::bridge::PhaseToString(phaseAfterApply),
            readyForHandoff ? 1 : 0,
            statusAfterApply.delayPromptSerial,
            statusAfterApply.delayPromptServedSerial);
        if (readyForHandoff)
        {
            (void)AdvanceVsHumanHandoffWarmup(
                screenContext,
                statusAfterApply,
                phaseAfterApply,
                "delay_confirm_ready",
                inactivityCounter);
        }
        return true;
    }

    return true;
}

void PrepareVsHumanGameState(uint32_t screenContext)
{
    const int gameSystem = GetGameSystem(screenContext);
    if (gameSystem == 0)
    {
        mod::Log("PrepareVsHumanGameState: skipped (gameSystem=null)");
        return;
    }

    auto* const state = reinterpret_cast<uint8_t*>(gameSystem);
    const uint8_t roundsSetting = state[kGameSystemOffsetRoundsSetting];

    // Mirror the native title "VS Human" branch:
    // 4931/4932 = human-vs-human, 4964 = mode 4, 4942 = configured round count.
    // Also set 4965 and 82563 which ConfigureModeFlags(0) would set via the
    // nav hooks at 0x763F04/0x763E50.  We bypass those hooks by forcing
    // g_pendingGlobalStateTransition=1, so the flags must be set here.
    state[kGameSystemOffsetCpuFlagP1] = 0;
    state[kGameSystemOffsetCpuFlagP2] = 0;
    state[kGameSystemOffsetMode] = kGameModeVsHuman;
    state[kGameSystemOffsetSecondaryModeFlag] = kSecondaryModeFlagVsHuman;
    state[kGameSystemOffsetRoundsCurrent] = roundsSetting;
    state[kGameSystemOffsetReplaySessionFlag] = kReplaySessionFlagCleared;

    // Reset match state so the next match starts fresh.  These fields are
    // normally cleared by initializeCharacterSelectScreen (0x7597D0) but
    // persist if a previous online match left them dirty.
    *reinterpret_cast<uint32_t*>(gameSystem + kGameSystemOffsetP1WinState) = 0;
    *reinterpret_cast<uint32_t*>(gameSystem + kGameSystemOffsetP2WinState) = 0;
    state[kGameSystemOffsetMatchCounter] = 0;
    state[kGameSystemOffsetContinueFlag] = 0;
    state[kGameSystemOffsetStageSelection] = 0;
    *reinterpret_cast<uint32_t*>(gameSystem + kGameSystemOffsetStageAnimState) = 0;
    *reinterpret_cast<uint16_t*>(gameSystem + kGameSystemOffsetStageAnim) = 0;
    state[kGameSystemOffsetStageCursor] = 0;

    // Force the charselect screen object to re-initialise next time it
    // runs (reset cursor positions, cameras, selection state, unlock
    // flags, etc.).  The screen table at 0x790110 holds pointers to
    // each screen object; index 1 is charselect.  Setting byte +44
    // (init required flag) to 1 causes updateCharacterSelectScreen to
    // call initializeCharacterSelectScreen on the next frame it runs.
    //
    // The full grid/palette/timer reset is guarded by g_charSelectResetPending
    // so it only fires on the initial menu→charselect transition and NOT on
    // subsequent handoffs within the same netplay session (e.g. rematches).
    __try
    {
        const uintptr_t tableAddr = RuntimeAddress(kVaScreenObjectTable);
        auto* const screenTable = reinterpret_cast<uint32_t*>(tableAddr);
        const uint32_t charSelectObj = screenTable[1];
        if (charSelectObj != 0)
        {
            // Read the current exit flag BEFORE we clear it - diagnostic.
            const uint8_t staleExitFlag =
                *reinterpret_cast<const uint8_t*>(charSelectObj + kOffsetScreenExitState);

            // Trigger the per-entry reinit (resets cursor pixels, camera,
            // colors, timers, grid states, input latches).
            *reinterpret_cast<uint8_t*>(charSelectObj + kOffsetScreenInitState) = 1;

            // CRITICAL: clear the exit flag. initializeCharacterSelectScreen
            // does NOT clear byte[45]; if it's stale from a previous session
            // the charselect update would immediately take the exit path.
            *reinterpret_cast<uint8_t*>(charSelectObj + kOffsetScreenExitState) = 0;

            if (staleExitFlag != 0)
            {
                mod::Log(
                    "PrepareVsHumanGameState: CLEARED stale exit flag! "
                    "charselect byte[45] was %u (obj=0x%08lX)",
                    static_cast<unsigned>(staleExitFlag),
                    static_cast<unsigned long>(charSelectObj));
            }

            if (g_charSelectResetPending)
            {
                g_charSelectResetPending = false;

                // The reinit does NOT reset grid col/row - those are only set
                // by the constructor.  Explicitly reset them to the constructor
                // defaults so both players start at a known position every time.
                *reinterpret_cast<uint8_t*>(charSelectObj + kOffsetCharSelectP1GridCol) = kCharSelectDefaultP1Col;
                *reinterpret_cast<uint8_t*>(charSelectObj + kOffsetCharSelectP2GridCol) = kCharSelectDefaultP2Col;
                *reinterpret_cast<uint8_t*>(charSelectObj + kOffsetCharSelectP1GridRow) = kCharSelectDefaultP1Row;
                *reinterpret_cast<uint8_t*>(charSelectObj + kOffsetCharSelectP2GridRow) = kCharSelectDefaultP2Row;

                // Derive character IDs from the grid map so they match the
                // reset col/row positions (avoids one frame of stale charId).
                const auto* gridMap = reinterpret_cast<const uint8_t*>(
                    charSelectObj + kOffsetCharSelectGridMap);
                *reinterpret_cast<uint8_t*>(charSelectObj + kOffsetCharSelectP1CharId) =
                    gridMap[kCharSelectDefaultP1Row * 3 + kCharSelectDefaultP1Col];
                *reinterpret_cast<uint8_t*>(charSelectObj + kOffsetCharSelectP2CharId) =
                    gridMap[kCharSelectDefaultP2Row * 3 + kCharSelectDefaultP2Col];

                // Reset palette/color selection and selection timers to 0.
                *reinterpret_cast<uint8_t*>(charSelectObj + kOffsetCharSelectP1Color) = 0;
                *reinterpret_cast<uint8_t*>(charSelectObj + kOffsetCharSelectP2Color) = 0;
                *reinterpret_cast<uint16_t*>(charSelectObj + kOffsetCharSelectP1Timer) = 0;
                *reinterpret_cast<uint16_t*>(charSelectObj + kOffsetCharSelectP2Timer) = 0;

                mod::Log("PrepareVsHumanGameState: charselect full reset "
                    "grid p1(%u,%u) p2(%u,%u) color=0/0 (obj=0x%08lX)",
                    kCharSelectDefaultP1Col, kCharSelectDefaultP1Row,
                    kCharSelectDefaultP2Col, kCharSelectDefaultP2Row,
                    static_cast<unsigned long>(charSelectObj));
            }
            else
            {
                mod::Log("PrepareVsHumanGameState: charselect initFlag=1, "
                    "grid/palette reset skipped (mid-session) obj=0x%08lX",
                    static_cast<unsigned long>(charSelectObj));
            }
        }
        else
        {
            mod::Log("PrepareVsHumanGameState: charselect screen object is null");
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        mod::Log("PrepareVsHumanGameState: SEH exception reading charselect screen object");
    }

    mod::Log(
        "PrepareVsHumanGameState: mode=%u secondaryMode=%u replaySession=%u rounds=%u cpuFlags=%u/%u "
        "wins=%u/%u match=%u continue=%u stage=%u gameStageCursor=%u gameStageAnim=%u",
        static_cast<unsigned>(state[kGameSystemOffsetMode]),
        static_cast<unsigned>(state[kGameSystemOffsetSecondaryModeFlag]),
        static_cast<unsigned>(state[kGameSystemOffsetReplaySessionFlag]),
        static_cast<unsigned>(state[kGameSystemOffsetRoundsCurrent]),
        static_cast<unsigned>(state[kGameSystemOffsetCpuFlagP1]),
        static_cast<unsigned>(state[kGameSystemOffsetCpuFlagP2]),
        *reinterpret_cast<uint32_t*>(gameSystem + kGameSystemOffsetP1WinState),
        *reinterpret_cast<uint32_t*>(gameSystem + kGameSystemOffsetP2WinState),
        static_cast<unsigned>(state[kGameSystemOffsetMatchCounter]),
        static_cast<unsigned>(state[kGameSystemOffsetContinueFlag]),
        static_cast<unsigned>(state[kGameSystemOffsetStageSelection]),
        static_cast<unsigned>(state[kGameSystemOffsetStageCursor]),
        static_cast<unsigned>(*reinterpret_cast<uint16_t*>(gameSystem + kGameSystemOffsetStageAnim)));
}

// ---------------------------------------------------------------------------
// PrepareSpectateReplayState - set the title screen's menu selection to
// "Replay" (4) so the Revival DLL's mode-transition detector recognises the
// spectate context.  The DLL checks EFZ_Mode0_ReadFlag1084() == 4 which is
// byte 1084 (0x43C) of the mode-0 screen object - the menu selection field.
// ---------------------------------------------------------------------------
void PrepareSpectateReplayState(uint32_t screenContext)
{
    auto* const menuSelection = reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection);
    const int oldSelection = static_cast<int>(*menuSelection);
    *menuSelection = kMenuSelectionReplay;
    const int newSelection = static_cast<int>(*menuSelection);

    // Cross-check: read the current screen index from the EXE game mode table
    // to confirm we're on the title screen (mode 0) where this write matters.
    int currentScreenIdx = -1;
    {
        auto* const screenIdxPtr = reinterpret_cast<const int*>(kVaCurrentScreenIndex);
        currentScreenIdx = *screenIdxPtr;
    }

    mod::Log(
        "PrepareSpectateReplayState: screenContext=0x%08lX offset=0x%03X "
        "menuSelection %d -> %d (Replay) screen=%d",
        static_cast<unsigned long>(screenContext),
        static_cast<unsigned>(kOffsetMenuSelection),
        oldSelection, newSelection, currentScreenIdx);
}
} // namespace

bool ShutdownLobbySessionForProcessExit(bool emergency, const char* reason)
{
    return ShutdownLobbySessionForProcessExitImpl(emergency, reason);
}

// ---------------------------------------------------------------------------
// HandoffSpectateSession - transition directly to Character Select (mode 1)
// for spectating.  The DLL creates a client session (type 1) for spectating
// which uses shared-memory IPC and input replay.  The client session
// survives all mode transitions (no watcher is created - the DLL's watcher
// creation path requires session type 2 which the mod never uses).  Going
// directly to charselect allows the client session's input replay to drive
// the character selection from the host's captured inputs.
// ---------------------------------------------------------------------------
static bool SuspendUiHooksForOnlineSimulation(
    uint32_t screenContext,
    const char* reason)
{
    // Request one final pre-handoff control-plane snapshot and complete any
    // temporary Host Protocol acknowledgement while still safely in the menu.
    // The barrier drains that accepted request, then proves that no exporter
    // is reading live game memory when online simulation begins. The snapshot
    // intentionally describes the last menu state, not a live battle feed.
    netplay::bridge::TickExportOnly(true);
    if (!netplay::bridge::state_export::SuspendForOnlineSimulation())
    {
        mod::Log(
            "ONLINE_SIMULATION_HANDOFF_BLOCKED reason=%s component=state_export",
            reason != nullptr ? reason : "unknown");
        netplay::bridge::state_export::ResumeControlPlaneUpdates();
        return false;
    }

    // Remove UI/render/update interpositions and verify each teardown. The
    // WndProc remover handles both legal DebugWndProc/NetplayWindowProc chain
    // orders without overwriting the live top-level proc.
    const bool windowRemoved = RemoveNetplayWindowHook();
    const bool imguiOk =
        netplay::debug_overlay::SuspendForOnlineSimulation();
    const bool renderOk = netplay::battle_log::ShutdownRenderOverlay();
    const bool frontendOk =
        netplay::bridge::frontend_return::SuspendUpdateHooksForOnlineSimulation();
    bool windowOk = windowRemoved;
#if !defined(EFZ_NATIVE_TICK_PASSTHROUGH)
    if (windowRemoved && imguiOk && renderOk && frontendOk)
    {
        // Shipping builds retain only the lightweight NetplayWindowProc close
        // path so WM_CLOSE can broadcast MessageQuit before process teardown.
        // With menu state inactive it otherwise immediately forwards to EFZ's
        // stock proc. The diagnostic native-tick arm leaves no WndProc detour.
        windowOk = InstallNetplayWindowHook(screenContext);
    }
#endif
    if (windowOk && imguiOk && renderOk && frontendOk)
    {
        return true;
    }

    mod::Log(
        "ONLINE_SIMULATION_HANDOFF_BLOCKED reason=%s window=%d imgui=%d endScene=%d screenUpdates=%d",
        reason != nullptr ? reason : "unknown",
        windowOk ? 1 : 0,
        imguiOk ? 1 : 0,
        renderOk ? 1 : 0,
        frontendOk ? 1 : 0);

    // The transition has not begun, so restore menu/control-plane ownership
    // and let a later frame retry the handoff after the failed component is
    // recoverable.
    netplay::bridge::state_export::ResumeControlPlaneUpdates();
    InstallNetplayWindowHook(screenContext);
    if (netplay::mod_settings::IsMenuTtfTextEnabled())
    {
        (void)netplay::battle_log::EnsureGameplayOverlayHook();
    }
    netplay::bridge::frontend_return::EnsureFrontendReturnUpdateHooks();
    return false;
}

void HandoffSpectateSession(uint32_t screenContext)
{
    const bool prepared = netplay::bridge::PrepareVsHumanHandoff();
    const netplay::bridge::NetbridgeStatus status = netplay::bridge::GetStatus();
    mod::Log(
        "HandoffSpectateSession: begin prepared=%d sync(mode=%d flag1084=%d session=%d flags=%d/%d role=%d)",
        prepared ? 1 : 0,
        status.syncGameMode,
        status.syncMode0Flag1084,
        status.syncSessionByte,
        status.syncGlobalFlag4964,
        status.syncGlobalFlag4965,
        status.roleFlag);

    if (!prepared)
    {
        mod::Log(
            "HandoffSpectateSession: aborted because spectator handoff is not ready");
        return;
    }
    if (!SuspendUiHooksForOnlineSimulation(
            screenContext, "spectator_handoff"))
    {
        return;
    }

    RunTransitionFadeOut(screenContext, 0, 0);
    PrepareSpectateReplayState(screenContext);

    // The spectator's charselect screen requires the same game-system state
    // as online VS Human: mode 4, CPU flags 0, rounds, and a clean
    // charselect init/exit flag pair.  The DLL's spectator never sets these
    // - it relies on the host EXE having the right state.  Without this
    // call the spectator enters charselect with stale flags (wrong mode,
    // possibly CPU players, stale exit flag) which causes silent desync.
    PrepareVsHumanGameState(screenContext);

    // Post-preparation verification.
    // The DLL client session (type 1, dword_100A05D0=1) uses shared memory
    // IPC and input replay.  The DLL frame hook's spectate watcher creation
    // requires dword_100A05D0==2 (practice session) which never matches the
    // mod's spectate sessions.  Therefore no watcher is ever created and the
    // client session survives through all mode transitions.  We transition
    // directly to charselect (mode 1) - bypassing the replay screen (mode 8)
    // avoids wasting shared-memory input frames on a screen that serves no
    // purpose for the client session.
    {
        const int verifyMenuSel = static_cast<int>(
            *reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection));
        const int verifyScreen = *reinterpret_cast<const int*>(kVaCurrentScreenIndex);
        const netplay::bridge::NetbridgeStatus postStatus = netplay::bridge::GetStatus();
        mod::Log(
            "HandoffSpectateSession: verify menuSel=%d (expect 4) screen=%d (expect 0) "
            "role=%d (expect 1/spectate) syncMode=%d peerAlive=%d",
            verifyMenuSel, verifyScreen, postStatus.roleFlag,
            postStatus.syncGameMode,
            netplay::bridge::IsPeerProcessAlive() ? 1 : 0);
    }

    if (g_netplayMenuState.bgmActive)
    {
        StopCurrentBgm(screenContext, "handoff_spectate_session");
    }

    g_netplayMenuState.active = false;
    g_netplayMenuState.bgmActive = false;
    g_netplayMenuState.useConfigStyleRender = false;
    g_netplayMenuState.menuId = NetplayMenuId::Main;
    g_netplayMenuState.mainSelection = 0;
    g_netplayMenuState.optionCount = kNetplayDefaultOptionCount;
    g_netplayMenuState.backIndex = kNetplayDefaultBackIndex;
    g_netplayMenuState.renderLayout = {};
    g_netplayMenuState.lobbyScrollOffset = 0;
    ResetMenuSlideTransition();
    ResetInlineEditState();
    ClearNetplayStatusMessage();
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = false;
    g_pendingVsHumanAutoConfirm = false;
    g_pendingVsHumanAutoConfirmTick = 0;
    g_pendingVsHumanAutoConfirmLastLogTick = 0;
    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
    ResetHostingOverlayState();
    ResetJoiningOverlayState();
    ClearPendingLobbySpectateWait("handoff_spectate_session");
    g_returnToNetplayAfterMatch = true;

    // Spectating should suppress room actions locally while we are out of the
    // lobby UI, but it must not reuse the real match accept/end lifecycle.
    // Mark a local spectate-busy state here instead of driving accept/end.
    if (g_lobbySession)
    {
        mod::Log("HandoffSpectateSession: notifying lobby - entering spectate lifecycle");
        g_lobbySession->NotifySpectateStarted();
    }

    // Transition directly to charselect (mode 1).  The DLL's client
    // session (type 1) survives mode transitions and drives the game via
    // input replay from shared memory.  Do not clear gameSystem input bytes
    // here: spectator replay must preserve the exact recorded frame that
    // Revival just applied before memorial_advance_screen reached this hook.
    ClearLocalMenuControlState(screenContext);
    ResetSpectateHandoffWarmup(nullptr);
    g_pendingGlobalStateTransition = kScreenIndexCharSelect;

    mod::Log(
        "HandoffSpectateSession: queued global transition nextState=%d returnToNetplay=%d",
        g_pendingGlobalStateTransition,
        g_returnToNetplayAfterMatch ? 1 : 0);
}

void EnterNetplayMenu(uint32_t screenContext, bool skipFadeOut)
{
    netplay::bridge::state_export::ResumeControlPlaneUpdates();
    mod::Log("EnterNetplayMenu: request active=%d skipFadeOut=%d", g_netplayMenuState.active, skipFadeOut ? 1 : 0);
    if (g_netplayMenuState.active)
    {
        mod::Log("EnterNetplayMenu: already active, ignoring duplicate entry");
        return;
    }

    // The game-RT TTF text layer (footer tooltips, battle log) renders from
    // the shared D3D9 EndScene hook - install it up front so menu text is
    // crisp from the first page, not only after the battle log installs it.
    if (netplay::mod_settings::IsMenuTtfTextEnabled())
    {
        (void)netplay::battle_log::EnsureGameplayOverlayHook();
    }

    bool recoveryOwnedMenuEntry = false;
    if (skipFadeOut)
    {
        recoveryOwnedMenuEntry =
            netplay::bridge::recovery::ShouldSuppressLegacyGameplayExitCleanup()
            || netplay::bridge::takeover::IsDeferredCancelCleanupGameplaySource();
        if (recoveryOwnedMenuEntry)
        {
            netplay::bridge::recovery::NoteGameplayExitMenuEntryStarted(
                screenContext,
                "enter_netplay_menu_skipFadeOut");
            (void)netplay::bridge::takeover::SuppressDeferredCancelCleanupAfterGameplayRecovery(
                netplay::bridge::recovery::CurrentGameplayExitRecoveryOrigin());
        }

        const int patchStateBeforeEntry =
            netplay::bridge::takeover::GetRevivalGraphicsPatchState();
        mod::Log(
            "RECOVERY_MENU_PATCH_STATE_BEFORE_ENTRY state=%d",
            patchStateBeforeEntry);
        const bool patchRestoreOk =
            netplay::bridge::takeover::EnsureRevivalGraphicsPatchSetEnabled(
                "before_recovery_menu_entry");
        mod::Log(
            "RECOVERY_MENU_PATCH_STATE_RESTORED result=%d",
            patchRestoreOk ? 1 : 0);

        const netplay::bridge::frontend_return::FrontendContext ctx =
            netplay::bridge::frontend_return::CaptureFrontendContext();
        mod::Log(
            "RECOVERY_MENU_VISUAL_BEGIN screenContext=0x%08X screen=%u mode=%u +44=%u +45=%u active=%d configStyle=%d",
            screenContext,
            static_cast<unsigned>(ctx.rawScreen),
            static_cast<unsigned>(ctx.gameModeRaw),
            static_cast<unsigned>(ctx.lifecycle44),
            static_cast<unsigned>(ctx.exit45),
            g_netplayMenuState.active ? 1 : 0,
            g_netplayMenuState.useConfigStyleRender ? 1 : 0);

        uint8_t old44 = 255;
        uint8_t old45 = 255;
        uint8_t new44 = 255;
        uint8_t new45 = 255;
        __try
        {
            old44 = *reinterpret_cast<uint8_t*>(
                screenContext + netplay::constants::kOffsetScreenInitState);
            old45 = *reinterpret_cast<uint8_t*>(
                screenContext + netplay::constants::kOffsetScreenExitState);
            *reinterpret_cast<uint8_t*>(
                screenContext + netplay::constants::kOffsetScreenInitState) = 0;
            *reinterpret_cast<uint8_t*>(
                screenContext + netplay::constants::kOffsetScreenExitState) = 0;
            new44 = *reinterpret_cast<uint8_t*>(
                screenContext + netplay::constants::kOffsetScreenInitState);
            new45 = *reinterpret_cast<uint8_t*>(
                screenContext + netplay::constants::kOffsetScreenExitState);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        mod::Log(
            "RECOVERY_MENU_LIFECYCLE_NORMALIZE old44=%u old45=%u new44=%u new45=%u",
            static_cast<unsigned>(old44),
            static_cast<unsigned>(old45),
            static_cast<unsigned>(new44),
            static_cast<unsigned>(new45));
    }

    if (!skipFadeOut)
    {
        RunTransitionFadeOut(screenContext, 0, 0);
    }
    else
    {
        // Disconnect recovery: the DirectDraw front/back buffers still
        // contain stale battle-scene pixels with a mismatched palette.
        // Skip the fade-out entirely - we will load fresh assets and
        // set the hardware palette before doing a clean fade-in.
        mod::Log("EnterNetplayMenu: skipping fade-out (disconnect recovery)");
    }

    if (!LoadNetplayAssets(screenContext))
    {
        mod::Log("EnterNetplayMenu: assets load failed - netplay menu disabled");
        mod::Log("EnterNetplayMenu: ensure netplay_bgd.dat, netplay_bgn.dat, and netplay_ob.dat are next to the DLL or in mods\\efz_netplay_mod\\assets\\");
        g_netplayAssetsAvailable = false;
        if (!skipFadeOut)
        {
            (void)LoadTitleAssets(screenContext);
            RunTransitionFadeIn(screenContext);
        }
        return;
    }

    LoadSerializedNetplayMenuSettingsFromIni();
    ResetTitleMenuState(screenContext, 0);
    ResetMenuControlCompatibilityState();
    (void)SyncMenuControlBindings(screenContext);

    // If a lobby session is still alive (e.g. returning from a lobby match),
    // re-enter the Lobby menu directly instead of Main so the user stays in
    // the lobby and can immediately see the player list / challenge again.
    const bool returnToLobby = (g_lobbySession != nullptr);
    g_deferredLobbyRefreshPending = returnToLobby && skipFadeOut;
    g_deferredLobbyRefreshDeadlineTick =
        g_deferredLobbyRefreshPending ? (GetTickCount() + kDeferredLobbyRefreshTimeoutMs) : 0;

    // As soon as we re-enter the menu after a match, flush any deferred
    // host End so the server drops the playing-pair right away.  This is
    // independent of the deferred-refresh gate - we want the End to go out
    // even if the bridge takes a while to reach a terminal phase.
    if (returnToLobby)
    {
        g_lobbySession->FlushDeferredEndOnReturn();
    }

    g_netplayMenuState.active = true;
    // Prewarm only local family capability on its own retained worker. The
    // eventual Host action does not wait for this scan and still performs just
    // its global public-address lookup.
    EnsureLocalNetworkCapabilityScanScheduled("enter_netplay_menu");
    g_netplayMenuState.bgmActive = true;
    g_netplayMenuState.menuId = returnToLobby ? NetplayMenuId::Lobby : NetplayMenuId::Main;
    g_netplayMenuState.mainSelection = 0;
    g_netplayMenuState.backgroundScrollOffset = 0.0;
    g_netplayMenuState.backgroundScrollTick = 0;
    g_charSelectResetPending = true;
    ResetMenuSlideTransition();
    ResetInlineEditState();
    ClearNetplayStatusMessage();
    g_lastNetplayFrameLogTick = 0;
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    ResetWindowFocusInputSuppression();
    g_pendingVsHumanAutoConfirm = false;
    g_pendingVsHumanAutoConfirmTick = 0;
    g_pendingVsHumanAutoConfirmLastLogTick = 0;
    g_pendingGlobalStateTransition = -1;
    g_returnToNetplayAfterMatch = false;
    ResetVsHumanHandoffWarmup("enter_netplay_menu");
    ResetSpectateHandoffWarmup("enter_netplay_menu");
    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
    ResetHostingOverlayState();
    ResetJoiningOverlayState();
    ClearPendingLobbySpectateWait("enter_netplay_menu");
    ResetLobbyChallengeNotificationState();
    ReleaseLoadedSoundBuffer(
        GetGameSystem(screenContext),
        &g_lobbyChallengeAlertBufferIndex,
        "enter_netplay");
    const NetplayMenuId targetMenu = returnToLobby ? NetplayMenuId::Lobby : NetplayMenuId::Main;
    SwitchToMenu(screenContext, targetMenu, -1);
    if (skipFadeOut && recoveryOwnedMenuEntry)
    {
        BeginRecoveryMenuInputQuarantine(
            screenContext,
            netplay::bridge::recovery::CurrentGameplayExitRecoveryOrigin());
    }
    InstallNetplayWindowHook(screenContext);

    PlayNetplayBgm(screenContext);

    if (skipFadeOut)
    {
        // Disconnect recovery: force the hardware palette to the freshly-
        // loaded netplay palette so the first rendered frame uses correct
        // colors, then render one clean frame into the back buffer and
        // present it.  This guarantees the front buffer has netplay-menu
        // content before the fade-in begins.
        auto const setPalette = reinterpret_cast<SetPaletteFn>(RuntimeAddress(kVaSetPalette));
        setPalette(GetGraphicsContext(screenContext), static_cast<int>(screenContext + kOffsetPalette));

        if (g_useRuntimeTextOverlay)
            (void)RenderNetplayMenuRuntimeText(screenContext);
        else if (g_netplayMenuState.useConfigStyleRender)
            (void)RenderNetplayMenuConfigStyle(screenContext);
        else
        {
            auto const render = GetOriginalTitleRender();
            (void)render(screenContext);
        }
        mod::Log("EnterNetplayMenu: disconnect recovery - rendered initial clean frame");
    }

    RunTransitionFadeIn(screenContext);

    // Async hosting: if a host listener is still active when the netplay menu is
    // (re-)entered, make sure the hosting overlay is available so the minimized
    // HOSTING badge shows again. We stay MINIMIZED (badge on Main); selecting
    // HOST un-minimizes to the full prompt. The host session persists across
    // menu exits (LeaveNetplayMenu keeps it alive while hosting).
    if (netplay::bridge::async_host::IsActive())
    {
        RestoreHostingOverlayFromAsyncHost();
        if (netplay::bridge::async_host::ConsumeReturnKeyArrival())
        {
            // Arrived via the F1 return hotkey pressed in gameplay: show the FULL
            // hosting overlay (un-minimized) in the HOST submenu so a held peer is
            // auto-accepted (the connecting branch accepts when not minimized) and
            // the user can act on the session.
            netplay::bridge::async_host::SetMinimized(false);
            SwitchToMenu(screenContext, NetplayMenuId::Host, -1);
            mod::Log(
                "AsyncHost: F1 return arrived - restored full HOST overlay (state=%d)",
                static_cast<int>(netplay::bridge::async_host::GetState()));
        }
        else
        {
            netplay::bridge::async_host::SetMinimized(true);
            mod::Log(
                "AsyncHost: menu re-entry with active host - badge restored (state=%d)",
                static_cast<int>(netplay::bridge::async_host::GetState()));
        }
    }

    mod::Log(
        "EnterNetplayMenu: active menu=%s selection=%d bgmTrack=%u configStyle=%d theme=%d bgScrollSupported=%d optionCount=%d backIndex=%d skipFadeOut=%d deferredLobbyRefresh=%d",
        MenuIdToString(g_netplayMenuState.menuId),
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)),
        kNetplayBgmTrack,
        g_netplayMenuState.useConfigStyleRender,
        static_cast<int>(g_netplayMenuState.theme),
        g_netplayMenuState.backgroundSupportsScroll ? 1 : 0,
        g_netplayMenuState.optionCount,
        g_netplayMenuState.backIndex,
        skipFadeOut ? 1 : 0,
        g_deferredLobbyRefreshPending ? 1 : 0);
}

void LeaveNetplayMenu(uint32_t screenContext, bool keepHostSession)
{
    PumpLobbySessionShutdown();
    mod::Log("LeaveNetplayMenu: request active=%d bgmActive=%d keepHostSession=%d",
        g_netplayMenuState.active, g_netplayMenuState.bgmActive, keepHostSession ? 1 : 0);
    if (!g_netplayMenuState.active)
    {
        mod::Log("LeaveNetplayMenu: already inactive");
        return;
    }

    RunTransitionFadeOut(screenContext, 0, 0);

    if (g_netplayMenuState.bgmActive)
    {
        StopCurrentBgm(screenContext, "leave_netplay");
    }
    ReleaseLoadedSoundBuffer(
        GetGameSystem(screenContext),
        &g_lobbyChallengeAlertBufferIndex,
        "leave_netplay");

    // Keep the host listener alive across menu exits whenever async hosting is
    // active - leaving the netplay menu does NOT stop hosting. The user cancels
    // hosting only by pressing B in the full HOSTING prompt (or via the
    // "Stop hosting?" modal). EnterNetplayMenu restores the badge on re-entry,
    // and HOST re-opens the full prompt.
    const bool keepHost = keepHostSession || netplay::bridge::async_host::IsActive();
    if (!keepHost)
    {
        netplay::bridge::CancelSession("leave_menu");

        // Defensive: tear down any active lobby session so the server is
        // notified (/leave) and the poll thread stops.  Normally unreachable
        // because SwitchToMenu handles this when navigating away from the
        // Lobby menu, but guards against future call-site additions.
        if (g_lobbySession)
        {
            mod::Log("LeaveNetplayMenu: tearing down g_lobbySession defensively");
            BeginLobbySessionShutdown(false);
        }
    }
    else
    {
        mod::Log("LeaveNetplayMenu: preserving active host session across menu exit");
    }

    g_netplayMenuState.active = false;
    g_netplayMenuState.bgmActive = false;
    g_netplayMenuState.useConfigStyleRender = false;
    g_netplayMenuState.menuId = NetplayMenuId::Main;
    g_netplayMenuState.mainSelection = 0;
    g_netplayMenuState.optionCount = kNetplayDefaultOptionCount;
    g_netplayMenuState.backIndex = kNetplayDefaultBackIndex;
    g_netplayMenuState.renderLayout = {};
    g_netplayMenuState.backgroundSupportsScroll = false;
    g_netplayMenuState.backgroundWidth = 320;
    g_netplayMenuState.backgroundHeight = 240;
    g_netplayMenuState.backgroundScrollOffset = 0.0;
    g_netplayMenuState.backgroundScrollTick = 0;
    g_netplayMenuState.lobbyScrollOffset = 0;
    ResetMenuSlideTransition();
    ResetInlineEditState();
    ClearNetplayStatusMessage();
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = false;
    ResetWindowFocusInputSuppression();
    g_pendingVsHumanAutoConfirm = false;
    g_pendingVsHumanAutoConfirmTick = 0;
    g_pendingVsHumanAutoConfirmLastLogTick = 0;
    g_pendingGlobalStateTransition = -1;
    g_returnToNetplayAfterMatch = false;
    ResetVsHumanHandoffWarmup("leave_netplay_menu");
    ResetSpectateHandoffWarmup("leave_netplay_menu");
    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
    if (!keepHost)
    {
        // Preserve the hosting overlay state while a host session persists so the
        // badge/full prompt can be restored when the menu is re-entered.
        ResetHostingOverlayState();
    }
    ResetJoiningOverlayState();
    ClearPendingLobbySpectateWait("leave_netplay_menu");
    ResetLobbyChallengeNotificationState();
    g_deferredLobbyRefreshPending = false;
    g_deferredLobbyRefreshDeadlineTick = 0;
    RemoveNetplayWindowHook();

    (void)LoadTitleAssets(screenContext);
    ResetTitleMenuState(screenContext, 5);
    RunTransitionFadeIn(screenContext);
    mod::Log(
        "LeaveNetplayMenu: returned to title assets, titleSelection=%d",
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
}

void HandoffConnectedSessionToVsHumanState(uint32_t screenContext)
{
    const bool prepared = netplay::bridge::PrepareVsHumanHandoff();
    const netplay::bridge::NetbridgeStatus status = netplay::bridge::GetStatus();

    // Pre-handoff diagnostic: read charselect screen object state BEFORE we
    // modify anything, so we can see if byte[45] (exit flag) was stale.
    uint8_t preInit = 0xFF, preExit = 0xFF, preMode = 0xFF, preSecondary = 0xFF;
    uint32_t preCharSelectObj = 0;
    __try
    {
        const auto* screenTable = reinterpret_cast<const uint32_t*>(
            RuntimeAddress(kVaScreenObjectTable));
        preCharSelectObj = screenTable[1];
        if (preCharSelectObj != 0)
        {
            preInit = *reinterpret_cast<const uint8_t*>(preCharSelectObj + kOffsetScreenInitState);
            preExit = *reinterpret_cast<const uint8_t*>(preCharSelectObj + kOffsetScreenExitState);
            const uint32_t gameSys = *reinterpret_cast<const uint32_t*>(
                preCharSelectObj + kOffsetGameSystem);
            if (gameSys != 0)
            {
                preMode = *reinterpret_cast<const uint8_t*>(gameSys + kGameSystemOffsetMode);
                preSecondary = *reinterpret_cast<const uint8_t*>(gameSys + kGameSystemOffsetSecondaryModeFlag);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    mod::Log(
        "HandoffConnectedSessionToVsHumanState: begin prepared=%d "
        "sync(mode=%d flag1084=%d session=%d flags=%d/%d role=%d) "
        "screen=%d peerAlive=%d "
        "PRE_charselect(obj=0x%08lX init=%u exit=%u mode=%u/%u)",
        prepared ? 1 : 0,
        status.syncGameMode,
        status.syncMode0Flag1084,
        status.syncSessionByte,
        status.syncGlobalFlag4964,
        status.syncGlobalFlag4965,
        status.roleFlag,
        *reinterpret_cast<const int*>(kVaCurrentScreenIndex),
        netplay::bridge::IsPeerProcessAlive() ? 1 : 0,
        static_cast<unsigned long>(preCharSelectObj),
        static_cast<unsigned>(preInit),
        static_cast<unsigned>(preExit),
        static_cast<unsigned>(preMode),
        static_cast<unsigned>(preSecondary));

    if (!prepared)
    {
        mod::Log(
            "HandoffConnectedSessionToVsHumanState: aborted because VS-human handoff is not ready");
        return;
    }
    if (!SuspendUiHooksForOnlineSimulation(
            screenContext, "connected_player_handoff"))
    {
        return;
    }

    RunTransitionFadeOut(screenContext, 0, 0);
    PrepareVsHumanGameState(screenContext);

    if (g_netplayMenuState.bgmActive)
    {
        StopCurrentBgm(screenContext, "handoff_connected_session");
    }

    g_netplayMenuState.active = false;
    g_netplayMenuState.bgmActive = false;
    g_netplayMenuState.useConfigStyleRender = false;
    g_netplayMenuState.menuId = NetplayMenuId::Main;
    g_netplayMenuState.mainSelection = 0;
    g_netplayMenuState.optionCount = kNetplayDefaultOptionCount;
    g_netplayMenuState.backIndex = kNetplayDefaultBackIndex;
    g_netplayMenuState.renderLayout = {};
    g_netplayMenuState.lobbyScrollOffset = 0;
    ResetMenuSlideTransition();
    ResetInlineEditState();
    ClearNetplayStatusMessage();
    g_hasLoggedInputSnapshot = false;
    g_netplayEscapeDown = false;
    g_pendingVsHumanAutoConfirm = false;
    g_pendingVsHumanAutoConfirmTick = 0;
    g_pendingVsHumanAutoConfirmLastLogTick = 0;
    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
    ResetHostingOverlayState();
    ResetJoiningOverlayState();
    ClearLocalMenuControlState(screenContext);
    ResetVsHumanHandoffWarmup(nullptr);
    g_returnToNetplayAfterMatch = true;
    g_pendingGlobalStateTransition = kScreenIndexCharSelect;

    // Post-handoff diagnostic: read charselect screen object state AFTER
    // PrepareVsHumanGameState to verify flags were set correctly.
    uint8_t postInit = 0xFF, postExit = 0xFF, postMode = 0xFF, postSecondary = 0xFF;
    __try
    {
        if (preCharSelectObj != 0)
        {
            postInit = *reinterpret_cast<const uint8_t*>(preCharSelectObj + kOffsetScreenInitState);
            postExit = *reinterpret_cast<const uint8_t*>(preCharSelectObj + kOffsetScreenExitState);
            const uint32_t gameSys = *reinterpret_cast<const uint32_t*>(
                preCharSelectObj + kOffsetGameSystem);
            if (gameSys != 0)
            {
                postMode = *reinterpret_cast<const uint8_t*>(gameSys + kGameSystemOffsetMode);
                postSecondary = *reinterpret_cast<const uint8_t*>(gameSys + kGameSystemOffsetSecondaryModeFlag);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}

    mod::Log(
        "HandoffConnectedSessionToVsHumanState: queued global transition nextState=%d returnToNetplay=%d "
        "POST_charselect(init=%u exit=%u mode=%u/%u)",
        g_pendingGlobalStateTransition,
        g_returnToNetplayAfterMatch ? 1 : 0,
        static_cast<unsigned>(postInit),
        static_cast<unsigned>(postExit),
        static_cast<unsigned>(postMode),
        static_cast<unsigned>(postSecondary));
}

void SwitchToMenu(uint32_t screenContext, NetplayMenuId menuId, int selection)
{
    PumpLobbySessionShutdown();
    const NetplayMenuId previousMenu = g_netplayMenuState.menuId;
    if (g_inlineEditState.active)
    {
        CancelInlineEdit();
    }
    if (previousMenu != menuId)
    {
        ClearNetplayStatusMessage();
    }
    if (previousMenu == NetplayMenuId::Options && menuId != NetplayMenuId::Options)
    {
        netplay::options::LeaveMenu();
    }
    if (previousMenu == NetplayMenuId::BattleLog && menuId != NetplayMenuId::BattleLog)
    {
        netplay::battle_log::LeaveMenu();
    }
    if (previousMenu == NetplayMenuId::PlayerRooms
        && menuId != NetplayMenuId::PlayerRooms
        && menuId != NetplayMenuId::Lobby)
    {
        netplay::player_rooms::LeaveMenu();
    }
    // Lobby session lifecycle: destroy when navigating away, create when entering.
    if (previousMenu == NetplayMenuId::Lobby && menuId != NetplayMenuId::Lobby)
    {
        g_deferredLobbyRefreshPending = false;
        g_deferredLobbyRefreshDeadlineTick = 0;
        if (g_lobbySession)
        {
            const bool returnToPlayerRooms =
                (menuId == NetplayMenuId::PlayerRooms
                    && g_lobbySession->GetOrigin() == netplay::lobby::RoomOrigin::PlayerRooms);
            mod::Log(
                "SwitchToMenu: leaving Lobby, tearing down lobby session asynchronously returnToPlayerRooms=%d",
                returnToPlayerRooms ? 1 : 0);
            BeginLobbySessionShutdown(returnToPlayerRooms);
        }
        g_netplayMenuState.lobbyScrollOffset = 0;
        ResetLobbyChallengeNotificationState();
    }
    g_netplayMenuState.menuId = menuId;
    if (menuId == NetplayMenuId::Options)
    {
        const bool loaded = netplay::options::EnterMenu();
        mod::Log("SwitchToMenu: entering Options loaded=%d", loaded ? 1 : 0);
    }
    if (menuId == NetplayMenuId::BattleLog)
    {
        const bool loaded = netplay::battle_log::EnterMenu();
        mod::Log("SwitchToMenu: entering BattleLog loaded=%d", loaded ? 1 : 0);
    }
    if (menuId == NetplayMenuId::PlayerRooms)
    {
        const bool loaded = netplay::player_rooms::EnterMenu();
        mod::Log("SwitchToMenu: entering PlayerRooms loaded=%d", loaded ? 1 : 0);
    }
    if (menuId == NetplayMenuId::Lobby && !g_lobbySession)
    {
        // Seed the dynamic entry list with 0 idle players before any spec
        // queries so that GetCurrentMenuEntryCount() returns a valid count.
        RebuildLobbyMenuEntries(0, 0);
        g_netplayMenuState.lobbyScrollOffset = 0;
        netplay::lobby::LobbyJoinedRoom joinedRoom = {};
        if (netplay::player_rooms::ConsumePendingJoinedRoom(&joinedRoom))
        {
            mod::Log(
                "SwitchToMenu: entering Lobby from PlayerRooms roomCode='%s' roomId=%d type='%s'",
                joinedRoom.roomCode.c_str(),
                joinedRoom.lobbyNumericId,
                joinedRoom.roomType.c_str());
            g_lobbySession = std::make_unique<netplay::lobby::LobbySession>(
                g_netplayMenuState.nickname,
                g_netplayMenuState.hostPort,
                &joinedRoom,
                g_netplayMenuState.hostFamily);
        }
        else
        {
            mod::Log("SwitchToMenu: entering Lobby, creating global session for '%s' port=%u",
                g_netplayMenuState.nickname.c_str(),
                static_cast<unsigned>(g_netplayMenuState.hostPort));
            g_lobbySession = std::make_unique<netplay::lobby::LobbySession>(
                g_netplayMenuState.nickname,
                g_netplayMenuState.hostPort,
                nullptr,
                g_netplayMenuState.hostFamily);
        }
    }
    else if (menuId == NetplayMenuId::Lobby && g_lobbySession)
    {
        // Re-entering Lobby with an existing session (e.g. returning from a
        // lobby match). During disconnect/post-match recovery, defer the
        // first refresh until the Lobby screen is actively running again so
        // the local "returning from match" guard survives the recovery phase.
        RebuildLobbyMenuEntries(0, 0);
        g_netplayMenuState.lobbyScrollOffset = 0;
        if (g_deferredLobbyRefreshPending)
        {
            mod::Log("SwitchToMenu: re-entering Lobby with existing session, refresh deferred until recovery completes");
        }
        else
        {
            g_lobbySession->RequestRefresh();
            mod::Log("SwitchToMenu: re-entering Lobby with existing session, requested immediate refresh");
        }
    }
    int requestedSelection = selection;
    if (requestedSelection < 0)
    {
        requestedSelection = GetDefaultSelectionForMenu(menuId);
    }
    const int clamped = ClampSelectionToCurrentMenu(requestedSelection);
    g_netplayMenuState.optionCount = GetCurrentMenuEntryCount();
    g_netplayMenuState.backIndex = g_netplayMenuState.optionCount > 0 ? g_netplayMenuState.optionCount - 1 : 0;

    *reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection) = static_cast<int8_t>(clamped);
    *reinterpret_cast<uint16_t*>(screenContext + kOffsetMenuAnimCounter) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
    *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
    *reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter) = 0;
    g_lastLoggedSelection = static_cast<int8_t>(clamped);

    const NetplayMenuEntry* entry = GetCurrentMenuEntry(clamped);
    const int rowIndex = GetRenderRowForSelection(clamped);
    mod::Log(
        "NetplayMenuSwitch: menu=%s selection=%d count=%d row=%d(%s) entry=%s",
        MenuIdToString(menuId),
        clamped,
        g_netplayMenuState.optionCount,
        rowIndex,
        RowIndexToString(rowIndex),
        entry != nullptr ? entry->debugLabel : "none");
}

void ShowStubActionMessage(HWND owner, const std::string& message)
{
    (void)owner;
    SetNetplayStatusMessage(message.c_str());
}

bool StartJoinSpectateIpAction(uint32_t screenContext, bool playSuccessSound)
{
    std::string errorMessage;
    if (TryStartWaitToSpectateFromJoinSettings(screenContext, &errorMessage))
    {
        if (playSuccessSound)
        {
            PlayUiSound(screenContext, kSfxConfirm);
        }
        return true;
    }

    const HWND owner = reinterpret_cast<HWND>(
        *reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
    ShowStubActionMessage(
        owner,
        "Spectate IP failed.\n\n"
        + (errorMessage.empty() ? std::string("Unknown error") : errorMessage));
    return false;
}

void SetNetplayStatusMessage(const char* text, DWORD durationMs)
{
    g_netplayStatusMessage.clear();
    g_netplayStatusExpireTick = 0;

    if (text == nullptr || text[0] == '\0')
    {
        return;
    }

    std::string normalized;
    normalized.reserve(std::strlen(text));

    bool lastWasNewline = false;
    for (const char* p = text; *p != '\0'; ++p)
    {
        const char ch = *p;
        if (ch == '\r')
        {
            continue;
        }
        if (ch == '\n')
        {
            if (!lastWasNewline)
            {
                normalized.push_back('\n');
                lastWasNewline = true;
            }
            continue;
        }

        normalized.push_back(ch);
        lastWasNewline = false;
    }

    while (!normalized.empty() && (normalized.back() == ' ' || normalized.back() == '\n'))
    {
        normalized.pop_back();
    }

    if (const size_t firstNewline = normalized.find('\n'); firstNewline != std::string::npos)
    {
        for (size_t extra = normalized.find('\n', firstNewline + 1);
             extra != std::string::npos;
             extra = normalized.find('\n', extra + 1))
        {
            normalized[extra] = ' ';
        }
    }

    g_netplayStatusMessage = std::move(normalized);
    if (!g_netplayStatusMessage.empty())
    {
        g_netplayStatusExpireTick = GetTickCount() + durationMs;
        mod::Log(
            "NetplayStatus: '%s' durationMs=%lu",
            g_netplayStatusMessage.c_str(),
            static_cast<unsigned long>(durationMs));
    }
}

bool HasNetplayStatusMessage()
{
    return !g_netplayStatusMessage.empty() && GetTickCount() < g_netplayStatusExpireTick;
}

void ClearNetplayStatusMessage()
{
    g_netplayStatusMessage.clear();
    g_netplayStatusExpireTick = 0;
}

std::string GetNetplayStatusMessage()
{
    if (!HasNetplayStatusMessage())
    {
        return {};
    }
    return g_netplayStatusMessage;
}

NetplayMenuId ResolveCancelTargetMenu()
{
    if (g_netplayMenuState.menuId == NetplayMenuId::Lobby
        && g_lobbySession
        && g_lobbySession->GetOrigin() == netplay::lobby::RoomOrigin::PlayerRooms)
    {
        return NetplayMenuId::PlayerRooms;
    }
    return NetplayMenuId::Main;
}

// If an async-host listener is active, arm the "Stop hosting?" confirm modal for
// |action| (deferred until the user confirms) and return true so the caller
// skips the navigation/action. Returns false when not hosting (caller proceeds).
bool RequestStopHostingConfirmIfHosting(
    uint32_t screenContext,
    NetplayMenuAction action,
    int logicalSelection)
{
    if (!netplay::bridge::async_host::IsActive())
    {
        return false;
    }
    g_stopHostingConfirm.active = true;
    g_stopHostingConfirm.waitingForInputRelease = true;
    g_stopHostingConfirm.confirmDown[0] = 0;
    g_stopHostingConfirm.confirmDown[1] = 0;
    g_stopHostingConfirm.cancelDown[0] = 0;
    g_stopHostingConfirm.cancelDown[1] = 0;
    g_stopHostingConfirm.selection = 1; // default: Keep hosting
    g_stopHostingConfirm.pendingAction = action;
    g_stopHostingConfirm.pendingLogicalSelection = logicalSelection;
    PlayUiSound(screenContext, kSfxMove);
    mod::Log("StopHostingConfirm: armed for action=%d", static_cast<int>(action));
    return true;
}

void ExecuteNetplayAction(uint32_t screenContext, NetplayMenuAction action, int logicalSelection)
{
    const HWND owner = reinterpret_cast<HWND>(*reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
    const int selectedRow = GetRenderRowForSelection(logicalSelection);
    mod::Log(
        "NetplayAction: menu=%s selection=%d row=%d(%s) action=%s",
        MenuIdToString(g_netplayMenuState.menuId),
        logicalSelection,
        selectedRow,
        RowIndexToString(selectedRow),
        MenuActionToString(action));

    if (g_netplayMenuState.menuId == NetplayMenuId::Options
        && netplay::options::ExecuteAction(screenContext, action))
    {
        return;
    }
    if (g_netplayMenuState.menuId == NetplayMenuId::BattleLog
        && netplay::battle_log::ExecuteAction(screenContext, action))
    {
        return;
    }
    if (g_netplayMenuState.menuId == NetplayMenuId::PlayerRooms
        && netplay::player_rooms::ExecuteAction(screenContext, action))
    {
        return;
    }

    switch (action)
    {
    case NetplayMenuAction::OpenHost:
        g_netplayMenuState.mainSelection = logicalSelection;
        // Restore from minimized async hosting - the full hosting overlay shows
        // again in the Host submenu.
        if (netplay::bridge::async_host::IsActive())
        {
            netplay::bridge::async_host::SetMinimized(false);
        }
        StartMenuSlideTransition(screenContext, NetplayMenuId::Host, -1, +1);
        break;
    case NetplayMenuAction::OpenJoin:
        if (RequestStopHostingConfirmIfHosting(screenContext, action, logicalSelection))
        {
            break;
        }
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Join, -1, +1);
        break;
    case NetplayMenuAction::OpenPlayerRooms:
        if (RequestStopHostingConfirmIfHosting(screenContext, action, logicalSelection))
        {
            break;
        }
        if (g_lobbySessionShutdownInFlight.load())
        {
            ShowStubActionMessage(owner, "Still leaving room.\nPlease wait.");
            break;
        }
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::PlayerRooms, -1, +1);
        break;
    case NetplayMenuAction::OpenLobby:
        if (RequestStopHostingConfirmIfHosting(screenContext, action, logicalSelection))
        {
            break;
        }
        if (g_lobbySessionShutdownInFlight.load())
        {
            ShowStubActionMessage(owner, "Still leaving room.\nPlease wait.");
            break;
        }
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Lobby, -1, +1);
        break;
    case NetplayMenuAction::OpenBattleLog:
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::BattleLog, -1, +1);
        break;
    case NetplayMenuAction::OpenOptions:
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Options, -1, +1);
        break;
    case NetplayMenuAction::BackToMain:
        if (g_netplayMenuState.menuId == NetplayMenuId::Lobby
            && g_lobbySession
            && g_lobbySession->GetOrigin() == netplay::lobby::RoomOrigin::PlayerRooms)
        {
            StartMenuSlideTransition(screenContext, NetplayMenuId::PlayerRooms, -1, -1);
        }
        else
        {
            StartMenuSlideTransition(screenContext, NetplayMenuId::Main, g_netplayMenuState.mainSelection, -1);
        }
        break;
    case NetplayMenuAction::LeaveNetplay:
        LeaveNetplayMenu(screenContext);
        break;
    case NetplayMenuAction::HostEditPort:
        BeginInlineEdit(NetplayMenuAction::HostEditPort);
        break;
    case NetplayMenuAction::JoinEditAddress:
        BeginInlineEdit(NetplayMenuAction::JoinEditAddress);
        break;
    case NetplayMenuAction::JoinEditPort:
        BeginInlineEdit(NetplayMenuAction::JoinEditPort);
        break;
    case NetplayMenuAction::NicknameEdit:
        BeginInlineEdit(NetplayMenuAction::NicknameEdit);
        break;
    case NetplayMenuAction::HostStart:
    {
        // Guard against re-hosting an already-active async host session. If a peer
        // already connected (PeerFoundHeld) this would tear down the held session;
        // if still waiting it would just churn. The full overlay is already up, so
        // ignore a stray Start. (Auto-accept handles the peer-found case.)
        if (netplay::bridge::async_host::IsActive())
        {
            mod::Log("HostStart: ignored - async host already active (state=%d)",
                static_cast<int>(netplay::bridge::async_host::GetState()));
            break;
        }
        const bool writeNicknameToIni = ShouldWriteNicknameToRevivalIniForSessionStart();
        BeginDirectHostDiscovery(
            g_netplayMenuState.hostPort,
            g_netplayMenuState.hostFamily,
            g_netplayMenuState.nickname.c_str(),
            writeNicknameToIni);
        break;
    }
    case NetplayMenuAction::JoinConnect:
    {
        const bool writeNicknameToIni = ShouldWriteNicknameToRevivalIniForSessionStart();
        const bool started = netplay::bridge::StartSession(
            NetbridgeRole::Join,
            g_netplayMenuState.joinPort,
            g_netplayMenuState.joinAddress.c_str(),
            g_netplayMenuState.nickname.c_str(),
            writeNicknameToIni);
        if (started)
        {
            ActivateJoiningOverlay(
                g_netplayMenuState.joinAddress.c_str(),
                g_netplayMenuState.joinPort);
        }
        else
        {
            const netplay::bridge::NetbridgeStatus status = netplay::bridge::GetStatus();
            char text[320] = {};
            snprintf(
                text,
                sizeof(text),
                "Join start failed.\n\n%s",
                status.errorMsg[0] != '\0' ? status.errorMsg : "Unknown error");
            ShowStubActionMessage(owner, text);
        }
        break;
    }
    case NetplayMenuAction::JoinSpectateIp:
        (void)StartJoinSpectateIpAction(screenContext, false);
        break;
    case NetplayMenuAction::LobbyPlaying0:
    {
        if (!g_lobbySession)
        {
            ShowStubActionMessage(owner, "Lobby status unavailable.");
            break;
        }

        const auto status = g_lobbySession->GetStatus();
        if (status.playing.empty() || status.playing[0].hostIp.empty())
        {
            ShowStubActionMessage(owner, "No active match host information is available for spectating yet.");
            break;
        }
        const auto selectedPair = status.playing[0];

        mod::Log("LobbyPlaying0: spectating playing pair '%s' vs '%s' hostIp=%s inBattle=%d",
            selectedPair.p1Name.c_str(), selectedPair.p2Name.c_str(),
            selectedPair.hostIp.c_str(), status.inBattle ? 1 : 0);

        if (g_lobbySession->IsInBattle())
        {
            mod::Log("LobbyPlaying0: BLOCKED spectate - lobby session still busy");
            ShowStubActionMessage(owner, "Cannot spectate right now.");
            break;
        }

        netplay::network::NetworkEndpoint spectateEndpoint;
        if (!netplay::network::ParseEndpoint(
                selectedPair.hostIp,
                &spectateEndpoint))
        {
            mod::Log(
                "LOBBY_ENDPOINT_CONSUME role=spectate result=rejected raw='%s'",
                selectedPair.hostIp.c_str());
            ShowStubActionMessage(
                owner,
                "The lobby published an invalid host endpoint.\nExpected IPv4:port or [IPv6]:port.");
            break;
        }

        mod::Log(
            "LOBBY_ENDPOINT_CONSUME role=spectate result=accepted family=%s endpoint='%s'",
            netplay::network::FamilyName(spectateEndpoint.family),
            selectedPair.hostIp.c_str());

        std::string errorMessage;
        const bool started =
            TryStartWaitToSpectateAtAddress(
                spectateEndpoint.host.c_str(),
                spectateEndpoint.port,
                &errorMessage);
        mod::Log("LobbyPlaying0: TryStartWaitToSpectateAtAddress returned %d", started ? 1 : 0);
        if (started)
        {
            ArmPendingLobbySpectateWait(&selectedPair, 0, std::string(), selectedPair.hostIp);
            mod::Log("LobbyPlaying0: waiting to spectate active pair");
        }
        else
        {
            char text[320] = {};
            snprintf(
                text,
                sizeof(text),
                "Wait to spectate failed.\n\n%s",
                errorMessage.empty() ? "Unknown error" : errorMessage.c_str());
            mod::Log("LobbyPlaying0: wait-to-spectate FAILED: %s", errorMessage.c_str());
            ShowStubActionMessage(owner, text);
        }
        break;
    }
    case NetplayMenuAction::LobbySlot0:
    case NetplayMenuAction::LobbySlot1:
    case NetplayMenuAction::LobbySlot2:
    case NetplayMenuAction::LobbySlot3:
    case NetplayMenuAction::LobbySlot4:
    case NetplayMenuAction::LobbySlot5:
    {
        const int visSlot = static_cast<int>(action) - static_cast<int>(NetplayMenuAction::LobbySlot0);
        const int realSlot = visSlot + g_netplayMenuState.lobbyScrollOffset;

        if (!g_lobbySession)
        {
            ShowStubActionMessage(owner, "Lobby session unavailable.");
            break;
        }

        const auto lobStatus = g_lobbySession->GetStatus();
        if (realSlot >= static_cast<int>(lobStatus.displayEntries.size()))
        {
            ShowStubActionMessage(owner, "Invalid lobby slot.");
            break;
        }

        const auto& entry = lobStatus.displayEntries[realSlot];

        // Block interactions with our own entry and playing entries.
        if (entry.isSelf)
        {
            ShowStubActionMessage(owner, "That's you!");
            break;
        }
        if (entry.isPlaying && !entry.isChallenge)
        {
            // --- Spectating a playing player ---
            if (g_lobbySession->IsInBattle())
            {
                mod::Log("LobbySpectate: BLOCKED spectate for '%s' id=%d - lobby session still busy",
                    entry.name.c_str(), entry.playerId);
                ShowStubActionMessage(owner, "Cannot spectate right now.");
                break;
            }

            if (entry.spectateIp.empty())
            {
                ShowStubActionMessage(owner, "No host information available\nfor spectating.");
                break;
            }

            netplay::network::NetworkEndpoint spectateEndpoint;
            if (!netplay::network::ParseEndpoint(
                    entry.spectateIp,
                    &spectateEndpoint))
            {
                mod::Log(
                    "LOBBY_ENDPOINT_CONSUME role=spectate result=rejected raw='%s'",
                    entry.spectateIp.c_str());
                ShowStubActionMessage(
                    owner,
                    "The lobby published an invalid host endpoint.\nExpected IPv4:port or [IPv6]:port.");
                break;
            }

            mod::Log(
                "LOBBY_ENDPOINT_CONSUME role=spectate result=accepted player='%s' id=%d family=%s endpoint='%s'",
                entry.name.c_str(),
                entry.playerId,
                netplay::network::FamilyName(spectateEndpoint.family),
                entry.spectateIp.c_str());
            netplay::lobby::LobbyPlayingPair selectedPair = {};
            const bool foundSelectedPair =
                TryResolvePlayingPairForLobbyEntry(lobStatus, entry, &selectedPair);

            std::string errorMessage;
            const bool started =
                TryStartWaitToSpectateAtAddress(
                    spectateEndpoint.host.c_str(),
                    spectateEndpoint.port,
                    &errorMessage);
            if (started)
            {
                ArmPendingLobbySpectateWait(
                    foundSelectedPair ? &selectedPair : nullptr,
                    entry.playerId,
                    entry.name,
                    entry.spectateIp);
                mod::Log("LobbySpectate: waiting to spectate '%s'", entry.name.c_str());
            }
            else
            {
                char text[320] = {};
                snprintf(text, sizeof(text),
                    "Wait to spectate failed for '%s'.\n\n%s",
                    entry.name.c_str(),
                    errorMessage.empty() ? "Unknown error" : errorMessage.c_str());
                ShowStubActionMessage(owner, text);
            }
            break;
        }

        if (entry.isChallenge)
        {
            // --- Accepting an incoming challenge ---
            // Safety guard: if we are in a match, do not accept challenges.
            // BuildDisplayEntries already filters them out, but guard here
            // against race conditions.
            if (g_lobbySession && g_lobbySession->IsInBattle())
            {
                mod::Log("LobbySlot: BLOCKED challenge accept from '%s' id=%d - lobby session still busy",
                    entry.name.c_str(), entry.playerId);
                ShowStubActionMessage(owner, "Cannot accept challenges\nright now.");
                break;
            }

            netplay::network::NetworkEndpoint challengeEndpoint;
            if (!netplay::network::ParseEndpoint(
                    entry.ipPort,
                    &challengeEndpoint))
            {
                mod::Log(
                    "LOBBY_ENDPOINT_CONSUME role=join result=rejected challenger='%s' id=%d raw='%s'",
                    entry.name.c_str(),
                    entry.playerId,
                    entry.ipPort.c_str());
                ShowStubActionMessage(
                    owner,
                    "The challenger published an invalid endpoint.\nExpected IPv4:port or [IPv6]:port.");
                break;
            }

            mod::Log(
                "LOBBY_ENDPOINT_CONSUME role=join result=accepted challenger='%s' id=%d family=%s endpoint='%s'",
                entry.name.c_str(),
                entry.playerId,
                netplay::network::FamilyName(challengeEndpoint.family),
                entry.ipPort.c_str());

            // Validate and queue the local connection before marking the lobby
            // challenge accepted. A synchronous family/address rejection must
            // not leave the lobby busy or send PreAccept without a P2P start.
            const bool writeNicknameToIni = ShouldWriteNicknameToRevivalIniForSessionStart();
            const bool started = netplay::bridge::StartSession(
                NetbridgeRole::Join,
                challengeEndpoint.port,
                challengeEndpoint.host.c_str(),
                g_netplayMenuState.nickname.c_str(),
                writeNicknameToIni);
            if (started)
            {
                // Send pre_accept + accept via the lobby session (async on its
                // poll thread) immediately after the local start is queued.
                g_lobbySession->AcceptChallenge(
                    entry.playerId,
                    entry.name);
                ActivateChallengeJoiningOverlay(
                    entry.name.c_str(),
                    challengeEndpoint.host.c_str(),
                    challengeEndpoint.port);
            }
            else
            {
                const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
                char text[320] = {};
                snprintf(text, sizeof(text),
                    "Accept failed for '%s'.\n\n%s",
                    entry.name.c_str(),
                    bridgeStatus.errorMsg[0] != '\0' ? bridgeStatus.errorMsg : "Unknown error");
                ShowStubActionMessage(owner, text);
            }
        }
        else
        {
            // --- Challenging an idle player (we become host) ---
            // Safety guard: if we are in a match, do not send challenges.
            if (g_lobbySession && g_lobbySession->IsInBattle())
            {
                mod::Log("LobbySlot: BLOCKED challenge send to '%s' id=%d - lobby session still busy",
                    entry.name.c_str(), entry.playerId);
                ShowStubActionMessage(owner, "Cannot send challenges\nright now.");
                break;
            }

            const std::string publicIp = lobStatus.publicIp;
            if (publicIp.empty())
            {
                if (!lobStatus.publicIpDiscoveryComplete)
                {
                    ShowStubActionMessage(
                        owner,
                        "Detecting your public address.\nPlease wait a moment and try again.");
                }
                else
                {
                    ShowHostStartupFailure(
                        "A public address could not be detected. Check your connection and try again.",
                        false);
                }
                break;
            }

            netplay::network::NetworkHost publicHost;
            if (!netplay::network::ParseBareHost(
                    publicIp,
                    &publicHost)
                || publicHost.family != lobStatus.effectiveFamily
                || !netplay::network::IsGloballyRoutableHost(publicHost))
            {
                mod::Log(
                    "LOBBY_ENDPOINT_PUBLISH result=rejected preferred=%s effective=%s raw='%s' port=%u",
                    netplay::network::FamilyName(lobStatus.preferredFamily),
                    netplay::network::FamilyName(lobStatus.effectiveFamily),
                    publicIp.c_str(),
                    static_cast<unsigned>(g_netplayMenuState.hostPort));
                ShowHostStartupFailure(
                    "The detected public address cannot be used for hosting.",
                    false);
                break;
            }

            mod::Log(
                "LOBBY_ENDPOINT_PUBLISH result=pending_listener_ack "
                "target='%s' id=%d preferred=%s effective=%s fallback=%d "
                "publicAddress='%s' requestedPort=%u",
                entry.name.c_str(),
                entry.playerId,
                netplay::network::FamilyName(lobStatus.preferredFamily),
                netplay::network::FamilyName(lobStatus.effectiveFamily),
                lobStatus.usedFamilyFallback ? 1 : 0,
                publicHost.host.c_str(),
                static_cast<unsigned>(
                    g_netplayMenuState.hostPort));

            // Start hosting first so we're ready to accept connections.
            const bool writeNicknameToIni = ShouldWriteNicknameToRevivalIniForSessionStart();
            netplay::bridge::HostSessionNetworkConfig networkConfig;
            networkConfig.preferredFamily =
                lobStatus.preferredFamily;
            networkConfig.effectiveFamily =
                lobStatus.effectiveFamily;
            networkConfig.publicAddress = publicHost.host;
            netplay::bridge::HostStartFailure startFailure =
                netplay::bridge::HostStartFailure::None;
            const bool started = netplay::bridge::StartHostSession(
                g_netplayMenuState.hostPort,
                g_netplayMenuState.nickname.c_str(),
                networkConfig,
                writeNicknameToIni,
                &startFailure);
            if (started)
            {
                ActivateChallengeHostingOverlay(
                    entry.name.c_str(),
                    g_netplayMenuState.hostPort,
                    lobStatus.preferredFamily,
                    lobStatus.effectiveFamily,
                    lobStatus.usedFamilyFallback,
                    publicHost.host.c_str());
                g_hostingOverlay.writeNicknameToIni =
                    writeNicknameToIni;
                CopyBoundedText(
                    g_hostingOverlay.hostNickname,
                    sizeof(g_hostingOverlay.hostNickname),
                    g_netplayMenuState.nickname.c_str());
                g_pendingLobbyChallengePublish.active = true;
                g_pendingLobbyChallengePublish.targetPlayerId = entry.playerId;
                g_pendingLobbyChallengePublish.targetName = entry.name;
                g_pendingLobbyChallengePublish.publicAddress =
                    publicHost.host;
                g_pendingLobbyChallengePublish.preferredFamily =
                    lobStatus.preferredFamily;
                g_pendingLobbyChallengePublish.family =
                    lobStatus.effectiveFamily;
                g_pendingLobbyChallengePublish.port =
                    g_netplayMenuState.hostPort;
                netplay::battle_log::EnsureGameplayOverlayHook();
            }
            else if (startFailure
                         == netplay::bridge::HostStartFailure::
                             FamilyUnavailable)
            {
                const netplay::network::NetworkFamily retryFamily =
                    AlternateHostFamily(lobStatus.effectiveFamily);
                mod::Log(
                    "HOST_FAMILY_FALLBACK_BEGIN owner=lobby_challenge "
                    "trigger=sync_preflight attempt=2 originalPreferred=%s "
                    "from=%s to=%s port=%u target='%s' id=%d",
                    netplay::network::FamilyName(
                        lobStatus.preferredFamily),
                    netplay::network::FamilyName(
                        lobStatus.effectiveFamily),
                    netplay::network::FamilyName(retryFamily),
                    static_cast<unsigned>(
                        g_netplayMenuState.hostPort),
                    entry.name.c_str(),
                    entry.playerId);
                netplay::bridge::CancelSession(
                    "host_sync_preflight_retry_ack");
                BeginHostDiscovery(
                    g_netplayMenuState.hostPort,
                    lobStatus.preferredFamily,
                    retryFamily,
                    false,
                    true,
                    g_netplayMenuState.nickname.c_str(),
                    writeNicknameToIni,
                    HostDiscoveryOwner::LobbyChallenge,
                    entry.playerId,
                    entry.name.c_str());
            }
            else
            {
                const netplay::bridge::NetbridgeStatus bridgeStatus =
                    netplay::bridge::GetStatus();
                ShowHostStartupFailure(
                    bridgeStatus.errorMsg[0] != '\0'
                        ? bridgeStatus.errorMsg
                        : "Could not start the Revival netplay session.",
                    true);
            }
        }
        break;
    }
    default:
        break;
    }
}

char UpdateNetplayMenu(uint32_t screenContext)
{
    ++g_netplayUpdateCallCount;
    auto const render = GetOriginalTitleRender();
    auto const processInput = reinterpret_cast<ProcessPlayerInputFn>(RuntimeAddress(kVaProcessPlayerInput));

    PumpLobbySessionShutdown();
    PromoteCompletedLocalNetworkCapabilityScan();
    AdvanceMenuSlideTransition(screenContext);
    if (g_useRuntimeTextOverlay)
    {
        (void)RenderNetplayMenuRuntimeText(screenContext);
    }
    else if (g_netplayMenuState.useConfigStyleRender)
    {
        (void)RenderNetplayMenuConfigStyle(screenContext);
    }
    else
    {
        (void)render(screenContext);
    }

    const int gameSystem = GetGameSystem(screenContext);
    processInput(reinterpret_cast<int*>(gameSystem));
    netplay::bridge::Tick();
    PumpDirectHostDiscovery();
    PumpHostListenerObservation();
    auto* const rawInputBytes = reinterpret_cast<uint8_t*>(gameSystem);
    std::array<uint8_t, kFilteredMenuInputBytes> menuInputBytes = {};
    std::memcpy(menuInputBytes.data(), rawInputBytes, kFilteredMenuInputBytes);
    MaybeRefreshMenuControlBindings(screenContext);
    ApplyIgcrMenuControlCompatibility(gameSystem, menuInputBytes.data());
    const bool windowFocused = IsScreenWindowFocused(screenContext);
    const uint8_t* const inputBytes = FilterMenuInputsForWindowFocus(menuInputBytes.data(), windowFocused);
    auto* const selectionPtr = reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection);
    auto* const inactivityCounter = reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter);

    if (SuppressRecoveryMenuInputIfNeeded(screenContext, inputBytes, inactivityCounter))
    {
        return 0;
    }

    InputSnapshot currentSnapshot = {
        static_cast<int8_t>(inputBytes[12]),
        static_cast<int8_t>(inputBytes[14]),
        inputBytes[16],
        inputBytes[18],
        static_cast<int8_t>(inputBytes[13]),
        static_cast<int8_t>(inputBytes[15]),
        inputBytes[17],
        inputBytes[19],
    };
    const bool joinWaitToSpectatePressed = ConsumeJoinWaitToSpectateHotkeyEdge(inputBytes);

    if (!g_hasLoggedInputSnapshot
        || std::memcmp(&currentSnapshot, &g_lastInputSnapshot, sizeof(InputSnapshot)) != 0)
    {
        mod::Log(
            "NetplayInput: P1(h=%d v=%d c=%u b=%u) P2(h=%d v=%d c=%u b=%u)",
            static_cast<int>(currentSnapshot.p1Horizontal),
            static_cast<int>(currentSnapshot.p1Vertical),
            static_cast<unsigned>(currentSnapshot.p1Confirm),
            static_cast<unsigned>(currentSnapshot.p1Cancel),
            static_cast<int>(currentSnapshot.p2Horizontal),
            static_cast<int>(currentSnapshot.p2Vertical),
            static_cast<unsigned>(currentSnapshot.p2Confirm),
            static_cast<unsigned>(currentSnapshot.p2Cancel));
        g_lastInputSnapshot = currentSnapshot;
        g_hasLoggedInputSnapshot = true;
    }

    const DWORD nowTick = GetTickCount();
    if (g_lastNetplayFrameLogTick == 0 || nowTick - g_lastNetplayFrameLogTick >= kNetplayFrameLogIntervalMs)
    {
        const int currentRow = GetRenderRowForSelection(static_cast<int>(*selectionPtr));
        const int nativeSlideYRaw = *reinterpret_cast<int*>(screenContext + kOffsetSlideAnimationY);
        const int nativeSlideYScaled = GetScaledNativeSlideY(screenContext);
        mod::Log(
            "NetplayFrame: updates=%llu menu=%s selection=%d row=%d(%s) inactivity=%u slide=%d nativeSlideRaw=%d nativeSlideScaled=%d",
            static_cast<unsigned long long>(g_netplayUpdateCallCount),
            MenuIdToString(g_netplayMenuState.menuId),
            static_cast<int>(*selectionPtr),
            currentRow,
            RowIndexToString(currentRow),
            *inactivityCounter,
            IsMenuSlideTransitionActive() ? 1 : 0,
            nativeSlideYRaw,
            nativeSlideYScaled);
        g_lastNetplayFrameLogTick = nowTick;
    }

    if (IsMenuSlideTransitionActive())
    {
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    // Lobby: rebuild dynamic entries each frame so that the visible row count
    // tracks the actual number of display entries (challenges + idle).
    // Also clamp the scroll offset and sync optionCount / backIndex.
    if (g_netplayMenuState.menuId == NetplayMenuId::Lobby)
    {
        int displayCount = 0;
        int playingCount = 0;
        if (g_lobbySession)
        {
            const auto lobSt = g_lobbySession->GetStatus();
            if (lobSt.pollState == netplay::lobby::PollState::Polling)
            {
                displayCount = static_cast<int>(lobSt.displayEntries.size());
                playingCount = static_cast<int>(lobSt.playing.size());
                UpdateLobbyChallengeNotificationState(screenContext, lobSt);
            }
            else
            {
                ResetLobbyChallengeNotificationState();
            }
        }
        else
        {
            ResetLobbyChallengeNotificationState();
        }
        RebuildLobbyMenuEntries(displayCount, playingCount);

        // Clamp scroll so we never point past the end of the display list.
        const int visSlots = std::min(displayCount, netplay::menu::kLobbyMaxDisplayPlayers);
        const int maxScroll = std::max(0, displayCount - visSlots);
        if (g_netplayMenuState.lobbyScrollOffset > maxScroll)
        {
            g_netplayMenuState.lobbyScrollOffset = maxScroll;
        }

        // Sync optionCount / backIndex from the freshly rebuilt spec.
        const int newCount = GetCurrentMenuEntryCount();
        if (newCount != g_netplayMenuState.optionCount && newCount > 0)
        {
            g_netplayMenuState.optionCount = newCount;
            g_netplayMenuState.backIndex   = newCount - 1;
            const int clampedSel = ClampSelectionToCurrentMenu(static_cast<int>(*selectionPtr));
            *selectionPtr = static_cast<int8_t>(clampedSel);
        }

        if (g_deferredLobbyRefreshPending && g_lobbySession)
        {
            const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
            const NetbridgePhase bridgePhase = static_cast<NetbridgePhase>(bridgeStatus.phase);
            const bool terminalPhase =
                bridgePhase == NetbridgePhase::Idle
                || bridgePhase == NetbridgePhase::Failed
                || bridgePhase == NetbridgePhase::SessionEnded;
            const bool timeoutElapsed =
                g_deferredLobbyRefreshDeadlineTick != 0
                && static_cast<int32_t>(GetTickCount() - g_deferredLobbyRefreshDeadlineTick) >= 0;
            if (terminalPhase || timeoutElapsed)
            {
                g_deferredLobbyRefreshPending = false;
                g_deferredLobbyRefreshDeadlineTick = 0;
                g_lobbySession->RequestRefresh();
                mod::Log(
                    "LobbyReturn: deferred refresh released phase=%s timeout=%d selection=%d inactivity=%u",
                    netplay::bridge::PhaseToString(bridgePhase),
                    timeoutElapsed ? 1 : 0,
                    static_cast<int>(*selectionPtr),
                    *inactivityCounter);
            }
        }
    }

    // Lobby: allow R key to trigger an immediate poll refresh.  If we are
    // still inside the deferred-refresh window, pressing R force-releases
    // the gate so the user has a manual escape hatch when the bridge is
    // slow to reach a terminal phase.
    if (g_netplayMenuState.menuId == NetplayMenuId::Lobby
        && g_lobbySession
        && ConsumeWindowFocusedHotkeyEdge(windowFocused, 'R'))
    {
        if (g_deferredLobbyRefreshPending)
        {
            g_deferredLobbyRefreshPending = false;
            g_deferredLobbyRefreshDeadlineTick = 0;
            mod::Log("LobbyReturn: deferred refresh released by R-key (manual override)");
        }
        g_lobbySession->RequestRefresh();
    }

    const int entryCount = GetCurrentMenuEntryCount();
    if (entryCount <= 0)
    {
        SwitchToMenu(screenContext, NetplayMenuId::Main, -1);
        return 0;
    }

    // Public-address discovery runs before the bridge owns a session, so the
    // normal Connecting-phase cancel handler cannot protect this window.
    // Treat it as a modal: permit cancellation and suppress all menu actions
    // until the main thread either queues the session or discards the result.
    if (g_hostingOverlay.active
        && g_hostingOverlay.discoveryInProgress
        && !g_hostingOverlay.sessionQueued)
    {
        bool cancelRequested = ConsumeNetplayEscapeEdge();
        for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
        {
            if (inputBytes[playerIndex + 18] == 1)
            {
                cancelRequested = true;
            }
        }
        if (cancelRequested)
        {
            PlayUiSound(screenContext, kSfxConfirm);
            const char* discoveryOwner =
                g_pendingHostDiscovery
                        && g_pendingHostDiscovery->owner
                            == HostDiscoveryOwner::LobbyChallenge
                    ? "lobby_challenge"
                    : "direct_host";
            mod::Log(
                "PUBLIC_IP_DISCOVERY_CANCEL owner=%s generation=%llu "
                "preferred=%s effective=%s automaticRetry=%d",
                discoveryOwner,
                static_cast<unsigned long long>(
                    g_directHostDiscoveryGeneration),
                netplay::network::FamilyName(
                    g_hostingOverlay.preferredFamily),
                netplay::network::FamilyName(
                    g_hostingOverlay.effectiveFamily),
                g_hostingOverlay.automaticFamilyRetryAttempted
                    ? 1
                    : 0);
            ResetHostingOverlayState();
        }
        *reinterpret_cast<uint8_t*>(
            screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(
            screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    if (windowFocused && HandleInlineEditInput(screenContext, inputBytes))
    {
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        *inactivityCounter = 0;
        return 0;
    }

    // NOTE: the old text "debug overlay" (toggled by the in-game D button) is
    // removed - it collided with the hosting overlay's D button (accept/minimize)
    // and is superseded by the ImGui debug panel (toggled with backslash, see
    // netplay::debug_overlay). HandleDebugOverlayInput is no longer called.

    if (windowFocused
        && g_netplayMenuState.menuId == NetplayMenuId::Options
        && netplay::options::HandleInput(screenContext, inputBytes, inactivityCounter, &g_netplayEscapeDown))
    {
        return 0;
    }

    if (windowFocused
        && g_netplayMenuState.menuId == NetplayMenuId::PlayerRooms
        && netplay::player_rooms::HandleInput(screenContext, inputBytes, inactivityCounter))
    {
        return 0;
    }

    if (windowFocused
        && g_netplayMenuState.menuId == NetplayMenuId::BattleLog
        && netplay::battle_log::HandleInput(screenContext, inputBytes, inactivityCounter))
    {
        return 0;
    }

    // --- Join menu: C button = paste IP:port from clipboard ---
    if (windowFocused
        && g_netplayMenuState.menuId == NetplayMenuId::Join
        && !g_inlineEditState.active
        && !g_stopHostingConfirm.active)
    {
        for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
        {
            if (inputBytes[playerIndex + 20] == 1)
            {
                const HWND owner = reinterpret_cast<HWND>(
                    *reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
                std::string clipText;
                if (netplay::input::TryReadClipboardAsciiText(owner, &clipText))
                {
                    netplay::network::NetworkEndpoint endpoint;
                    if (netplay::network::ParseEndpoint(clipText, &endpoint))
                    {
                        g_netplayMenuState.joinAddress = endpoint.host;
                        g_netplayMenuState.joinPort = endpoint.port;
                        SaveNetplayJoinAddressToIni();
                        PlayUiSound(screenContext, kSfxConfirm);
                        std::string canonicalEndpoint;
                        (void)netplay::network::FormatEndpoint(
                            endpoint,
                            &canonicalEndpoint);
                        mod::Log(
                            "REMOTE_ENDPOINT_PARSED source=clipboard family=%s endpoint=%s",
                            netplay::network::FamilyName(endpoint.family),
                            canonicalEndpoint.c_str());
                    }
                    else
                    {
                        SetNetplayStatusMessage(
                            "Invalid endpoint. Use IPv4:port or [IPv6]:port.");
                        mod::Log(
                            "JoinPaste: rejected endpoint='%s' expected=IPv4:port_or_[IPv6]:port",
                            clipText.c_str());
                    }
                }
                break;
            }
        }
    }

    if (windowFocused
        && !g_inlineEditState.active
        && !g_stopHostingConfirm.active
        && joinWaitToSpectatePressed)
    {
        const int shortcutSelection =
            ClampSelectionToCurrentMenu(static_cast<int>(*selectionPtr));
        const NetplayMenuEntry* shortcutEntry = GetCurrentMenuEntry(shortcutSelection);
        const bool canSpectateIp =
            g_netplayMenuState.menuId == NetplayMenuId::Join
            || (g_netplayMenuState.menuId == NetplayMenuId::Main
                && shortcutEntry != nullptr
                && shortcutEntry->action == NetplayMenuAction::OpenJoin);
        if (canSpectateIp)
        {
            if (RequestStopHostingConfirmIfHosting(
                    screenContext,
                    NetplayMenuAction::JoinSpectateIp,
                    shortcutSelection))
            {
                *inactivityCounter = 0;
                return 0;
            }
            if (IsTransientNetplayOverlayActive())
            {
                mod::Log(
                    "WaitToSpectate: ignored Join D hotkey while transient overlay active joining=%d hosting=%d delay=%d spectateConfirm=%d",
                    g_joiningOverlay.active ? 1 : 0,
                    g_hostingOverlay.active ? 1 : 0,
                    g_delaySetupOverlay.active ? 1 : 0,
                    g_spectateConfirmOverlay.active ? 1 : 0);
            }
            else
            {
                (void)StartJoinSpectateIpAction(screenContext, true);
                *inactivityCounter = 0;
                return 0;
            }
        }
    }

    const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
    const NetbridgePhase bridgePhase = static_cast<NetbridgePhase>(bridgeStatus.phase);
    const bool bridgeDelaySetupReady = bridgeStatus.delaySetupReady != 0;
    auto notifyLobbySessionEndedForCurrentBridgeRole = [&](bool preserveSpectateUntilRefresh) {
        if (!g_lobbySession)
        {
            return;
        }
        if (bridgeStatus.roleFlag == kRoleFlagSpectate)
        {
            g_lobbySession->NotifyEndSpectate(preserveSpectateUntilRefresh);
        }
        else
        {
            g_lobbySession->NotifyEndMatch();
        }
    };
    auto promoteTerminalBridgeFailureToJoiningOverlay = [&](const netplay::bridge::NetbridgeStatus& status, NetbridgePhase phase) {
        if (g_joiningOverlay.active
            || (g_hostingOverlay.active
                && g_hostingOverlay.failed))
        {
            return;
        }
        if (!g_delaySetupOverlay.active && !g_spectateConfirmOverlay.active && !g_hostingOverlay.active)
        {
            return;
        }

        const bool hadDelayOverlay = g_delaySetupOverlay.active;
        const bool hadSpectateConfirmOverlay = g_spectateConfirmOverlay.active;
        const bool hadHostingOverlay = g_hostingOverlay.active;
        const char* failureText =
            status.errorMsg[0] != '\0'
                ? status.errorMsg
                : (phase == NetbridgePhase::SessionEnded ? "Session ended" : "Unknown error");

        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        if (hadHostingOverlay)
        {
            netplay::bridge::async_host::Reset();
            ShowHostStartupFailure(failureText, true);
            mod::Log(
                "NetplayTerminal: promoted transient overlay to host failure dialog "
                "phase=%s delay=%d spectateConfirm=%d hosting=1 "
                "needsBridgeCancel=1 error='%s'",
                netplay::bridge::PhaseToString(phase),
                hadDelayOverlay ? 1 : 0,
                hadSpectateConfirmOverlay ? 1 : 0,
                failureText);
            return;
        }
        ResetHostingOverlayState();
        ResetJoiningOverlayState();
        g_joiningOverlay.active = true;
        g_joiningOverlay.failed = true;
        strncpy_s(g_joiningOverlay.errorText, sizeof(g_joiningOverlay.errorText), failureText, _TRUNCATE);
        g_joiningOverlay.errorText[sizeof(g_joiningOverlay.errorText) - 1] = '\0';

        mod::Log(
            "NetplayTerminal: promoted transient overlay to failure dialog phase=%s delay=%d spectateConfirm=%d hosting=%d error='%s'",
            netplay::bridge::PhaseToString(phase),
            hadDelayOverlay ? 1 : 0,
            hadSpectateConfirmOverlay ? 1 : 0,
            hadHostingOverlay ? 1 : 0,
            g_joiningOverlay.errorText);
    };
    auto abortPendingTransitionIfSessionLost = [&](int nextState, const char* abortReason) {
        const netplay::bridge::NetbridgeStatus latestStatus = netplay::bridge::GetStatus();
        const NetbridgePhase latestPhase = static_cast<NetbridgePhase>(latestStatus.phase);
        if (latestPhase == NetbridgePhase::Failed || latestPhase == NetbridgePhase::SessionEnded)
        {
            mod::Log(
                "NetplayTransition: ABORT - bridge entered terminal phase=%s before state=%d error='%s', re-entering netplay menu",
                netplay::bridge::PhaseToString(latestPhase),
                nextState,
                latestStatus.errorMsg[0] != '\0' ? latestStatus.errorMsg : "");
            netplay::bridge::CancelSession(abortReason);
            notifyLobbySessionEndedForCurrentBridgeRole(true);
            ReenterNetplayMenuAfterSessionAbort(screenContext, abortReason);
            return true;
        }
        if (!netplay::bridge::IsPeerProcessAlive())
        {
            mod::Log(
                "NetplayTransition: ABORT - peer exited before state=%d, re-entering netplay menu",
                nextState);
            netplay::bridge::CancelSession(abortReason);
            notifyLobbySessionEndedForCurrentBridgeRole(true);
            ReenterNetplayMenuAfterSessionAbort(screenContext, abortReason);
            return true;
        }
        return false;
    };

    if (g_lobbySession && g_lobbySession->ConsumeAbandonedOutgoingChallenge())
    {
        mod::Log(
            "LobbyChallenge: target left lobby before connect, canceling local session phase=%s",
            netplay::bridge::PhaseToString(bridgePhase));
        SetNetplayStatusMessage("Challenge canceled: player left lobby.");
        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        ResetHostingOverlayState();
        ResetJoiningOverlayState();
        if (bridgePhase == NetbridgePhase::Connecting || bridgePhase == NetbridgePhase::DelaySetup)
        {
            netplay::bridge::CancelSession("challenge_target_left_lobby");
        }
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    if (g_lobbySession && g_lobbySession->ConsumeAbandonedIncomingChallenge())
    {
        mod::Log(
            "LobbyChallenge: challenger left lobby before connect, canceling local accept phase=%s",
            netplay::bridge::PhaseToString(bridgePhase));
        SetNetplayStatusMessage("Challenger left the lobby.");
        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        ResetHostingOverlayState();
        ResetJoiningOverlayState();
        if (bridgePhase == NetbridgePhase::Connecting || bridgePhase == NetbridgePhase::DelaySetup)
        {
            netplay::bridge::CancelSession("challenger_left_lobby");
        }
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    if (TryBeginTerminalHostFamilyRetry(
            bridgeStatus,
            bridgePhase))
    {
        *reinterpret_cast<uint8_t*>(
            screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(
            screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    if (g_hostingOverlay.active
        && !g_hostingOverlay.failed
        && netplay::bridge::async_host::
               HasHostListenerStartupFailed())
    {
        const char* failureText =
            bridgeStatus.errorMsg[0] != '\0'
                ? bridgeStatus.errorMsg
                : "Hosting could not start. Please try again.";
        ShowHostStartupFailure(
            failureText,
            HostFailureNeedsBridgeCancel());
        mod::Log(
            "ASYNC_HOST_LISTENER_START_FAILURE_PROMOTED "
            "phase=%s needsBridgeCancel=%d error='%s'",
            netplay::bridge::PhaseToString(bridgePhase),
            g_hostingOverlay.failureNeedsBridgeCancel
                ? 1
                : 0,
            failureText);
    }

    if (bridgePhase == NetbridgePhase::Failed || bridgePhase == NetbridgePhase::SessionEnded)
    {
        promoteTerminalBridgeFailureToJoiningOverlay(bridgeStatus, bridgePhase);
    }

    if (g_hostingOverlay.active && g_hostingOverlay.failed)
    {
        bool dismissed = ConsumeNetplayEscapeEdge();
        for (int playerIndex = 0;
             !dismissed && playerIndex < 2;
             ++playerIndex)
        {
            if (inputBytes[playerIndex + 16] == 1
                || inputBytes[playerIndex + 18] == 1
                || inputBytes[playerIndex + 20] == 1
                || inputBytes[playerIndex + 22] == 1)
            {
                dismissed = true;
            }
        }
        if (dismissed)
        {
            const bool needsBridgeCancel =
                g_hostingOverlay.failureNeedsBridgeCancel;
            const std::string failureText =
                g_hostingOverlay.errorText;
            PlayUiSound(screenContext, kSfxConfirm);
            ResetDelaySetupOverlayState();
            ResetSpectateConfirmOverlayState();
            ResetHostingOverlayState();
            ResetJoiningOverlayState();
            ClearPendingLobbySpectateWait(
                "dismissed_host_error");
            netplay::bridge::async_host::Reset();
            if (needsBridgeCancel)
            {
                netplay::bridge::CancelSession(
                    "dismissed_host_error");
                notifyLobbySessionEndedForCurrentBridgeRole(
                    false);
            }
            mod::Log(
                "HostingOverlay: error dismissed by user "
                "needsBridgeCancel=%d action=%s error='%s'",
                needsBridgeCancel ? 1 : 0,
                needsBridgeCancel
                    ? "bridge_cancel_or_rejection_ack"
                    : "overlay_only",
                failureText.c_str());
        }
        *reinterpret_cast<uint8_t*>(
            screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(
            screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    if (g_pendingLobbySpectateWait.active
        && g_lobbySession
        && bridgeStatus.roleFlag == kRoleFlagSpectate
        && (bridgePhase == NetbridgePhase::Connecting
            || bridgePhase == NetbridgePhase::DelaySetup
            || bridgePhase == NetbridgePhase::Connected)
        && !IsPendingLobbySpectateWaitStillValid())
    {
        mod::Log(
            "LobbySpectateWait: tracked playing pair disappeared before handoff, canceling spectate wait phase=%s hostIp=%s pair='%s' vs '%s'",
            netplay::bridge::PhaseToString(bridgePhase),
            g_pendingLobbySpectateWait.hostIp.c_str(),
            g_pendingLobbySpectateWait.p1Name.c_str(),
            g_pendingLobbySpectateWait.p2Name.c_str());
        SetNetplayStatusMessage("Wait to spectate canceled: players stopped playing.");
        ClearPendingLobbySpectateWait("pair_left_playing");
        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        ResetHostingOverlayState();
        ResetJoiningOverlayState();
        netplay::bridge::CancelSession("spectate_pair_left_lobby_playing");
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    // --- Fast handoff ---
    // If the Revival rollback engine is already in sync state (vsHumanSyncReady)
    // and delay setup was never shown, transition immediately. Delay-confirmed
    // sessions below use the 17-frame title warmup so we don't preempt their
    // vanilla title-navigation tail.
    if (bridgeStatus.vsHumanSyncReady != 0
        && (bridgePhase == NetbridgePhase::Connected
            || bridgePhase == NetbridgePhase::DelaySetup)
        && !g_delaySetupOverlay.active)
    {
        mod::Log(
            "FastHandoff: vsHumanSyncReady detected, immediate transition phase=%s sync(mode=%d flag1084=%d session=%d)",
            netplay::bridge::PhaseToString(bridgePhase),
            bridgeStatus.syncGameMode,
            bridgeStatus.syncMode0Flag1084,
            bridgeStatus.syncSessionByte);
        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        HandoffConnectedSessionToVsHumanState(screenContext);
        if (g_pendingGlobalStateTransition >= 0)
        {
            const int nextState = g_pendingGlobalStateTransition;
            g_pendingGlobalStateTransition = -1;
            // Pre-flight: if the helper died or the bridge already entered a
            // terminal failure/session-ended phase during the handoff/fade-out,
            // abort the state transition. Without this check, EFZ.exe's
            // state-1 init calls the DLL rollback tick immediately, which can
            // fire ExitProcess on the main game thread where no setjmp recovery
            // point is active.
            if (abortPendingTransitionIfSessionLost(nextState, "peer_died_before_transition"))
            {
                return 0;
            }
            mod::Log("NetplayTransition: returning global state=%d from netplay menu", nextState);
            return static_cast<char>(nextState);
        }
        return 0;
    }

    // --- Spectate confirm overlay ---
    // The spectate confirm prompt fires during Connecting when the host is
    // already mid-match.  We must handle it BEFORE the delay-setup check so
    // that the user can accept/decline before Revival continues.
    // Consider the spectate-confirm prompt resolved if:
    //  - serials match (normal path), OR
    //  - localInitApplied is set (aux auto-answered before overlay tracking,
    //    e.g. JoinSpectate from a lobby session).
    const bool spectateConfirmPending =
        (bridgePhase == NetbridgePhase::Connecting
            || bridgePhase == NetbridgePhase::DelaySetup
            || bridgePhase == NetbridgePhase::Connected)
        &&
        bridgeStatus.spectateConfirmPromptSerial > 0
        && bridgeStatus.spectateConfirmPromptServedSerial < bridgeStatus.spectateConfirmPromptSerial
        && bridgeStatus.localInitApplied == 0;
    const bool autoJoinSpectatePrompt =
        spectateConfirmPending
        && bridgeStatus.role == static_cast<int>(NetbridgeRole::JoinSpectate)
        && bridgeStatus.spectateConfirmPromptKind == static_cast<int>(NetbridgeSpectatePromptKind::HostAlreadyPlaying);
    if (autoJoinSpectatePrompt)
    {
        const uint64_t promptFingerprint =
            (static_cast<uint64_t>(bridgeStatus.processId) << 32)
            | static_cast<uint32_t>(bridgeStatus.spectateConfirmPromptSerial);
        if (g_lastAutoAnsweredSpectatePromptFingerprint != promptFingerprint)
        {
            const bool answered = netplay::bridge::AnswerSpectatePromptChoice(1);
            mod::Log(
                "JoinSpectate: auto-answering host-already-playing prompt with Yes result=%d promptSerial=%d pid=%u",
                answered ? 1 : 0,
                bridgeStatus.spectateConfirmPromptSerial,
                static_cast<unsigned>(bridgeStatus.processId));
            if (answered)
            {
                g_lastAutoAnsweredSpectatePromptFingerprint = promptFingerprint;
            }
        }
    }
    if (spectateConfirmPending
        && !autoJoinSpectatePrompt
        && !g_spectateConfirmOverlay.active)
    {
        ActivateSpectateConfirmOverlay(bridgeStatus.spectateConfirmPromptKind);
    }

    const bool joiningOverlayShouldShowWaitForGameBegin =
        g_joiningOverlay.active
        && g_joiningOverlay.spectateMode
        && bridgePhase == NetbridgePhase::Connecting
        && bridgeStatus.spectateConfirmPromptKind
            == static_cast<int>(NetbridgeSpectatePromptKind::HostNotYetPlaying)
        && bridgeStatus.spectateConfirmPromptSerial > 0
        && bridgeStatus.spectateConfirmPromptServedSerial >= bridgeStatus.spectateConfirmPromptSerial;
    if (g_joiningOverlay.waitingForGameBegin != joiningOverlayShouldShowWaitForGameBegin)
    {
        g_joiningOverlay.waitingForGameBegin = joiningOverlayShouldShowWaitForGameBegin;
        mod::Log(
            "JoiningOverlay: waiting_for_game_begin=%d phase=%s promptSerial=%d served=%d role=%d",
            g_joiningOverlay.waitingForGameBegin ? 1 : 0,
            netplay::bridge::PhaseToString(bridgePhase),
            bridgeStatus.spectateConfirmPromptSerial,
            bridgeStatus.spectateConfirmPromptServedSerial,
            bridgeStatus.role);
    }

    if (g_spectateConfirmOverlay.active)
    {
        if (bridgePhase == NetbridgePhase::Failed || bridgePhase == NetbridgePhase::SessionEnded)
        {
            mod::Log(
                "SpectateConfirmOverlay: dismissed due to terminal bridge phase=%s error='%s'",
                netplay::bridge::PhaseToString(bridgePhase),
                bridgeStatus.errorMsg[0] != '\0' ? bridgeStatus.errorMsg : "");
            ResetSpectateConfirmOverlayState();
        }
        else if (!spectateConfirmPending)
        {
            // Prompt was resolved externally (aux auto-answer / init applied).
            // Dismiss the overlay so it doesn't block the handoff.
            mod::Log("SpectateConfirmOverlay: auto-dismissed (prompt resolved externally)");
            ResetSpectateConfirmOverlayState();
        }
        else
        {
            (void)HandleSpectateConfirmOverlayInput(screenContext, inputBytes, inactivityCounter);
            return 0;
        }
    }

    // --- Spectate handoff ---
    // Revival's spectator replay consumes recorded title-navigation frames
    // before character select.  Native 1.02h/1.02j traces spend frames 1-16
    // in state 0 and reach state 1 on frame 17; switching immediately makes
    // the local spectator one screen ahead of the recorded stream.
    const bool spectateHandoffReady =
        bridgeStatus.roleFlag == kRoleFlagSpectate
        && bridgeStatus.localInitApplied != 0
        && (bridgePhase == NetbridgePhase::Connecting
            || bridgePhase == NetbridgePhase::DelaySetup
            || bridgePhase == NetbridgePhase::Connected)
        && !spectateConfirmPending;
    if (!spectateHandoffReady)
    {
        ResetSpectateHandoffWarmup("conditions_lost");
    }
    if (spectateHandoffReady)
    {
        if (!g_spectateHandoffWarmupActive)
        {
            g_spectateHandoffWarmupActive = true;
            g_spectateHandoffWarmupFrames = 0;
            g_spectateHandoffWarmupStartTick = GetTickCount();
            ClearPendingLobbySpectateWait("spectate_handoff_warmup");
            ResetDelaySetupOverlayState();
            ResetSpectateConfirmOverlayState();
            mod::Log(
                "SpectateHandoffWarmup: armed targetFrames=%d role=%d init=%d phase=%s "
                "sync(mode=%d flag1084=%d session=%d flags=%d/%d) "
                "confirmSerial=%d/%d screen=%d menuSel=%d peerAlive=%d",
                kSpectateTitleWarmupFrames,
                bridgeStatus.roleFlag,
                bridgeStatus.localInitApplied,
                netplay::bridge::PhaseToString(bridgePhase),
                bridgeStatus.syncGameMode,
                bridgeStatus.syncMode0Flag1084,
                bridgeStatus.syncSessionByte,
                bridgeStatus.syncGlobalFlag4964,
                bridgeStatus.syncGlobalFlag4965,
                bridgeStatus.spectateConfirmPromptSerial,
                bridgeStatus.spectateConfirmPromptServedSerial,
                *reinterpret_cast<const int*>(kVaCurrentScreenIndex),
                static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)),
                netplay::bridge::IsPeerProcessAlive() ? 1 : 0);
        }

        ++g_spectateHandoffWarmupFrames;
        if (g_spectateHandoffWarmupFrames < kSpectateTitleWarmupFrames)
        {
            ClearLocalMenuControlState(screenContext);
            ++(*inactivityCounter);
            return 0;
        }

        const uint32_t elapsedMs =
            g_spectateHandoffWarmupStartTick != 0
                ? GetTickCount() - g_spectateHandoffWarmupStartTick
                : 0;
        mod::Log(
            "SpectateHandoffWarmup: complete frames=%d target=%d elapsedMs=%lu "
            "screen=%d menuSel=%d",
            g_spectateHandoffWarmupFrames,
            kSpectateTitleWarmupFrames,
            static_cast<unsigned long>(elapsedMs),
            *reinterpret_cast<const int*>(kVaCurrentScreenIndex),
            static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)));
        HandoffSpectateSession(screenContext);
        if (g_pendingGlobalStateTransition >= 0)
        {
            const int nextState = g_pendingGlobalStateTransition;
            g_pendingGlobalStateTransition = -1;
            if (abortPendingTransitionIfSessionLost(nextState, "peer_died_before_spectate_transition"))
            {
                return 0;
            }
            mod::Log(
                "NetplayTransition: returning spectate state=%d from netplay menu "
                "menuSel=%d screen=%d",
                nextState,
                static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)),
                *reinterpret_cast<const int*>(kVaCurrentScreenIndex));
            return static_cast<char>(nextState);
        }
        return 0;
    }

    if ((bridgePhase == NetbridgePhase::DelaySetup
            || bridgePhase == NetbridgePhase::Connected
            || bridgePhase == NetbridgePhase::Connecting)
        && bridgeDelaySetupReady
        && !netplay::bridge::async_host::ShouldSuppressDelayOverlay())
    {
        const netplay::bridge::DelayPromptMetrics promptMetrics = netplay::bridge::GetDelayPromptMetrics();
        if (!g_delaySetupOverlay.active)
        {
            ActivateDelaySetupOverlay(bridgeStatus);

            // Notify the lobby that the P2P connection was established so it
            // can send the deferred 'accept' and transition to "playing".
            if (g_lobbySession)
            {
                mod::Log("DelaySetupOverlay: notifying lobby - setting inBattle=true (P2P match path)");
                g_lobbySession->NotifyMatchConnected();
            }
        }
        else
        {
            if (bridgeStatus.pingMs >= 0)
            {
                g_delaySetupOverlay.pingMs = bridgeStatus.pingMs;
            }
            if (bridgeStatus.p1Name[0] != '\0' && bridgeStatus.p2Name[0] != '\0')
            {
                CopyBoundedText(g_delaySetupOverlay.p1Name, sizeof(g_delaySetupOverlay.p1Name), bridgeStatus.p1Name);
                CopyBoundedText(g_delaySetupOverlay.p2Name, sizeof(g_delaySetupOverlay.p2Name), bridgeStatus.p2Name);
            }
            else
            {
                g_delaySetupOverlay.p1Name[0] = '\0';
                g_delaySetupOverlay.p2Name[0] = '\0';
            }
            if (promptMetrics.serial > 0)
            {
                if (promptMetrics.averagePingMs >= 0)
                {
                    g_delaySetupOverlay.pingMs = promptMetrics.averagePingMs;
                }
                if (promptMetrics.minDelay >= kDelaySelectionMin && promptMetrics.minDelay <= kDelaySelectionMax)
                {
                    g_delaySetupOverlay.minDelay = promptMetrics.minDelay;
                }
                if (promptMetrics.maxDelay >= kDelaySelectionMin && promptMetrics.maxDelay <= kDelaySelectionMax)
                {
                    g_delaySetupOverlay.maxDelay = promptMetrics.maxDelay;
                }
                if (g_delaySetupOverlay.maxDelay < g_delaySetupOverlay.minDelay)
                {
                    g_delaySetupOverlay.maxDelay = g_delaySetupOverlay.minDelay;
                }
                if (promptMetrics.recommendedDelay >= kDelaySelectionMin && promptMetrics.recommendedDelay <= kDelaySelectionMax)
                {
                    const int clampedRecommended =
                        (std::max)(g_delaySetupOverlay.minDelay, (std::min)(g_delaySetupOverlay.maxDelay, promptMetrics.recommendedDelay));
                    g_delaySetupOverlay.recommendedDelay = clampedRecommended;
                    if (!g_delaySetupOverlay.waitingForRuntimeReady)
                    {
                        g_delaySetupOverlay.selectedDelay =
                            (std::max)(g_delaySetupOverlay.minDelay, (std::min)(g_delaySetupOverlay.maxDelay, g_delaySetupOverlay.selectedDelay));
                    }
                }
            }
            g_delaySetupOverlay.selectedDelay =
                (std::max)(g_delaySetupOverlay.minDelay, (std::min)(g_delaySetupOverlay.maxDelay, g_delaySetupOverlay.selectedDelay));
        }

        if (g_delaySetupOverlay.waitingForRuntimeReady
            && (bridgeStatus.vsHumanSyncReady != 0
                || (bridgePhase == NetbridgePhase::Connected
                    && !netplay::bridge::RequiresNativeVsHumanSyncForHandoff())))
        {
            if (!g_vsHumanHandoffWarmupActive)
            {
                mod::Log(
                    "DelayOverlay: runtime ready for VS-human title warmup phase=%s sync(mode=%d flag1084=%d session=%d)",
                    netplay::bridge::PhaseToString(bridgePhase),
                    bridgeStatus.syncGameMode,
                    bridgeStatus.syncMode0Flag1084,
                    bridgeStatus.syncSessionByte);
            }
            const bool handoffQueued = AdvanceVsHumanHandoffWarmup(
                screenContext,
                bridgeStatus,
                bridgePhase,
                "delay_runtime_ready",
                inactivityCounter);
            if (handoffQueued && g_pendingGlobalStateTransition >= 0)
            {
                const int nextState = g_pendingGlobalStateTransition;
                g_pendingGlobalStateTransition = -1;
                if (abortPendingTransitionIfSessionLost(nextState, "peer_died_before_transition"))
                {
                    return 0;
                }
                mod::Log("NetplayTransition: returning global state=%d from netplay menu", nextState);
                return static_cast<char>(nextState);
            }
            return 0;
        }
        if (g_delaySetupOverlay.waitingForRuntimeReady)
        {
            ResetVsHumanHandoffWarmup("runtime_not_ready");
        }

        (void)HandleDelaySetupOverlayInput(screenContext, inputBytes, inactivityCounter);
        if (g_pendingGlobalStateTransition >= 0)
        {
            const int nextState = g_pendingGlobalStateTransition;
            g_pendingGlobalStateTransition = -1;
            if (abortPendingTransitionIfSessionLost(nextState, "peer_died_before_transition"))
            {
                return 0;
            }
            mod::Log("NetplayTransition: returning global state=%d from netplay menu", nextState);
            return static_cast<char>(nextState);
        }
        return 0;
    }
    else
    {
        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
    }

    // Some sessions can complete delay negotiation inside Revival without
    // presenting a prompt we can mirror. In that case, go straight to VS Human.
    if (bridgePhase == NetbridgePhase::Connected
        && !bridgeDelaySetupReady
        && bridgeStatus.vsHumanSyncReady != 0)
    {
        mod::Log(
            "NetplayTransition: connected without delay prompt; auto handoff sync(mode=%d flag1084=%d session=%d)",
            bridgeStatus.syncGameMode,
            bridgeStatus.syncMode0Flag1084,
            bridgeStatus.syncSessionByte);
        HandoffConnectedSessionToVsHumanState(screenContext);
        if (g_pendingGlobalStateTransition >= 0)
        {
            const int nextState = g_pendingGlobalStateTransition;
            g_pendingGlobalStateTransition = -1;
            if (abortPendingTransitionIfSessionLost(nextState, "peer_died_before_transition"))
            {
                return 0;
            }
            mod::Log("NetplayTransition: returning global state=%d from netplay menu", nextState);
            return static_cast<char>(nextState);
        }
        return 0;
    }

    if (bridgePhase == NetbridgePhase::Failed || bridgePhase == NetbridgePhase::SessionEnded)
    {
        if (g_joiningOverlay.active && !g_joiningOverlay.failed)
        {
            // First frame of failure - populate the overlay with the error
            g_joiningOverlay.failed = true;
            if (bridgeStatus.errorMsg[0] != '\0')
            {
                strncpy_s(g_joiningOverlay.errorText, sizeof(g_joiningOverlay.errorText),
                    bridgeStatus.errorMsg, _TRUNCATE);
                g_joiningOverlay.errorText[sizeof(g_joiningOverlay.errorText) - 1] = '\0';
            }
            else
            {
                strncpy_s(g_joiningOverlay.errorText, sizeof(g_joiningOverlay.errorText),
                    bridgePhase == NetbridgePhase::SessionEnded ? "Session ended" : "Unknown error",
                    _TRUNCATE);
            }
            mod::Log("JoiningOverlay: connection failed - %s", g_joiningOverlay.errorText);
        }

        // While the joining overlay is showing the error, wait for any button press to dismiss
        if (g_joiningOverlay.active && g_joiningOverlay.failed)
        {
            bool dismissed = ConsumeNetplayEscapeEdge();
            for (int playerIndex = 0; !dismissed && playerIndex < 2; ++playerIndex)
            {
                // Check confirm (A), cancel (B), heavy (C), or direction
                if (inputBytes[playerIndex + 16] == 1  // A
                    || inputBytes[playerIndex + 18] == 1  // B
                    || inputBytes[playerIndex + 20] == 1  // C
                    || inputBytes[playerIndex + 22] == 1) // D
                {
                    dismissed = true;
                }
            }
            if (dismissed)
            {
                PlayUiSound(screenContext, kSfxConfirm);
                ResetJoiningOverlayState();
                ResetHostingOverlayState();
                ClearPendingLobbySpectateWait("dismissed_error");
                netplay::bridge::CancelSession("dismissed_error");
                notifyLobbySessionEndedForCurrentBridgeRole(false);
                mod::Log("JoiningOverlay: error dismissed by user");
            }
            *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
            *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
            ++(*inactivityCounter);
            return 0;
        }

        if (netplay::bridge::recovery::WasGameplayExitRecoveryCompleted())
        {
            if (kEnableGameplayExitRecoveryRenderDiagnostics)
            {
                const DWORD suppressTick = GetTickCount();
                if (g_lastRecoveryNoOverlaySuppressedTick == 0
                    || suppressTick - g_lastRecoveryNoOverlaySuppressedTick >= 1000u)
                {
                    mod::Log(
                        "RECOVERY_MENU_SUPPRESS_NO_OVERLAY_SESSION_ENDED origin=%s phase=%s completed=1",
                        netplay::bridge::recovery::CurrentGameplayExitRecoveryOrigin(),
                        netplay::bridge::PhaseToString(bridgePhase));
                    g_lastRecoveryNoOverlaySuppressedTick = suppressTick;
                }
            }
        }
        else
        {
            // No joining overlay active - just reset and fall through to idle menu
            mod::Log("UpdateNetplayMenu: session ended with no overlay active, resetting (inBattle will be cleared)");
            ResetHostingOverlayState();
            ResetJoiningOverlayState();
            ClearPendingLobbySpectateWait("session_ended");
            if (bridgeStatus.errorMsg[0] != '\0')
            {
                SetNetplayStatusMessage(bridgeStatus.errorMsg, 3200);
            }
            netplay::bridge::CancelSession("no_overlay_session_ended");
            notifyLobbySessionEndedForCurrentBridgeRole(false);
        }
    }

    // "Stop hosting?" confirm modal - intercepts all input while active.
    if (g_stopHostingConfirm.active)
    {
        bool cancel = ConsumeNetplayEscapeEdge();
        bool confirm = false;
        bool anyModalControlDown = false;
        for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
        {
            auto* const latch = reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1 + playerIndex);
            const int8_t horizontal = static_cast<int8_t>(inputBytes[playerIndex + 12]);
            const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);
            const bool axisDown = horizontal != 0 || vertical != 0;
            const bool confirmDown = inputBytes[playerIndex + 16] != 0;
            const bool cancelDown = inputBytes[playerIndex + 18] != 0;
            anyModalControlDown |= axisDown || confirmDown || cancelDown;

            if (g_stopHostingConfirm.waitingForInputRelease)
            {
                // Consume the button/axis state that opened the modal. Keeping
                // the game's axis latch in sync prevents a held direction from
                // becoming a fresh menu press on the next frame.
                *latch = axisDown ? 1u : 0u;
            }
            else if (axisDown)
            {
                if (*latch == 0)
                {
                    g_stopHostingConfirm.selection = (g_stopHostingConfirm.selection == 0) ? 1 : 0;
                    PlayUiSound(screenContext, kSfxMove);
                }
                *latch = 1;
            }
            else
            {
                *latch = 0;
            }

            if (!g_stopHostingConfirm.waitingForInputRelease
                && confirmDown
                && g_stopHostingConfirm.confirmDown[playerIndex] == 0)
            {
                confirm = true;
            }
            if (!g_stopHostingConfirm.waitingForInputRelease
                && cancelDown
                && g_stopHostingConfirm.cancelDown[playerIndex] == 0)
            {
                cancel = true;
            }
            g_stopHostingConfirm.confirmDown[playerIndex] = confirmDown ? 1u : 0u;
            g_stopHostingConfirm.cancelDown[playerIndex] = cancelDown ? 1u : 0u;
        }

        if (g_stopHostingConfirm.waitingForInputRelease && !cancel)
        {
            if (!anyModalControlDown)
            {
                g_stopHostingConfirm.waitingForInputRelease = false;
                mod::Log("StopHostingConfirm: input released; modal press edges armed");
            }
            ++(*inactivityCounter);
            return 0;
        }

        if (cancel)
        {
            g_stopHostingConfirm.active = false;
            PlayUiSound(screenContext, kSfxConfirm);
            mod::Log("StopHostingConfirm: cancelled (keep hosting)");
        }
        else if (confirm)
        {
            const bool stopHosting = (g_stopHostingConfirm.selection == 0);
            const netplay::menu::NetplayMenuAction pending = g_stopHostingConfirm.pendingAction;
            const int pendingSel = g_stopHostingConfirm.pendingLogicalSelection;
            g_stopHostingConfirm.active = false;
            PlayUiSound(screenContext, kSfxConfirm);
            if (stopHosting)
            {
                mod::Log("StopHostingConfirm: stopping host, proceeding to action=%d",
                    static_cast<int>(pending));
                netplay::bridge::async_host::Reset();
                netplay::bridge::CancelSession("stop_hosting_for_menu");
                ResetHostingOverlayState();
                notifyLobbySessionEndedForCurrentBridgeRole(false);
                // Re-dispatch the deferred navigation now that hosting is gone.
                ExecuteNetplayAction(screenContext, pending, pendingSel);
            }
            else
            {
                mod::Log("StopHostingConfirm: keep hosting selected");
            }
        }

        ++(*inactivityCounter);
        return 0;
    }

    // While async hosting is MINIMIZED, do not run the connecting/hosting
    // overlay input handler - let normal menu navigation work so the user can
    // browse Battle Log / Options while the host listener stays alive (a small
    // badge shows the hosting state). Selecting HOST again un-minimizes.
    if ((bridgePhase == NetbridgePhase::Connecting || bridgePhase == NetbridgePhase::DelaySetup)
        && !netplay::bridge::async_host::IsMinimized())
    {
        bool cancelRequested = ConsumeNetplayEscapeEdge();
        for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
        {
            if (inputBytes[playerIndex + 18] == 1)
            {
                cancelRequested = true;
                break;
            }
            if (g_joiningOverlay.active && inputBytes[playerIndex + 22] == 1)
            {
                cancelRequested = true;
                mod::Log(
                    "JoiningOverlay: D/back pressed during active connect flow player=%d roleFlag=%d phase=%s",
                    playerIndex,
                    bridgeStatus.roleFlag,
                    netplay::bridge::PhaseToString(bridgePhase));
                break;
            }
        }

        // Async hosting: once a peer has connected and the delay prompt is being
        // held, accept AUTOMATICALLY as soon as the full hosting overlay is on
        // screen - we only reach here when NOT minimized. The user no longer
        // presses D to accept; releasing the hold makes the normal delay-setup
        // overlay activate next frame. (While minimized the hold persists and a
        // badge shows "OPPONENT FOUND!"; returning to HOST un-minimizes, lands
        // here, and auto-accepts.) RequestAccept is idempotent - it only acts on
        // the PeerFoundHeld -> Accepted edge, so the sound plays once.
        if (!cancelRequested && netplay::bridge::async_host::IsPeerFoundHeld())
        {
            netplay::bridge::async_host::RequestAccept();
            PlayUiSound(screenContext, kSfxConfirm);
        }

        // Async hosting: while waiting for a peer (no prompt held yet), the D
        // button (offset 22/23) MINIMIZES - collapse the hosting overlay to a
        // small badge and drop back to the Main netplay menu while the host
        // listener stays alive, so the user can browse Battle Log / Options /
        // etc. The hosting overlay (kept active) renders as a badge while
        // minimized; selecting HOST again restores the full overlay.
        if (!cancelRequested
            && g_hostingOverlay.active
            && !g_hostingOverlay.challengeMode
            && g_hostingOverlay.listenerReady
            && netplay::bridge::async_host::GetState()
                   == netplay::bridge::async_host::State::Hosting)
        {
            bool minimizeRequested = false;
            for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
            {
                if (inputBytes[playerIndex + 22] == 1)
                {
                    minimizeRequested = true;
                    break;
                }
            }
            if (minimizeRequested)
            {
                netplay::bridge::async_host::SetMinimized(true);
                PlayUiSound(screenContext, kSfxConfirm);
                mod::Log("AsyncHost: minimized to Main menu badge, host session kept alive");
                // Stay in the netplay menu - go to Main. Keep g_hostingOverlay
                // active so it renders as a badge (DrawHostingOverlayGdi checks
                // async_host::IsMinimized()). Do NOT cancel the session.
                SwitchToMenu(screenContext, NetplayMenuId::Main, g_netplayMenuState.mainSelection);
                *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
                *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
                return 0;
            }
        }

        // C button (heavy attack, offset 20/21) - copy IP:PORT to clipboard
        if (g_hostingOverlay.active
            && !g_hostingOverlay.challengeMode
            && g_hostingOverlay.listenerReady
            && g_hostingOverlay.ipFetchDone
            && !g_hostingOverlay.ipFetchFailed)
        {
            bool copyRequested = false;
            for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
            {
                if (inputBytes[playerIndex + 20] == 1)
                {
                    copyRequested = true;
                    break;
                }
            }
            if (copyRequested)
            {
                const std::string clipText = FormatNetworkEndpoint(
                    g_hostingOverlay.effectiveFamily,
                    g_hostingOverlay.publicIp,
                    g_hostingOverlay.port);
                if (!clipText.empty()
                    && netplay::bridge::takeover::TryWriteClipboardAscii(
                        clipText.c_str()))
                {
                    g_hostingOverlay.copiedToClipboard = true;
                    g_hostingOverlay.copiedFlashTick = GetTickCount();
                    PlayUiSound(screenContext, kSfxConfirm);
                    mod::Log(
                        "HostingOverlay: copied endpoint='%s' family=%s",
                        clipText.c_str(),
                        netplay::network::FamilyName(
                            g_hostingOverlay.effectiveFamily));
                }
            }
        }

        if (cancelRequested)
        {
            PlayUiSound(screenContext, kSfxConfirm);
            ResetHostingOverlayState();
            ResetJoiningOverlayState();
            ClearPendingLobbySpectateWait("user_cancel");
            netplay::bridge::async_host::Reset();
            netplay::bridge::CancelSession("user_cancel");
            notifyLobbySessionEndedForCurrentBridgeRole(false);
            mod::Log("NetplayBridge: cancel requested during connecting");
        }

        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    if (ConsumeNetplayEscapeEdge())
    {
        PlayUiSound(screenContext, kSfxConfirm);
        mod::Log(
            "NetplayCancel: keyboard=ESC menu=%s selection=%d",
            MenuIdToString(g_netplayMenuState.menuId),
            static_cast<int>(*selectionPtr));

        if (g_netplayMenuState.menuId == NetplayMenuId::Main)
        {
            LeaveNetplayMenu(screenContext);
        }
        else if (g_netplayMenuState.menuId == NetplayMenuId::BattleLog
            && netplay::battle_log::HandleCancel(screenContext))
        {
        }
        else if (g_netplayMenuState.menuId == NetplayMenuId::PlayerRooms
            && netplay::player_rooms::HandleCancel(screenContext))
        {
        }
        else
        {
            const NetplayMenuId targetMenu = ResolveCancelTargetMenu();
            if (targetMenu == NetplayMenuId::PlayerRooms)
            {
                StartMenuSlideTransition(screenContext, targetMenu, -1, -1);
            }
            else
            {
                StartMenuSlideTransition(screenContext, targetMenu, g_netplayMenuState.mainSelection, -1);
            }
        }
        return 0;
    }

    bool hadDirectionalInput = false;

    for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
    {
        auto* const inputLatch = reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1 + playerIndex);
        const int8_t vertical = static_cast<int8_t>(inputBytes[playerIndex + 14]);

        if (vertical != 0)
        {
            hadDirectionalInput = true;
            *inactivityCounter = 0;
            if (*inputLatch == 0)
            {
                PlayUiSound(screenContext, kSfxMove);
                const int current = ClampSelectionToCurrentMenu(static_cast<int>(*selectionPtr));
                const int delta = vertical > 0 ? 1 : -1;
                int next = (current + delta + entryCount) % entryCount;

                if (g_netplayMenuState.menuId == NetplayMenuId::Options)
                {
                    int optionsNext = next;
                    if (netplay::options::HandleVerticalNavigation(current, delta, &optionsNext))
                    {
                        next = optionsNext;
                    }
                }

                // Lobby: intercept boundary movement to scroll the display list
                // instead of wrapping when more entries exist off-screen.
                if (g_netplayMenuState.menuId == NetplayMenuId::Lobby && g_lobbySession)
                {
                    const auto lobSt = g_lobbySession->GetStatus();
                    const int displayCount = static_cast<int>(lobSt.displayEntries.size());
                    const int visSlots  = std::min(displayCount, netplay::menu::kLobbyMaxDisplayPlayers);

                    if (delta > 0 && current == visSlots - 1 && next == visSlots)
                    {
                        // Moving down from the last visible slot: scroll if more
                        // entries are below, otherwise fall through to LobbyPlaying0.
                        const int maxScroll = std::max(0, displayCount - visSlots);
                        if (g_netplayMenuState.lobbyScrollOffset < maxScroll)
                        {
                            g_netplayMenuState.lobbyScrollOffset++;
                            next = current; // stay at last visible slot
                        }
                    }
                    else if (delta < 0 && current == 0 && next == entryCount - 1)
                    {
                        // Moving up from the first slot: scroll back if possible,
                        // otherwise fall through to BackToMain (wrap).
                        if (g_netplayMenuState.lobbyScrollOffset > 0)
                        {
                            g_netplayMenuState.lobbyScrollOffset--;
                            next = current; // stay at first slot
                        }
                    }
                }

                const NetplayMenuEntry* nextEntry = GetCurrentMenuEntry(next);
                mod::Log(
                    "NetplaySelection: player=%d menu=%s from=%d to=%d row=%d(%s) label=%s inputV=%d",
                    playerIndex,
                    MenuIdToString(g_netplayMenuState.menuId),
                    current,
                    next,
                    nextEntry != nullptr ? nextEntry->renderRow : -1,
                    nextEntry != nullptr ? RowIndexToString(nextEntry->renderRow) : "ROW_UNKNOWN",
                    nextEntry != nullptr ? nextEntry->debugLabel : "none",
                    static_cast<int>(vertical));
                *selectionPtr = static_cast<int8_t>(next);
                *reinterpret_cast<uint16_t*>(screenContext + kOffsetMenuAnimCounter) = 0;
                *inputLatch = 1;
                g_lastLoggedSelection = *selectionPtr;
            }
        }
        else
        {
            *inputLatch = 0;
        }

        if (inputBytes[playerIndex + 16] == 1)
        {
            const int logicalSelection = ClampSelectionToCurrentMenu(static_cast<int>(*selectionPtr));
            const NetplayMenuEntry* selectedEntry = GetCurrentMenuEntry(logicalSelection);
            if (selectedEntry == nullptr)
            {
                return 0;
            }

            PlayUiSound(screenContext, kSfxConfirm);
            mod::Log(
                "NetplayConfirm: player=%d menu=%s selection=%d row=%d(%s) label=%s action=%s",
                playerIndex,
                MenuIdToString(g_netplayMenuState.menuId),
                logicalSelection,
                selectedEntry->renderRow,
                RowIndexToString(selectedEntry->renderRow),
                selectedEntry->debugLabel,
                MenuActionToString(selectedEntry->action));
            ExecuteNetplayAction(screenContext, selectedEntry->action, logicalSelection);
            return 0;
        }

        if (inputBytes[playerIndex + 18] == 1)
        {
            PlayUiSound(screenContext, kSfxConfirm);
            mod::Log(
                "NetplayCancel: player=%d menu=%s selection=%d",
                playerIndex,
                MenuIdToString(g_netplayMenuState.menuId),
                static_cast<int>(*selectionPtr));

            if (g_netplayMenuState.menuId == NetplayMenuId::Main)
            {
                LeaveNetplayMenu(screenContext);
            }
            else if (g_netplayMenuState.menuId == NetplayMenuId::BattleLog
                && netplay::battle_log::HandleCancel(screenContext))
            {
            }
            else if (g_netplayMenuState.menuId == NetplayMenuId::PlayerRooms
                && netplay::player_rooms::HandleCancel(screenContext))
            {
            }
            else
            {
                const NetplayMenuId targetMenu = ResolveCancelTargetMenu();
                if (targetMenu == NetplayMenuId::PlayerRooms)
                {
                    StartMenuSlideTransition(screenContext, targetMenu, -1, -1);
                }
                else
                {
                    StartMenuSlideTransition(screenContext, targetMenu, g_netplayMenuState.mainSelection, -1);
                }
            }
            return 0;
        }
    }

    if (!hadDirectionalInput)
    {
        ++(*inactivityCounter);
    }

    return 0;
}

void TriggerNetplayMenuEntry(uint32_t screenContext)
{
    mod::Log(
        "TriggerNetplayMenuEntry: titleSelection=%d screenContext=0x%08X",
        static_cast<int>(*reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection)),
        screenContext);
    PlayUiSound(screenContext, kSfxConfirm);
    EnterNetplayMenu(screenContext);
}
} // namespace netplay::hooks::internal
