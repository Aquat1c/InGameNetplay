#include "netplay/hooks/internal/shared.h"
#include "netplay/assets/assets.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/battle_log_menu.h"
#include "netplay/core/input_utils.h"
#include "netplay/core/mod_settings.h"
#include "netplay/core/options_menu.h"
#include "netplay/core/player_rooms_menu.h"
#include "netplay/core/tls_http_client.h"

#include "logger.h"

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif

#include <dinput.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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
constexpr int kConnectedPreHandoffDelayFrames = 18;
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
constexpr int kScreenIndexReplay = 8;           // EFZ screen table index for replay (used by spectate)
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
constexpr uint32_t kDeferredLobbyRefreshTimeoutMs = 3000;
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
    g_delaySetupOverlay = {};
}

void ResetSpectateConfirmOverlayState()
{
    g_spectateConfirmOverlay = {};
}

} // namespace (close anonymous to expose hosting overlay functions)

// ---------------------------------------------------------------------------
// Hosting overlay — shows "Hosting on IP:PORT" while waiting for a client.
// A background thread fetches the public IPv4 from api4.ipify.org.
// ---------------------------------------------------------------------------
void ResetHostingOverlayState()
{
    g_hostingOverlay = {};
}

static bool TryFetchIpFromUrl(const char* url, const char* label)
{
    constexpr uint32_t kTimeoutMs = 5000;
    std::string body;
    std::string error;
    // verifyPeer=false: the embedded mbedTLS build has no CA root store,
    // so certificate verification always fails.  This request only fetches
    // a plain-text public IP address — no sensitive data.
    if (!netplay::tls::HttpGet(url, false, kTimeoutMs, &body, &error))
    {
        mod::Log("HostingOverlay: %s fetch failed: %s", label, error.c_str());
        return false;
    }

    // Trim whitespace / newlines from the response
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' '))
    {
        body.pop_back();
    }
    if (!body.empty() && body.size() < sizeof(g_hostingOverlay.publicIp))
    {
        std::memcpy(g_hostingOverlay.publicIp, body.c_str(), body.size() + 1);
        mod::Log("HostingOverlay: %s = %s", label, g_hostingOverlay.publicIp);
        return true;
    }
    mod::Log("HostingOverlay: %s bad response body size=%zu", label, body.size());
    return false;
}

static void FetchPublicIpThread()
{
    // Try IPv4 first (api4.ipify.org), fall back to IPv6 (api6.ipify.org)
    // if the ISP doesn't support IPv4.
    if (TryFetchIpFromUrl("https://api4.ipify.org", "public IPv4"))
    {
        g_hostingOverlay.ipFetchFailed = false;
        g_hostingOverlay.ipFetchDone = true;
        return;
    }
    mod::Log("HostingOverlay: IPv4 unavailable, trying IPv6 fallback...");
    if (TryFetchIpFromUrl("https://api6.ipify.org", "public IPv6"))
    {
        g_hostingOverlay.ipFetchFailed = false;
        g_hostingOverlay.ipFetchDone = true;
        return;
    }
    g_hostingOverlay.ipFetchFailed = true;
    g_hostingOverlay.ipFetchDone = true;
    mod::Log("HostingOverlay: all IP detection methods failed");
}

void ActivateHostingOverlay(uint16_t port)
{
    ResetHostingOverlayState();
    g_hostingOverlay.active = true;
    g_hostingOverlay.port = port;
    mod::Log("HostingOverlay: activated port=%u, starting IP fetch",
        static_cast<unsigned>(port));

    // Fire-and-forget background thread for the blocking HTTP request.
    std::thread(FetchPublicIpThread).detach();
}

void ActivateChallengeHostingOverlay(const char* targetName, uint16_t port)
{
    ResetHostingOverlayState();
    g_hostingOverlay.active = true;
    g_hostingOverlay.port = port;
    g_hostingOverlay.challengeMode = true;
    strncpy_s(g_hostingOverlay.targetName, sizeof(g_hostingOverlay.targetName), targetName, _TRUNCATE);
    g_hostingOverlay.targetName[sizeof(g_hostingOverlay.targetName) - 1] = '\0';
    mod::Log(
        "HostingOverlay: activated challenge target='%s' port=%u",
        g_hostingOverlay.targetName,
        static_cast<unsigned>(port));
}

// ---------------------------------------------------------------------------
// Joining overlay — shows "Connecting to IP:PORT ..." while connecting.
// Transitions to delay setup on success, or shows the error on failure.
// ---------------------------------------------------------------------------
void ResetJoiningOverlayState()
{
    g_joiningOverlay = {};
}

