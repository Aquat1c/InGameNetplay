#pragma once

#include <array>
#include <cstdint>

#include "netplay/core/menu_model.h"

namespace netplay::constants
{
inline constexpr uintptr_t kEfzImageBase = 0x00400000;

inline constexpr uintptr_t kVaProcessPlayerInput = 0x00406590;
inline constexpr uintptr_t kVaPlaySoundEffect = 0x00406860;
inline constexpr uintptr_t kVaPlayBackgroundMusic = 0x004068B0;
inline constexpr uintptr_t kVaStopBackgroundMusic = 0x00406A10;
inline constexpr uintptr_t kVaStopSoundBuffer = 0x0040DE40;
inline constexpr uintptr_t kVaPlaySoundBuffer = 0x0040DE80;
inline constexpr uintptr_t kVaReleaseSoundBufferAndMemory = 0x0040DF50;
inline constexpr uintptr_t kVaLoadWaveFile = 0x0040DFB0;
inline constexpr uintptr_t kVaLoadAudioTimingData = 0x0040E2B0;
inline constexpr uintptr_t kVaLoadCompressedImageFile = 0x00406DB0;
inline constexpr uintptr_t kVaFadeWithSoundAdjustment = 0x00759C90;
inline constexpr uintptr_t kVaFadeScreenEffect = 0x00759E20;
inline constexpr uintptr_t kVaPerformSlideAnimation = 0x0075DA30;
inline constexpr uintptr_t kVaLoadBgrColorsFromRawFile = 0x0040B760;
inline constexpr uintptr_t kVaReadPixelValue = 0x0040BCC0;
inline constexpr uintptr_t kVaSetPalette = 0x0040BD30;
inline constexpr uintptr_t kVaBlitSurfaceWithTransparency = 0x00409A90;
inline constexpr uintptr_t kVaPresentFrameToScreen = 0x0040B5E0;

inline constexpr uintptr_t kVaTitleRender = 0x007764B0;
inline constexpr uintptr_t kVaUpdateTitleScreenLogic = 0x00775FB0;
inline constexpr uintptr_t kVaTitleVtableRender = 0x00789980;
inline constexpr uintptr_t kVaTitleVtableUpdate = 0x00789984;
inline constexpr uintptr_t kVaTitleCaseEpilogue = 0x00776483;
inline constexpr uintptr_t kVaCurrentScreenIndex = 0x00790148;

inline constexpr uintptr_t kVaWrapCmpMax = 0x0077612E;
inline constexpr uintptr_t kVaWrapClampNegative = 0x0077614B;
inline constexpr uintptr_t kVaSwitchCmpMax = 0x007761D5;
inline constexpr uintptr_t kVaSwitchTableDisp = 0x007761E5;

inline constexpr uintptr_t kVaRenderPanelDestH = 0x00776578;
inline constexpr uintptr_t kVaRenderPanelSourceH = 0x007765A3;
inline constexpr uintptr_t kVaRenderPanelDestTop = 0x007765A7;
inline constexpr uintptr_t kVaRenderHighlightDestBase = 0x0077664A;
inline constexpr uintptr_t kVaRenderHighlightScreenBase = 0x00776696;

inline constexpr uint32_t kMenuDestTopPatched = 123;

inline constexpr uintptr_t kVaCaseArcade = 0x007761E9;
inline constexpr uintptr_t kVaCaseVsCpu = 0x00776268;
inline constexpr uintptr_t kVaCaseVsHuman = 0x007762E3;
inline constexpr uintptr_t kVaCasePractice = 0x00776352;
inline constexpr uintptr_t kVaCaseReplay = 0x007763D1;
inline constexpr uintptr_t kVaCaseOptions = 0x00776432;
inline constexpr uintptr_t kVaCaseExit = 0x0077645F;

inline constexpr uint32_t kOffsetWindowHandle = 0x08;
inline constexpr uint32_t kOffsetGameSystem = 0x1C;
inline constexpr uint32_t kOffsetGraphicsContext = 0x20;
inline constexpr uint32_t kOffsetPalette = 46;
inline constexpr uint32_t kOffsetTransparentColor = 0x42E;
inline constexpr uint32_t kOffsetBackgroundSurface = 0x434;
inline constexpr uint32_t kOffsetObjectsSurface = 0x438;
inline constexpr uint32_t kOffsetMenuSelection = 0x43C;
inline constexpr uint32_t kOffsetMenuAnimCounter = 0x43E;
inline constexpr uint32_t kOffsetInputLatchP1 = 0x440;
inline constexpr uint32_t kOffsetInputLatchP2 = 0x441;
inline constexpr uint32_t kOffsetTitleMenuState = 0x442;
inline constexpr uint32_t kOffsetInactivityCounter = 0x444;
inline constexpr uint32_t kOffsetSlideAnimationY = 1876;
inline constexpr uint32_t kOffsetScreenInitState = 44;
inline constexpr uint32_t kOffsetScreenExitState = 45;

// Character-select screen object offsets (relative to the screen object).
// Grid col/row are only set by the constructor (Ex), NOT by the per-entry
// reinit at 0x7597D0, so they must be explicitly zeroed for a clean start.
inline constexpr uint32_t kOffsetCharSelectP1GridCol   = 1336;
inline constexpr uint32_t kOffsetCharSelectP2GridCol   = 1337;
inline constexpr uint32_t kOffsetCharSelectP1GridRow   = 1338;
inline constexpr uint32_t kOffsetCharSelectP2GridRow   = 1339;
inline constexpr uint32_t kOffsetCharSelectP1CharId    = 1340;
inline constexpr uint32_t kOffsetCharSelectP2CharId    = 1341;
inline constexpr uint32_t kOffsetCharSelectP1Color    = 1342;
inline constexpr uint32_t kOffsetCharSelectP2Color    = 1343;
inline constexpr uint32_t kOffsetCharSelectP1Timer    = 1344;
inline constexpr uint32_t kOffsetCharSelectP2Timer    = 1346;
// Default values matching the constructor (initializeCharacterSelectScreenEx):
inline constexpr uint8_t  kCharSelectDefaultP1Col = 0;  // leftmost column
inline constexpr uint8_t  kCharSelectDefaultP1Row = 0;  // top row
inline constexpr uint8_t  kCharSelectDefaultP2Col = 2;  // third column
inline constexpr uint8_t  kCharSelectDefaultP2Row = 0;  // top row
// Grid map starts at screen object + 1209; charId = gridMap[row*3 + col].
inline constexpr uint32_t kOffsetCharSelectGridMap = 1209;
inline constexpr uint32_t kOffsetGraphicsPrimarySurface = 33283u * sizeof(uint32_t);
inline constexpr uint32_t kOffsetGraphicsBackBufferSurface = 33284u * sizeof(uint32_t);

inline constexpr uint32_t kVtableOffsetSurfaceGetDc = 68;
inline constexpr uint32_t kVtableOffsetSurfaceReleaseDc = 104;
inline constexpr uint32_t kVtableOffsetSurfaceLock = 100;
inline constexpr uint32_t kVtableOffsetSurfaceUnlock = 128;

inline constexpr unsigned short kSfxConfirm = 6;
inline constexpr unsigned short kSfxMove = 8;
inline constexpr unsigned short kNetplayBgmTrack = 8;
inline constexpr uint32_t kNetplayFrameLogIntervalMs = 2000;
inline constexpr uint16_t kDefaultNetplayPort = 7500;

inline constexpr int kNetplayDefaultOptionCount = 7; // main menu now has 7 entries
inline constexpr int kNetplayDefaultBackIndex = 6;
inline constexpr int kNetplayConfigOptionCount = netplay::menu::kConfigOptionCount;
inline constexpr int kNetplayConfigBackIndex = 9;
inline constexpr int kNetplayDefaultHighlightHeight = 14;
inline constexpr int kNetplayCompactMenuTopY = 72;
inline constexpr int kNetplayCompactMenuRowStep = 16;
inline constexpr int kNetplayFooterPanelTopY = 214;
inline constexpr int kNetplayFooterPanelHeight = 24;
inline constexpr int kNetplayFooterTextY = 220;
inline constexpr int kNetplayFooterTextLeft = 14;
inline constexpr int kNetplayFooterTextRight = 306;
inline constexpr int kNetplayNativeSlideDivisor = 3;
inline constexpr uint32_t kInlineEditCaretBlinkMs = 350;
inline constexpr uint32_t kInlineEditErrorDisplayMs = 1800;
inline constexpr size_t kInlineEditMaxPortLength = 5;
inline constexpr size_t kInlineEditMaxJoinAddressLength = 63;
inline constexpr size_t kInlineEditMaxRoomCodeLength = 63;
inline constexpr size_t kInlineEditMaxNicknameLength = 60;

inline constexpr std::array<int, kNetplayConfigOptionCount> kDefaultHighlightSourceY = {316, 330, 344, 358, 372, 386, 400, 414, 428, 442};
inline constexpr std::array<int, kNetplayConfigOptionCount> kDefaultHighlightDestY = {76, 90, 104, 118, 132, 146, 160, 174, 188, 202};
inline constexpr std::array<int, kNetplayConfigOptionCount> kDefaultHighlightWidth = {320, 320, 320, 320, 320, 320, 320, 320, 320, 320};

struct NetplayRenderLayout
{
    std::array<int, kNetplayConfigOptionCount> highlightSourceY = kDefaultHighlightSourceY;
    std::array<int, kNetplayConfigOptionCount> highlightDestY = kDefaultHighlightDestY;
    std::array<int, kNetplayConfigOptionCount> highlightWidth = kDefaultHighlightWidth;
    int highlightSourceX = 0;
    int highlightDestX = 0;
    int highlightHeight = kNetplayDefaultHighlightHeight;
};
}
