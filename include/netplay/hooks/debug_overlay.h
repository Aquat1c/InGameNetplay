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
    Left = 0,    // anchor at x0
    Center,      // centered between x0 and x1
    Right,       // anchor right edge at x1
};

enum class RtTextProfile : uint8_t
{
    MenuHeader = 0,   // large page headers
    MenuRow,          // generic menu rows
    Footer,           // footer tooltip lines
    BattleLogHeader,  // battle log panel titles
    BattleLogRow,     // battle log dense rows
    MenuSection,      // small inline section headers between menu rows
    OverlayTitle,     // HOSTING / JOINING / modal titles
    OverlayBody,      // modal status, address, error, and option text
    OverlayHint,      // modal controls and secondary hints
    Count,
};

struct RtTextItem
{
    int16_t x0 = 0;                    // logical left bound (320-space)
    int16_t x1 = 0;                    // logical right bound
    int16_t y = 0;                     // logical top of the 5x7 cell it replaces
    RtTextAlign align = RtTextAlign::Left;
    RtTextProfile profile = RtTextProfile::MenuRow;
    uint32_t rgba = 0xFFFFFFFFu;       // IM_COL32-style ABGR-packed color
    int16_t clipX0 = 0;
    int16_t clipX1 = 0;
    char text[112] = {};
};

bool IsRtTextAvailable();

bool RtTextHasExtendedGlyphs();

void NotifyFontSettingsChanged();

int MeasureRtTextWidth(RtTextProfile profile, const char* text);

void BeginRtTextFrame();
void SubmitRtText(const RtTextItem& item);
void CommitRtTextFrame();
void ClearRtText();
}