void ActivateJoiningOverlay(const char* address, uint16_t port)
{
    ResetJoiningOverlayState();
    g_joiningOverlay.active = true;
    g_joiningOverlay.port = port;
    strncpy_s(g_joiningOverlay.address, sizeof(g_joiningOverlay.address), address, _TRUNCATE);
    g_joiningOverlay.address[sizeof(g_joiningOverlay.address) - 1] = '\0';
    mod::Log("JoiningOverlay: activated  target=%s:%u", address, static_cast<unsigned>(port));
}

void ActivateChallengeJoiningOverlay(const char* targetName, const char* address, uint16_t port)
{
    ResetJoiningOverlayState();
    g_joiningOverlay.active = true;
    g_joiningOverlay.port = port;
    g_joiningOverlay.displayTargetName = true;
    strncpy_s(g_joiningOverlay.address, sizeof(g_joiningOverlay.address), address, _TRUNCATE);
    g_joiningOverlay.address[sizeof(g_joiningOverlay.address) - 1] = '\0';
    strncpy_s(g_joiningOverlay.targetName, sizeof(g_joiningOverlay.targetName), targetName, _TRUNCATE);
    g_joiningOverlay.targetName[sizeof(g_joiningOverlay.targetName) - 1] = '\0';
    mod::Log(
        "JoiningOverlay: activated challenge target='%s' address=%s:%u",
        g_joiningOverlay.targetName,
        g_joiningOverlay.address,
        static_cast<unsigned>(port));
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
        mod::Log(
            "WaitToSpectate: started spectate session -> %s:%u",
            menuState.joinAddress.c_str(),
            static_cast<unsigned>(menuState.joinPort));
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
        mod::Log(
            "WaitToSpectate: started spectate session -> %s:%u",
            address,
            static_cast<unsigned>(port));
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
    DisarmSpectateReplayBypass();
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
    const bool displayTargetName = g_joiningOverlay.displayTargetName;
    const std::string targetName = g_joiningOverlay.targetName;

    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
    ResetHostingOverlayState();
    ResetJoiningOverlayState();
    ClearPendingLobbySpectateWait("spectate_prompt_join");
    DisarmSpectateReplayBypass();
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
            "SpectateConfirmOverlay: restarting pending spectate as Join target=%s:%u started=1",
            address.c_str(),
            static_cast<unsigned>(port));
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
        "SpectateConfirmOverlay: restarting pending spectate as Join failed target=%s:%u error='%s'",
        address.c_str(),
        static_cast<unsigned>(port),
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
        // If accepted, the session continues — Revival will proceed to delay
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
        "DelayOverlay: activated promptSerial=%d ping=%d current=%d min=%d max=%d recommended=%d names='%s' vs '%s'",
        promptMetrics.serial,
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
        mod::Log(
            "DelayOverlay: confirmed player=%d selected=%d recommended=%d min=%d max=%d ping=%d phase=%s syncReady=%d",
            confirmPlayer,
            selectedDelay,
            g_delaySetupOverlay.recommendedDelay,
            g_delaySetupOverlay.minDelay,
            g_delaySetupOverlay.maxDelay,
            g_delaySetupOverlay.pingMs,
            netplay::bridge::PhaseToString(phaseAfterApply),
            statusAfterApply.vsHumanSyncReady);

        const bool readyForHandoff =
            phaseAfterApply == NetbridgePhase::Connected || statusAfterApply.vsHumanSyncReady != 0;
        if (!readyForHandoff)
        {
            g_delaySetupOverlay.waitingForRuntimeReady = true;
            g_delaySetupOverlay.vsHumanSyncArmed = false;
            g_delaySetupOverlay.nextHandoffRetryTick = 0;
            g_delaySetupOverlay.errorMessage[0] = '\0';
            mod::Log(
                "DelayOverlay: waiting for runtime sync after delay selection phase=%s prompt=%d/%d",
                netplay::bridge::PhaseToString(phaseAfterApply),
                statusAfterApply.delayPromptSerial,
                statusAfterApply.delayPromptServedSerial);
            return true;
        }

        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        HandoffConnectedSessionToVsHumanState(screenContext);
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
            // Read the current exit flag BEFORE we clear it — diagnostic.
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

                // The reinit does NOT reset grid col/row — those are only set
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
// PrepareSpectateReplayState — set the title screen's menu selection to
// "Replay" (4) so the Revival DLL's mode-transition detector recognises the
// spectate context.  The DLL checks EFZ_Mode0_ReadFlag1084() == 4 which is
// byte 1084 (0x43C) of the mode-0 screen object — the menu selection field.
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
// HandoffSpectateSession — transition directly to Character Select (mode 1)
// for spectating.  The DLL creates a client session (type 1) for spectating
// which uses shared-memory IPC and input replay.  The client session
// survives all mode transitions (no watcher is created — the DLL's watcher
// creation path requires session type 2 which the mod never uses).  Going
// directly to charselect allows the client session's input replay to drive
// the character selection from the host's captured inputs.
// ---------------------------------------------------------------------------
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

    RunTransitionFadeOut(screenContext, 0, 0);
    PrepareSpectateReplayState(screenContext);

    // The spectator's charselect screen requires the same game-system state
    // as online VS Human: mode 4, CPU flags 0, rounds, and a clean
    // charselect init/exit flag pair.  The DLL's spectator never sets these
    // — it relies on the host EXE having the right state.  Without this
    // call the spectator enters charselect with stale flags (wrong mode,
    // possibly CPU players, stale exit flag) which causes silent desync.
    PrepareVsHumanGameState(screenContext);

    // Post-preparation verification.
    // The DLL client session (type 1, dword_100A05D0=1) uses shared memory
    // IPC and input replay.  The DLL frame hook's spectate watcher creation
    // requires dword_100A05D0==2 (practice session) which never matches the
    // mod's spectate sessions.  Therefore no watcher is ever created and the
    // client session survives through all mode transitions.  We transition
    // directly to charselect (mode 1) — bypassing the replay screen (mode 8)
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
        mod::Log("HandoffSpectateSession: notifying lobby — entering spectate lifecycle");
        g_lobbySession->NotifySpectateStarted();
    }

    // Transition directly to charselect (mode 1).  The DLL's client
    // session (type 1) survives mode transitions and drives the game via
    // input replay from shared memory — the mode-8 replay screen detour
    // is unnecessary because the DLL's watcher creation path only triggers
    // for session type 2 (dword_100A05D0==2), not the mod's type 1.
    // Going directly to charselect avoids wasting shared-memory input
    // frames on the unused replay screen.
    g_pendingGlobalStateTransition = kScreenIndexCharSelect;

    // Immediately publish the post-handoff state so inNetplayMenu=0 is visible
    // to export consumers before the next frame hook fires.
    netplay::bridge::TickExportOnly();

    mod::Log(
        "HandoffSpectateSession: queued global transition nextState=%d returnToNetplay=%d",
        g_pendingGlobalStateTransition,
        g_returnToNetplayAfterMatch ? 1 : 0);
}

