#pragma once

#include <cstdint>
#include <windows.h>
#include "compat/overlay_reset_gate.h"

struct IDirect3DDevice9;
struct ImGuiContext;

namespace netplay::debug_overlay
{
class DeviceRenderScope {
public:
    explicit DeviceRenderScope(IDirect3DDevice9* device = nullptr);
    ~DeviceRenderScope();
    DeviceRenderScope(const DeviceRenderScope&) = delete;
    DeviceRenderScope& operator=(const DeviceRenderScope&) = delete;
    bool Admitted() const { return admitted_; }
    void Release();
private:
    EfzOverlayResetGate::Access access_;
    ImGuiContext* previousContext_ = nullptr;
    ImGuiContext* ownedContextAtEntry_ = nullptr;
    bool admitted_ = false;
};

void Render(IDirect3DDevice9* device);

bool SuspendForOnlineSimulation();

bool ReplaceChainedWindowProc(
    HWND hwnd,
    WNDPROC expectedPrevious,
    WNDPROC replacement);

bool HasChainedWindowProc(HWND hwnd, WNDPROC expectedPrevious);

void ToggleDebugPanel();

bool IsDebugPanelActive();

enum class RtTextAlign : uint8_t
{
    Left = 0,
    Center,
    Right,
};

enum class RtTextProfile : uint8_t
{
    MenuHeader = 0,
    MenuRow,
    Footer,
    BattleLogHeader,
    BattleLogRow,
    MenuSection,
    OverlayTitle,
    OverlayBody,
    OverlayHint,
    Count,
};

struct RtTextItem
{
    int16_t x0 = 0;                    // 320-space
    int16_t x1 = 0;
    int16_t y = 0;
    RtTextAlign align = RtTextAlign::Left;
    RtTextProfile profile = RtTextProfile::MenuRow;
    uint32_t rgba = 0xFFFFFFFFu;       // ABGR-packed
    int16_t clipX0 = 0;
    int16_t clipX1 = 0;
    int16_t y1 = 0;
    bool occluder = false;             // hides items submitted before it inside [x0,x1)x[y,y1)
    char text[192] = {};
};

void SetRtText(RtTextItem& item, const char* text, size_t length);

bool IsRtTextAvailable();

bool RtTextHasExtendedGlyphs();

void NotifyFontSettingsChanged();

int MeasureRtTextWidth(RtTextProfile profile, const char* text);

void BeginRtTextFrame();
void SubmitRtText(const RtTextItem& item);
void SubmitRtOccluder(int x, int y, int w, int h);
void CommitRtTextFrame();
void ClearRtText();

// Brackets the game's 21-step palette fade so RT frames dim with it.
void BeginRtTextFade(bool fadeIn);
void EndRtTextFade();
}
