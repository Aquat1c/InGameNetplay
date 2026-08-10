#pragma once

#include "netplay/core/constants.h"
#include "netplay/core/inline_edit.h"
#include "netplay/core/lobby_client.h"
#include "netplay/core/menu_model.h"
#include "netplay/core/network_endpoint.h"
#include "netplay/core/patch_utils.h"
#include "netplay/render/menu_overlay.h"
#include "netplay/render/sprite_font_map.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

namespace netplay::hooks::internal
{
// Per-frame render tracing for gameplay-exit menu recovery (issue fixed).
inline constexpr bool kEnableGameplayExitRecoveryRenderDiagnostics = false;

enum class NetplayNicknameSource : uint8_t
{
    Placeholder = 0,
    LoadedFromIni,
    UserProvided,
};

enum class NetplayMenuTheme : uint8_t
{
    Scroll = 0,
    Classic,
};

struct NetplayMenuState
{
    bool active = false;
    bool bgmActive = false;
    bool useConfigStyleRender = false;
    NetplayMenuTheme theme = NetplayMenuTheme::Scroll;
    netplay::menu::NetplayMenuId menuId = netplay::menu::NetplayMenuId::Main;
    int mainSelection = 0;
    int optionCount = netplay::constants::kNetplayDefaultOptionCount;
    int backIndex = netplay::constants::kNetplayDefaultBackIndex;
    netplay::network::NetworkFamily hostFamily =
        netplay::network::NetworkFamily::IPv4;
    uint16_t hostPort = netplay::constants::kDefaultNetplayPort;
    std::string joinAddress = "127.0.0.1";
    uint16_t joinPort = netplay::constants::kDefaultNetplayPort;
    std::string nickname = "Player";
    NetplayNicknameSource nicknameSource = NetplayNicknameSource::Placeholder;
    uint8_t paletteStart = 193;
    uint8_t paletteCount = 48;
    netplay::constants::NetplayRenderLayout renderLayout = {};
    bool backgroundSupportsScroll = false;
    int backgroundWidth = 320;
    int backgroundHeight = 240;
    double backgroundScrollOffset = 0.0;
    DWORD backgroundScrollTick = 0;
    // Lobby browser: index of the first idle player shown in the visible window.
    // Incremented/decremented at the scroll boundary rows to pan through a list
    // longer than kLobbyMaxDisplayPlayers.
    int lobbyScrollOffset = 0;
};

struct MenuSlideTransition
{
    bool active = false;
    netplay::menu::NetplayMenuId fromMenu = netplay::menu::NetplayMenuId::Main;
    netplay::menu::NetplayMenuId toMenu = netplay::menu::NetplayMenuId::Main;
    int fromSelection = 0;
    int toSelection = 0;
    int direction = 1;
    int frame = 0;
};

struct InputSnapshot
{
    int8_t p1Horizontal = 0;
    int8_t p1Vertical = 0;
    uint8_t p1Confirm = 0;
    uint8_t p1Cancel = 0;
    int8_t p2Horizontal = 0;
    int8_t p2Vertical = 0;
    uint8_t p2Confirm = 0;
    uint8_t p2Cancel = 0;
};

struct DelaySetupOverlayState
{
    bool active = false;
    bool waitingForRuntimeReady = false;
    bool vsHumanSyncArmed = false;
    bool connectedHandoffDelayActive = false;
    DWORD nextHandoffRetryTick = 0;
    int connectedHandoffDelayFramesRemaining = 0;
    int selectedDelay = 0;
    int recommendedDelay = 0;
    int minDelay = 0;
    int maxDelay = 20;
    int pingMs = -1;
    int currentDelay = 0;
    char p1Name[64] = {};
    char p2Name[64] = {};
    char errorMessage[96] = {};
};

struct SpectateConfirmOverlayState
{
    bool active = false;
    int selectedOption = 0;
    int optionCount = 2;
    int promptKind = 0;
    char errorMessage[96] = {};
};

struct HostingOverlayState
{
    bool active = false;
    uint16_t port = 0;
    bool challengeMode = false;
    netplay::network::NetworkFamily preferredFamily =
        netplay::network::NetworkFamily::IPv4;
    netplay::network::NetworkFamily effectiveFamily =
        netplay::network::NetworkFamily::IPv4;
    bool usedFamilyFallback = false;
    bool discoveryInProgress = false;
    bool sessionQueued = false;
    bool listenerReady = false;
    bool listenerMismatch = false;
    bool familyRetryAllowed = false;
    bool automaticFamilyRetryAttempted = false;
    bool failed = false;
    bool failureNeedsBridgeCancel = false;
    bool writeNicknameToIni = true;
    char publicIp[128] = {};       // filled asynchronously
    char hostNickname[64] = {};
    char targetName[64] = {};
    char errorText[128] = {};
    bool ipFetchDone = false;      // true once background fetch completes (success or fail)
    bool ipFetchFailed = false;    // true if all attempts failed
    uint32_t preferredSourceAttempts = 0;
    uint32_t preferredTransportFailures = 0;
    uint32_t preferredParseFailures = 0;
    uint32_t alternateSourceAttempts = 0;
    uint32_t alternateTransportFailures = 0;
    uint32_t alternateParseFailures = 0;
    bool copiedToClipboard = false;
    DWORD copiedFlashTick = 0;     // GetTickCount() when copy happened (for brief visual feedback)
    bool familyFallbackNoticeStarted = false;
    DWORD familyFallbackNoticeStartTick = 0;
    DWORD listenerWaitStartTick = 0;
    uint32_t listenerMismatchSerial = 0;
    uint16_t listenerMismatchPort = 0;
    DWORD listenerMismatchFirstTick = 0;
};

struct JoiningOverlayState
{
    bool active = false;
    uint16_t port = 0;
    netplay::network::NetworkFamily family =
        netplay::network::NetworkFamily::IPv4;
    bool displayTargetName = false;
    bool spectateMode = false;
    bool waitingForGameBegin = false;
    char address[128] = {};         // target address
    char targetName[64] = {};
    char errorText[128] = {};       // populated when bridge reports failure
    bool failed = false;            // true when connection attempt failed
};

struct DebugOverlayState
{
    bool open = false;
    int selectedIndex = 0;
    bool forceDelayOverlay = false;
    bool forceSpectateOverlay = false;
    bool forceRuntimeTextOverlay = false;
    bool showConsole = false;
};

using PlaySoundEffectFn = int(__thiscall*)(void* gameSystem, unsigned short soundIndex);
using PlayBackgroundMusicFn = void(__thiscall*)(int gameSystem, unsigned short trackNumber);
using StopBackgroundMusicFn = int(__thiscall*)(int gameSystem);
using StopSoundBufferFn = int(__thiscall*)(uint32_t* soundManager, unsigned short bufferIndex);
using PlaySoundBufferFn = int(__thiscall*)(void* soundManager, unsigned short bufferIndex, int loopFlag);
using ReleaseSoundBufferAndMemoryFn = int(__thiscall*)(void* soundManager, unsigned short bufferIndex);
using LoadWaveFileFn = unsigned short(__thiscall*)(void* soundManager, char* fileName);
using LoadAudioTimingDataFn = void(__thiscall*)(uint32_t* timingPtr, const char* fileName);
using ProcessPlayerInputFn = void(__thiscall*)(int* inputManager);
using LoadCompressedImageFileFn =
    void(__thiscall*)(void*** graphicsManager, uint32_t* destSurface, const char* fileName, unsigned char colorOffset1, unsigned char colorOffset2);
using LoadBgrColorsFromRawFileFn = int(__stdcall*)(int destBuffer, const char* rgbFileName, int srcStartIndex, int destStartIndex, int colorCount);
using ReadPixelValueFn = char(__stdcall*)(int surfacePtr);
using SetPaletteFn = int(__thiscall*)(void** graphicsContext, int paletteData);
using FadeWithSoundAdjustmentFn =
    int(__thiscall*)(void* screenContext, int paletteId, unsigned char fadeDirection, int baseVolume, int volumeAdjustment);
using FadeScreenEffectFn = int(__thiscall*)(void* screenContext, int paletteId, unsigned char fadeDirection);
using PerformSlideAnimationFn = int(__thiscall*)(void* screenContext, unsigned char slideDirection);
using BlitSurfaceWithTransparencyFn = BOOL(__thiscall*)(
    void** graphicsContext,
    int destX,
    int destY,
    int destRight,
    int destBottom,
    int sourceSurface,
    int sourceX,
    int sourceY,
    int sourceRight,
    int sourceBottom,
    char transparentColor,
    int flipHorizontal);
using PresentFrameToScreenFn = BOOL(__thiscall*)(int graphicsContext);
using TitleUpdateFn = char(__thiscall*)(uint32_t screenContext);
using TitleRenderFn = BOOL(__thiscall*)(uint32_t screenContext);

extern std::mutex g_patchMutex;
extern std::atomic<bool> g_hooksInstalled;
extern uintptr_t g_exeBase;
extern std::vector<netplay::patch::PatchRecord> g_appliedPatches;
extern uint32_t g_customDispatchTable[8];
extern uint32_t g_replayCaseDispatchAddress;
extern "C" uint32_t g_titleCaseReturnAddress;
extern std::string g_moduleDirectory;
extern bool g_netplayAssetsAvailable;
extern NetplayMenuState g_netplayMenuState;
extern MenuSlideTransition g_menuSlideTransition;
extern netplay::inline_edit::State g_inlineEditState;
extern DWORD g_lastNetplayFrameLogTick;
extern uint64_t g_titleUpdateCallCount;
extern uint64_t g_netplayUpdateCallCount;
extern int8_t g_lastLoggedSelection;
extern bool g_hasLoggedInputSnapshot;
extern netplay::fontmap::SpriteFont g_spriteFont;
extern bool g_useRuntimeTextOverlay;
extern bool g_enableGdiFallbackOverlay;
extern HFONT g_menuOverlayFont;
extern HWND g_hookedWindow;
extern WNDPROC g_originalWindowProc;
extern bool g_netplayEscapeDown;
extern std::string g_netplayStatusMessage;
extern DWORD g_netplayStatusExpireTick;
extern bool g_restoreReplaySelectionOnNextTitleUpdate;
extern uint32_t g_replaySelectionGuardFramesRemaining;
extern int8_t g_replaySelectionRestoreTarget;
extern bool g_pendingVsHumanAutoConfirm;
extern DWORD g_pendingVsHumanAutoConfirmTick;
extern DWORD g_pendingVsHumanAutoConfirmLastLogTick;
extern bool g_returnToNetplayAfterMatch;
extern int g_recoveryRenderTraceFramesRemaining;
extern InputSnapshot g_lastInputSnapshot;
extern DelaySetupOverlayState g_delaySetupOverlay;
extern SpectateConfirmOverlayState g_spectateConfirmOverlay;
extern HostingOverlayState g_hostingOverlay;
extern JoiningOverlayState g_joiningOverlay;
extern DebugOverlayState g_debugOverlay;

// "Stop hosting?" confirmation modal shown when the user selects a netplay-menu
// option that conflicts with an active async-host listener (Join / Spectate IP /
// Lobby / Player Rooms). On confirm, the host session is cancelled and the deferred
// action runs; on cancel, hosting continues.
struct StopHostingConfirmState
{
    bool active = false;
    // The A press that opened the modal must be released before the modal
    // accepts navigation/confirmation. Per-player button state then provides
    // true press edges instead of treating a held value as a new press every
    // frame.
    bool waitingForInputRelease = true;
    uint8_t confirmDown[2] = {};
    uint8_t cancelDown[2] = {};
    int  selection = 1;  // 0 = Stop hosting (Yes), 1 = Keep hosting (No, default)
    netplay::menu::NetplayMenuAction pendingAction = netplay::menu::NetplayMenuAction::BackToMain;
    int  pendingLogicalSelection = 0;
};
extern StopHostingConfirmState g_stopHostingConfirm;
bool DrawStopHostingConfirmGdi(uint32_t screenContext);
extern std::unique_ptr<netplay::lobby::LobbySession> g_lobbySession;

HMODULE ResolveCurrentModule();
uintptr_t RuntimeAddress(uintptr_t va);
void SwitchToMenu(uint32_t screenContext, netplay::menu::NetplayMenuId menuId, int selection);
bool LoadNetplaySpriteFont();
bool IsInlineEditableAction(netplay::menu::NetplayMenuAction action);
bool GetInlineEditDisplayValue(netplay::menu::NetplayMenuAction action, std::string* outValue, bool includeCaret);
void BeginInlineEdit(netplay::menu::NetplayMenuAction action);
void ResetInlineEditState();
void CancelInlineEdit();
bool HandleInlineEditInput(uint32_t screenContext, const uint8_t* inputBytes);
void SetNetplayStatusMessage(const char* text, DWORD durationMs = 2200);
bool HasNetplayStatusMessage();
void ClearNetplayStatusMessage();
std::string GetNetplayStatusMessage();
void PlayUiSound(uint32_t screenContext, unsigned short soundIndex);
void ActivateChallengeHostingOverlay(
    const char* targetName,
    uint16_t port,
    netplay::network::NetworkFamily preferredFamily,
    netplay::network::NetworkFamily effectiveFamily,
    bool usedFamilyFallback,
    const char* publicIp);
void ResetHostingOverlayState();
void ActivateJoiningOverlay(const char* address, uint16_t port);
void ActivateChallengeJoiningOverlay(const char* targetName, const char* address, uint16_t port);
void ResetJoiningOverlayState();
bool TryStartWaitToSpectateFromJoinSettings(uint32_t screenContext, std::string* outErrorMessage);
bool InstallNetplayWindowHook(uint32_t screenContext);
bool RemoveNetplayWindowHook();
bool IsWindowFocused(HWND hwnd);
bool IsScreenWindowFocused(uint32_t screenContext);
bool ConsumeNetplayEscapeEdge();

int GetCurrentMenuEntryCount();
int ClampSelectionToCurrentMenu(int selection);
const netplay::menu::NetplayMenuEntry* GetCurrentMenuEntry(int selection);
int GetRenderRowForSelection(int selection);
bool IsRenderRowUsedByMenu(netplay::menu::NetplayMenuId menuId, int rowIndex);
int GetScaledNativeSlideY(uint32_t screenContext);
void ResetMenuSlideTransition();
bool IsMenuSlideTransitionActive();
void StartMenuSlideTransition(uint32_t screenContext, netplay::menu::NetplayMenuId targetMenu, int targetSelection, int direction);
void AdvanceMenuSlideTransition(uint32_t screenContext);

bool ApplyPatch(uintptr_t address, const std::vector<uint8_t>& expectedBytes, const std::vector<uint8_t>& patchedBytes, const char* label);
void RestorePatches();

int GetGameSystem(uint32_t screenContext);
void*** GetGraphicsManager(uint32_t screenContext);
void** GetGraphicsContext(uint32_t screenContext);
TitleUpdateFn GetOriginalTitleUpdate();
TitleRenderFn GetOriginalTitleRender();

void StopCurrentBgm(uint32_t screenContext, const char* reason);
void ResetTitleMenuState(uint32_t screenContext, int8_t selection);
void RunTransitionFadeOut(uint32_t screenContext, int baseVolume, int volumeAdjustment);
void RunTransitionFadeIn(uint32_t screenContext);
bool LoadTitleAssets(uint32_t screenContext);
bool LoadNetplayAssets(uint32_t screenContext);
void LoadNetplayMenuSettingsFromIni();
void SaveNetplayJoinAddressToIni();
const char* NetplayNicknameSourceToString(NetplayNicknameSource source);
bool ShouldWriteNicknameToRevivalIni();

std::string BuildMenuHeaderText();
std::string BuildRowLabel(const netplay::menu::NetplayMenuEntry& entry);
std::string BuildFooterText();
HFONT GetMenuOverlayFont();
const netplay::render::OverlayCallbacks& GetOverlayCallbacks();
bool DrawRuntimeTextOverlayGdi(uint32_t screenContext, bool allowWindowDc);
bool DrawDynamicFieldValuesGdi(uint32_t screenContext, bool allowWindowDc);
bool DrawFooterTooltipOverlayGdi(uint32_t screenContext, bool allowWindowDc);
bool DrawDelaySetupOverlayGdi(uint32_t screenContext, bool allowWindowDc);
bool DrawHostingOverlayGdi(uint32_t screenContext, bool allowWindowDc);
bool DrawJoiningOverlayGdi(uint32_t screenContext, bool allowWindowDc);
bool DrawSpectateConfirmOverlayGdi(uint32_t screenContext, bool allowWindowDc);
bool DrawDebugOverlay(uint32_t screenContext);
bool HandleDebugOverlayInput(uint32_t screenContext, const uint8_t* inputBytes, uint32_t* inactivityCounter);
BOOL RenderNetplayMenuRuntimeText(uint32_t screenContext);
BOOL RenderNetplayMenuConfigStyle(uint32_t screenContext);
void DrawAnimatedCompactMenuLayer(uint32_t screenContext);
void DrawNetplayBaseLayer(uint32_t screenContext);

void EnterNetplayMenu(uint32_t screenContext, bool skipFadeOut = false);
// |keepHostSession| true = "minimize": tear down the menu UI but DO NOT cancel
// the active netplay session (async hosting keeps the host listener alive while
// the user returns to the title screen).
void LeaveNetplayMenu(uint32_t screenContext, bool keepHostSession = false);
bool ShutdownLobbySessionForProcessExit(bool emergency, const char* reason);
void ExecuteNetplayAction(uint32_t screenContext, netplay::menu::NetplayMenuAction action, int logicalSelection);
char UpdateNetplayMenu(uint32_t screenContext);
void TriggerNetplayMenuEntry(uint32_t screenContext);

extern "C" char __cdecl HookedTitleUpdateImpl(uint32_t screenContext);
extern "C" BOOL __cdecl HookedTitleRenderImpl(uint32_t screenContext);


#if defined(_M_IX86)
extern "C" void __cdecl NetplayCaseImpl(uint32_t screenContext);
extern "C" void NetplayCaseThunk();
extern "C" void __cdecl ReplayCaseCompatImpl(uint32_t screenContext);
extern "C" void ReplayCaseCompatThunk();
extern "C" void HookedTitleUpdateThunk();
extern "C" void HookedTitleRenderThunk();
#endif
} // namespace netplay::hooks::internal