void EnterNetplayMenu(uint32_t screenContext, bool skipFadeOut)
{
    mod::Log("EnterNetplayMenu: request active=%d skipFadeOut=%d", g_netplayMenuState.active, skipFadeOut ? 1 : 0);
    if (g_netplayMenuState.active)
    {
        mod::Log("EnterNetplayMenu: already active, ignoring duplicate entry");
        return;
    }

    if (!skipFadeOut)
    {
        RunTransitionFadeOut(screenContext, 0, 0);
    }
    else
    {
        // Disconnect recovery: the DirectDraw front/back buffers still
        // contain stale battle-scene pixels with a mismatched palette.
        // Skip the fade-out entirely — we will load fresh assets and
        // set the hardware palette before doing a clean fade-in.
        mod::Log("EnterNetplayMenu: skipping fade-out (disconnect recovery)");
    }

    if (!LoadNetplayAssets(screenContext))
    {
        mod::Log("EnterNetplayMenu: assets load failed — netplay menu disabled");
        mod::Log("EnterNetplayMenu: ensure netplay_bg.dat and netplay_ob.dat are next to the DLL or in mods\\efz_netplay_mod\\assets\\");
        g_netplayAssetsAvailable = false;
        if (!skipFadeOut)
        {
            (void)LoadTitleAssets(screenContext);
            RunTransitionFadeIn(screenContext);
        }
        return;
    }

    LoadNetplayMenuSettingsFromIni();
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
    // independent of the deferred-refresh gate — we want the End to go out
    // even if the bridge takes a while to reach a terminal phase.
    if (returnToLobby)
    {
        g_lobbySession->FlushDeferredEndOnReturn();
    }

    g_netplayMenuState.active = true;
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
        mod::Log("EnterNetplayMenu: disconnect recovery — rendered initial clean frame");
    }

    RunTransitionFadeIn(screenContext);
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

void LeaveNetplayMenu(uint32_t screenContext)
{
    PumpLobbySessionShutdown();
    mod::Log("LeaveNetplayMenu: request active=%d bgmActive=%d", g_netplayMenuState.active, g_netplayMenuState.bgmActive);
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
    DisarmSpectateReplayBypass();
    ResetDelaySetupOverlayState();
    ResetSpectateConfirmOverlayState();
    ResetHostingOverlayState();
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
    g_returnToNetplayAfterMatch = true;
    g_pendingGlobalStateTransition = kScreenIndexCharSelect;

    // Immediately publish the post-handoff state so that inNetplayMenu=0 and
    // activityPhase=InMatch are visible to export consumers before the next
    // frame hook fires (avoids a stale "menu=1" window during the fade-out).
    netplay::bridge::TickExportOnly();

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
                &joinedRoom);
        }
        else
        {
            mod::Log("SwitchToMenu: entering Lobby, creating global session for '%s' port=%u",
                g_netplayMenuState.nickname.c_str(),
                static_cast<unsigned>(g_netplayMenuState.hostPort));
            g_lobbySession = std::make_unique<netplay::lobby::LobbySession>(
                g_netplayMenuState.nickname,
                g_netplayMenuState.hostPort);
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
        StartMenuSlideTransition(screenContext, NetplayMenuId::Host, -1, +1);
        break;
    case NetplayMenuAction::OpenJoin:
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::Join, -1, +1);
        break;
    case NetplayMenuAction::OpenPlayerRooms:
        if (g_lobbySessionShutdownInFlight.load())
        {
            ShowStubActionMessage(owner, "Still leaving room.\nPlease wait.");
            break;
        }
        g_netplayMenuState.mainSelection = logicalSelection;
        StartMenuSlideTransition(screenContext, NetplayMenuId::PlayerRooms, -1, +1);
        break;
    case NetplayMenuAction::OpenLobby:
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
        const bool writeNicknameToIni = ShouldWriteNicknameToRevivalIniForSessionStart();
        const bool started = netplay::bridge::StartSession(
            NetbridgeRole::Host,
            g_netplayMenuState.hostPort,
            "",
            g_netplayMenuState.nickname.c_str(),
            writeNicknameToIni);
        if (started)
        {
            ActivateHostingOverlay(g_netplayMenuState.hostPort);
        }
        else
        {
            const netplay::bridge::NetbridgeStatus status = netplay::bridge::GetStatus();
            char text[320] = {};
            snprintf(
                text,
                sizeof(text),
                "Host start failed.\n\n%s",
                status.errorMsg[0] != '\0' ? status.errorMsg : "Unknown error");
            ShowStubActionMessage(owner, text);
        }
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
            mod::Log("LobbyPlaying0: BLOCKED spectate — lobby session still busy");
            ShowStubActionMessage(owner, "Cannot spectate right now.");
            break;
        }

        // Parse ip:port from the playing pair's hostIp field.
        std::string spectateAddr = selectedPair.hostIp;
        uint16_t spectatePort = g_netplayMenuState.joinPort;
        const size_t colonPos = spectateAddr.rfind(':');
        if (colonPos != std::string::npos && colonPos > 0 && colonPos + 1 < spectateAddr.size())
        {
            const unsigned long parsedPort = std::strtoul(spectateAddr.c_str() + colonPos + 1, nullptr, 10);
            if (parsedPort > 0 && parsedPort <= 65535)
            {
                spectatePort = static_cast<uint16_t>(parsedPort);
                spectateAddr = spectateAddr.substr(0, colonPos);
            }
        }

        mod::Log("LobbyPlaying0: parsed spectateAddr=%s spectatePort=%u",
            spectateAddr.c_str(), static_cast<unsigned>(spectatePort));

        std::string errorMessage;
        const bool started =
            TryStartWaitToSpectateAtAddress(spectateAddr.c_str(), spectatePort, &errorMessage);
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
                mod::Log("LobbySpectate: BLOCKED spectate for '%s' id=%d — lobby session still busy",
                    entry.name.c_str(), entry.playerId);
                ShowStubActionMessage(owner, "Cannot spectate right now.");
                break;
            }

            if (entry.spectateIp.empty())
            {
                ShowStubActionMessage(owner, "No host information available\nfor spectating.");
                break;
            }

            std::string spectateAddr = entry.spectateIp;
            uint16_t spectatePort = g_netplayMenuState.joinPort;
            const size_t specColonPos = spectateAddr.rfind(':');
            if (specColonPos != std::string::npos && specColonPos > 0 && specColonPos + 1 < spectateAddr.size())
            {
                const unsigned long parsedPort = std::strtoul(spectateAddr.c_str() + specColonPos + 1, nullptr, 10);
                if (parsedPort > 0 && parsedPort <= 65535)
                {
                    spectatePort = static_cast<uint16_t>(parsedPort);
                    spectateAddr = spectateAddr.substr(0, specColonPos);
                }
            }

            mod::Log("LobbySpectate: spectating '%s' id=%d via %s addr=%s port=%u",
                entry.name.c_str(), entry.playerId, entry.spectateIp.c_str(),
                spectateAddr.c_str(), static_cast<unsigned>(spectatePort));
            netplay::lobby::LobbyPlayingPair selectedPair = {};
            const bool foundSelectedPair =
                TryResolvePlayingPairForLobbyEntry(lobStatus, entry, &selectedPair);

            std::string errorMessage;
            const bool started =
                TryStartWaitToSpectateAtAddress(spectateAddr.c_str(), spectatePort, &errorMessage);
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
                mod::Log("LobbySlot: BLOCKED challenge accept from '%s' id=%d — lobby session still busy",
                    entry.name.c_str(), entry.playerId);
                ShowStubActionMessage(owner, "Cannot accept challenges\nright now.");
                break;
            }

            // Parse the challenger's ip:port into address + port for StartSession.
            std::string challengeAddr = entry.ipPort;
            uint16_t challengePort = g_netplayMenuState.hostPort;
            const size_t colonPos = entry.ipPort.rfind(':');
            if (colonPos != std::string::npos && colonPos > 0 && colonPos + 1 < entry.ipPort.size())
            {
                challengeAddr = entry.ipPort.substr(0, colonPos);
                const unsigned long parsedPort = std::strtoul(entry.ipPort.c_str() + colonPos + 1, nullptr, 10);
                if (parsedPort > 0 && parsedPort <= 65535)
                {
                    challengePort = static_cast<uint16_t>(parsedPort);
                }
            }

            mod::Log("LobbyAccept: accepting challenge from '%s' id=%d ip=%s addr=%s port=%u",
                entry.name.c_str(), entry.playerId, entry.ipPort.c_str(),
                challengeAddr.c_str(), static_cast<unsigned>(challengePort));

            // Send pre_accept + accept via the lobby session (async on poll thread).
            g_lobbySession->AcceptChallenge(entry.playerId, entry.name);

            // Connect to the challenger's address.
            const bool writeNicknameToIni = ShouldWriteNicknameToRevivalIniForSessionStart();
            const bool started = netplay::bridge::StartSession(
                NetbridgeRole::Join,
                challengePort,
                challengeAddr.c_str(),
                g_netplayMenuState.nickname.c_str(),
                writeNicknameToIni);
            if (started)
            {
                ActivateChallengeJoiningOverlay(entry.name.c_str(), challengeAddr.c_str(), challengePort);
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
                mod::Log("LobbySlot: BLOCKED challenge send to '%s' id=%d — lobby session still busy",
                    entry.name.c_str(), entry.playerId);
                ShowStubActionMessage(owner, "Cannot send challenges\nright now.");
                break;
            }

            const std::string publicIp = lobStatus.publicIp;
            if (publicIp.empty())
            {
                ShowStubActionMessage(owner, "Public IP not yet discovered.\nPlease wait a moment and try again.");
                break;
            }

            char ipPortBuf[128] = {};
            snprintf(ipPortBuf, sizeof(ipPortBuf), "%s:%u",
                publicIp.c_str(), static_cast<unsigned>(g_netplayMenuState.hostPort));

            mod::Log("LobbyChallenge: challenging '%s' id=%d with our ip=%s",
                entry.name.c_str(), entry.playerId, ipPortBuf);

            // Start hosting first so we're ready to accept connections.
            const bool writeNicknameToIni = ShouldWriteNicknameToRevivalIniForSessionStart();
            const bool started = netplay::bridge::StartSession(
                NetbridgeRole::Host,
                g_netplayMenuState.hostPort,
                "",
                g_netplayMenuState.nickname.c_str(),
                writeNicknameToIni);
            if (started)
            {
                ActivateChallengeHostingOverlay(entry.name.c_str(), g_netplayMenuState.hostPort);

                // Send the challenge to the lobby server (async on poll thread).
                g_lobbySession->SendChallenge(entry.playerId, entry.name, std::string(ipPortBuf));
            }
            else
            {
                const netplay::bridge::NetbridgeStatus bridgeStatus = netplay::bridge::GetStatus();
                char text[320] = {};
                snprintf(text, sizeof(text),
                    "Host start failed for challenge to '%s'.\n\n%s",
                    entry.name.c_str(),
                    bridgeStatus.errorMsg[0] != '\0' ? bridgeStatus.errorMsg : "Unknown error");
                ShowStubActionMessage(owner, text);
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
    auto* const rawInputBytes = reinterpret_cast<uint8_t*>(gameSystem);
    std::array<uint8_t, kFilteredMenuInputBytes> menuInputBytes = {};
    std::memcpy(menuInputBytes.data(), rawInputBytes, kFilteredMenuInputBytes);
    MaybeRefreshMenuControlBindings(screenContext);
    ApplyIgcrMenuControlCompatibility(gameSystem, menuInputBytes.data());
    const bool windowFocused = IsScreenWindowFocused(screenContext);
    const uint8_t* const inputBytes = FilterMenuInputsForWindowFocus(menuInputBytes.data(), windowFocused);
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

    auto* const selectionPtr = reinterpret_cast<int8_t*>(screenContext + kOffsetMenuSelection);
    auto* const inactivityCounter = reinterpret_cast<uint32_t*>(screenContext + kOffsetInactivityCounter);

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

    if (windowFocused && HandleInlineEditInput(screenContext, inputBytes))
    {
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        *inactivityCounter = 0;
        return 0;
    }

    // --- Debug overlay (D key) ---
    // Always poll the D key toggle before menu-specific handlers so the
    // debug overlay is truly modal once opened.
    if (windowFocused && HandleDebugOverlayInput(screenContext, inputBytes, inactivityCounter))
    {
        return 0;
    }

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
        && !g_inlineEditState.active)
    {
        const bool debugMenuEnabled = netplay::mod_settings::IsDebugMenuEnabled();
        for (int playerIndex = 0; playerIndex < 2; ++playerIndex)
        {
            if (inputBytes[playerIndex + 20] == 1)
            {
                const HWND owner = reinterpret_cast<HWND>(
                    *reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
                std::string clipText;
                if (netplay::input::TryReadClipboardAsciiText(owner, &clipText))
                {
                    // Trim whitespace
                    while (!clipText.empty() && (clipText.back() == ' ' || clipText.back() == '\n' || clipText.back() == '\r' || clipText.back() == '\t'))
                        clipText.pop_back();
                    while (!clipText.empty() && (clipText.front() == ' ' || clipText.front() == '\n' || clipText.front() == '\r' || clipText.front() == '\t'))
                        clipText.erase(clipText.begin());

                    // Split on the last ':' to handle IPv6 addresses (e.g. [::1]:7500)
                    const std::size_t colonPos = clipText.rfind(':');
                    if (colonPos != std::string::npos && colonPos > 0 && colonPos + 1 < clipText.size())
                    {
                        const std::string ipPart = clipText.substr(0, colonPos);
                        const std::string portPart = clipText.substr(colonPos + 1);
                        const unsigned long parsedPort = std::strtoul(portPart.c_str(), nullptr, 10);
                        if (parsedPort > 0 && parsedPort <= 65535 && !ipPart.empty())
                        {
                            g_netplayMenuState.joinAddress = ipPart;
                            g_netplayMenuState.joinPort = static_cast<uint16_t>(parsedPort);
                            SaveNetplayJoinAddressToIni();
                            PlayUiSound(screenContext, kSfxConfirm);
                            mod::Log("JoinPaste: pasted address='%s' port=%u from clipboard",
                                ipPart.c_str(), static_cast<unsigned>(parsedPort));
                        }
                        else
                        {
                            mod::Log("JoinPaste: invalid IP:port in clipboard '%s'", clipText.c_str());
                        }
                    }
                    else
                    {
                        mod::Log("JoinPaste: no ':' separator found in clipboard '%s'", clipText.c_str());
                    }
                }
                break;
            }
        }

        if (!debugMenuEnabled && joinWaitToSpectatePressed)
        {
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
                std::string errorMessage;
                if (TryStartWaitToSpectateFromJoinSettings(screenContext, &errorMessage))
                {
                    PlayUiSound(screenContext, kSfxConfirm);
                }
                else
                {
                    const HWND owner = reinterpret_cast<HWND>(
                        *reinterpret_cast<uint32_t*>(screenContext + kOffsetWindowHandle));
                    ShowStubActionMessage(
                        owner,
                        "Wait to spectate failed.\n\n"
                        + (errorMessage.empty() ? std::string("Unknown error") : errorMessage));
                }
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
        if (g_joiningOverlay.active)
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
    auto abortPendingTransitionIfSessionLost = [&](int nextState, const char* abortReason, bool disarmSpectateReplayBypass) {
        const netplay::bridge::NetbridgeStatus latestStatus = netplay::bridge::GetStatus();
        const NetbridgePhase latestPhase = static_cast<NetbridgePhase>(latestStatus.phase);
        if (latestPhase == NetbridgePhase::Failed || latestPhase == NetbridgePhase::SessionEnded)
        {
            mod::Log(
                "NetplayTransition: ABORT — bridge entered terminal phase=%s before state=%d error='%s', re-entering netplay menu",
                netplay::bridge::PhaseToString(latestPhase),
                nextState,
                latestStatus.errorMsg[0] != '\0' ? latestStatus.errorMsg : "");
            if (disarmSpectateReplayBypass)
            {
                DisarmSpectateReplayBypass();
            }
            netplay::bridge::CancelSession(abortReason);
            notifyLobbySessionEndedForCurrentBridgeRole(true);
            ReenterNetplayMenuAfterSessionAbort(screenContext, abortReason);
            return true;
        }
        if (!netplay::bridge::IsPeerProcessAlive())
        {
            mod::Log(
                "NetplayTransition: ABORT — peer exited before state=%d, re-entering netplay menu",
                nextState);
            if (disarmSpectateReplayBypass)
            {
                DisarmSpectateReplayBypass();
            }
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

    if (bridgePhase == NetbridgePhase::Failed || bridgePhase == NetbridgePhase::SessionEnded)
    {
        promoteTerminalBridgeFailureToJoiningOverlay(bridgeStatus, bridgePhase);
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
        DisarmSpectateReplayBypass();
        netplay::bridge::CancelSession("spectate_pair_left_lobby_playing");
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP1) = 0;
        *reinterpret_cast<uint8_t*>(screenContext + kOffsetInputLatchP2) = 0;
        ++(*inactivityCounter);
        return 0;
    }

    // --- Fast handoff ---
    // If the Revival rollback engine is already in sync state (vsHumanSyncReady)
    // and the delay overlay has been submitted (waitingForRuntimeReady) or delay
    // setup was never shown, transition immediately.  This prevents a multi-frame
    // window where the remote side is at State 1 (charselect) while we're still
    // at State 0 (title), which causes a non-rollback desync freeze.
    if (bridgeStatus.vsHumanSyncReady != 0
        && (bridgePhase == NetbridgePhase::Connected
            || bridgePhase == NetbridgePhase::DelaySetup)
        && (!g_delaySetupOverlay.active || g_delaySetupOverlay.waitingForRuntimeReady))
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
            if (abortPendingTransitionIfSessionLost(nextState, "peer_died_before_transition", false))
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
    // Spectating uses a fundamentally different path from online play:
    // the DLL expects the game to transition to screen index 8 (Replay
    // Screen), NOT screen 1 (Character Select).  The DLL's frame-hook
    // (sub_1006D810) watches for a 0→8 mode transition, verifies role==2
    // and title menu selection==4, then creates a lightweight spectator
    // watcher session.
    //
    // Unlike online play, we cannot wait for vsHumanSyncReady (which
    // requires syncGameMode==8) because the game mode will only BECOME 8
    // after we perform this transition — waiting would deadlock.  Instead,
    // we trigger the handoff as soon as DLL init is applied and any
    // spectate-confirm prompt has been resolved.
    if (bridgeStatus.roleFlag == kRoleFlagSpectate
        && bridgeStatus.localInitApplied != 0
        && (bridgePhase == NetbridgePhase::Connecting
            || bridgePhase == NetbridgePhase::DelaySetup
            || bridgePhase == NetbridgePhase::Connected)
        && !spectateConfirmPending)
    {
        mod::Log(
            "SpectateHandoff: triggered — role=%d init=%d phase=%s "
            "sync(mode=%d flag1084=%d session=%d flags=%d/%d) "
            "confirmSerial=%d/%d screen=%d menuSel=%d peerAlive=%d",
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
        ClearPendingLobbySpectateWait("spectate_handoff");
        ResetDelaySetupOverlayState();
        ResetSpectateConfirmOverlayState();
        HandoffSpectateSession(screenContext);
        if (g_pendingGlobalStateTransition >= 0)
        {
            const int nextState = g_pendingGlobalStateTransition;
            g_pendingGlobalStateTransition = -1;
            if (abortPendingTransitionIfSessionLost(nextState, "peer_died_before_spectate_transition", true))
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
        && bridgeDelaySetupReady)
    {
        const netplay::bridge::DelayPromptMetrics promptMetrics = netplay::bridge::GetDelayPromptMetrics();
        if (!g_delaySetupOverlay.active)
        {
            ActivateDelaySetupOverlay(bridgeStatus);

            // Notify the lobby that the P2P connection was established so it
            // can send the deferred 'accept' and transition to "playing".
            if (g_lobbySession)
            {
                mod::Log("DelaySetupOverlay: notifying lobby — setting inBattle=true (P2P match path)");
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
            && (bridgePhase == NetbridgePhase::Connected || bridgeStatus.vsHumanSyncReady != 0))
        {
            if (bridgeStatus.vsHumanSyncReady == 0 && bridgePhase == NetbridgePhase::Connected)
            {
                if (!g_delaySetupOverlay.connectedHandoffDelayActive)
                {
                    g_delaySetupOverlay.connectedHandoffDelayActive = true;
                    g_delaySetupOverlay.connectedHandoffDelayFramesRemaining = kConnectedPreHandoffDelayFrames;
                    mod::Log(
                        "DelayOverlay: connected pre-handoff delay started frames=%d sync(mode=%d flag1084=%d session=%d)",
                        g_delaySetupOverlay.connectedHandoffDelayFramesRemaining,
                        bridgeStatus.syncGameMode,
                        bridgeStatus.syncMode0Flag1084,
                        bridgeStatus.syncSessionByte);
                }

                if (g_delaySetupOverlay.connectedHandoffDelayFramesRemaining > 0)
                {
                    --g_delaySetupOverlay.connectedHandoffDelayFramesRemaining;
                    return 0;
                }
            }

            mod::Log(
                "DelayOverlay: runtime sync ready after delay selection phase=%s sync(mode=%d flag1084=%d session=%d)",
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
                if (abortPendingTransitionIfSessionLost(nextState, "peer_died_before_transition", false))
                {
                    return 0;
                }
                mod::Log("NetplayTransition: returning global state=%d from netplay menu", nextState);
                return static_cast<char>(nextState);
            }
            return 0;
        }

        (void)HandleDelaySetupOverlayInput(screenContext, inputBytes, inactivityCounter);
        if (g_pendingGlobalStateTransition >= 0)
        {
            const int nextState = g_pendingGlobalStateTransition;
            g_pendingGlobalStateTransition = -1;
            if (abortPendingTransitionIfSessionLost(nextState, "peer_died_before_transition", false))
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
            if (abortPendingTransitionIfSessionLost(nextState, "peer_died_before_transition", false))
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
            // First frame of failure — populate the overlay with the error
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
            mod::Log("JoiningOverlay: connection failed — %s", g_joiningOverlay.errorText);
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

        // No joining overlay active — just reset and fall through to idle menu
        mod::Log("UpdateNetplayMenu: session ended with no overlay active, resetting (inBattle will be cleared)");
        ResetHostingOverlayState();
        ResetJoiningOverlayState();
        ClearPendingLobbySpectateWait("session_ended");
        DisarmSpectateReplayBypass();
        if (bridgeStatus.errorMsg[0] != '\0')
        {
            SetNetplayStatusMessage(bridgeStatus.errorMsg, 3200);
        }
        netplay::bridge::CancelSession("no_overlay_session_ended");
        notifyLobbySessionEndedForCurrentBridgeRole(false);
    }

    if (bridgePhase == NetbridgePhase::Connecting || bridgePhase == NetbridgePhase::DelaySetup)
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

        // C button (heavy attack, offset 20/21) — copy IP:PORT to clipboard
        if (g_hostingOverlay.active
            && !g_hostingOverlay.challengeMode
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
                char clipText[192] = {};
                std::snprintf(clipText, sizeof(clipText), "%s:%u",
                    g_hostingOverlay.publicIp,
                    static_cast<unsigned>(g_hostingOverlay.port));
                if (netplay::bridge::takeover::TryWriteClipboardAscii(clipText))
                {
                    g_hostingOverlay.copiedToClipboard = true;
                    g_hostingOverlay.copiedFlashTick = GetTickCount();
                    PlayUiSound(screenContext, kSfxConfirm);
                    mod::Log("HostingOverlay: copied '%s' to clipboard", clipText);
                }
            }
        }

        if (cancelRequested)
        {
            PlayUiSound(screenContext, kSfxConfirm);
            ResetHostingOverlayState();
            ResetJoiningOverlayState();
            ClearPendingLobbySpectateWait("user_cancel");
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
