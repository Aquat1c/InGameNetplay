#include "netplay/hooks/debug_overlay.h"

#include "netplay/bridge/async_hosting.h"
#include "netplay/core/mod_settings.h"
#include "netplay/hooks/menu_query.h"
#include "logger.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

#include <cfloat>
#include <cstdio>

// Declared by the Win32 backend; we call it from our chained wndproc.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace netplay::debug_overlay
{
namespace
{
bool g_inited = false;
IDirect3DDevice9* g_device = nullptr;
HWND g_hwnd = nullptr;
WNDPROC g_prevWndProc = nullptr;
bool g_panelOpen = false;

// Crisp badge font, loaded from a system TTF at a fixed native pixel size so the
// text is not a scaled-up bitmap (the ImGui default font blurs when scaled).
// nullptr -> fall back to the default font.
ImFont* g_badgeFont = nullptr;
constexpr float kBadgeFontPx = 22.0f;

// EFZ renders its scene to a fixed 640x480 D3D9 render target; EFZ Revival then
// composites/upscales that to the window backbuffer. Our overlay must draw on the
// 640x480 GAME surface (so it scales with the game) — NOT on Revival's window
// backbuffer. EndScene fires for both; we filter by render-target size. (This is
// exactly how efz-training-mode picks the correct surface.)
constexpr int kGameRtW = 640;
constexpr int kGameRtH = 480;

// Backbuffer dimensions last seen — used to detect a device RESET (window
// resize / borderless-fullscreen toggle), which invalidates ImGui's
// D3DPOOL_DEFAULT resources and makes the overlay vanish until rebuilt.
int g_lastBackBufferW = 0;
int g_lastBackBufferH = 0;

// Tunable async-host in-battle indicator parameters. Adjust live in the panel,
// then hardcode the chosen defaults here.
float g_asyncPosX = 0.5f;     // fraction of screen width  (0=left, 1=right)
float g_asyncPosY = 0.0f;     // fraction of screen height (0=top, 1=bottom)
float g_asyncFontScale = 0.65f;
float g_asyncBgAlpha = 0.48f; // tuned live via the backslash debug panel

// Load a crisp UI font from the system fonts directory. Tries a few common
// faces; returns nullptr (caller falls back to the default font) if none load.
ImFont* LoadSystemBadgeFont()
{
    char winDir[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryA(winDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return nullptr;
    }
    static const char* kFaces[] = { "segoeui.ttf", "tahoma.ttf", "arial.ttf", "verdana.ttf" };
    ImGuiIO& io = ImGui::GetIO();
    for (const char* face : kFaces)
    {
        char path[MAX_PATH] = {};
        std::snprintf(path, sizeof(path), "%s\\Fonts\\%s", winDir, face);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
        {
            continue;
        }
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, kBadgeFontPx);
        if (f != nullptr)
        {
            mod::Log("DebugOverlay: loaded badge font %s @%.0fpx", path, kBadgeFontPx);
            return f;
        }
    }
    return nullptr;
}

LRESULT CALLBACK DebugWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (g_inited)
    {
        // Toggle key: backslash ("\", next to Enter) = VK_OEM_5. DELETE is
        // already used elsewhere in the game.
        if (msg == WM_KEYDOWN && wParam == VK_OEM_5
            && netplay::mod_settings::IsDebugMenuEnabled())
        {
            g_panelOpen = !g_panelOpen;
            mod::Log("DebugOverlay: panel %s (backslash)", g_panelOpen ? "opened" : "closed");
        }

        const LRESULT handled = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        if (handled != 0 && g_panelOpen)
        {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.WantCaptureMouse || io.WantCaptureKeyboard)
            {
                return handled;
            }
        }
    }

    if (g_prevWndProc != nullptr)
    {
        return CallWindowProcA(g_prevWndProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void ShutdownImGui()
{
    if (!g_inited)
    {
        return;
    }
    if (g_prevWndProc != nullptr && g_hwnd != nullptr)
    {
        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prevWndProc));
        g_prevWndProc = nullptr;
    }
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_inited = false;
    g_device = nullptr;
    g_hwnd = nullptr;
    g_badgeFont = nullptr;
    g_lastBackBufferW = 0;
    g_lastBackBufferH = 0;
}

// Returns the device's current RENDER TARGET size, or {0,0} on failure. During
// the game's scene draw this is 640x480; during Revival's window present it is the
// window size. Used to pick the correct surface.
void GetRenderTargetSize(IDirect3DDevice9* device, int* outW, int* outH)
{
    *outW = 0;
    *outH = 0;
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt != nullptr)
    {
        D3DSURFACE_DESC desc = {};
        if (SUCCEEDED(rt->GetDesc(&desc)))
        {
            *outW = static_cast<int>(desc.Width);
            *outH = static_cast<int>(desc.Height);
        }
        rt->Release();
    }
}

// Returns the swap chain's backbuffer (window) size, or {0,0} on failure. Unlike
// the 640x480 render target, this changes on window resize / fullscreen toggle,
// so it is what we use to detect a device reset (ImGui object rebuild).
void GetSwapChainBackBufferSize(IDirect3DDevice9* device, int* outW, int* outH)
{
    *outW = 0;
    *outH = 0;
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &sc)) && sc != nullptr)
    {
        D3DPRESENT_PARAMETERS pp = {};
        if (SUCCEEDED(sc->GetPresentParameters(&pp)))
        {
            *outW = static_cast<int>(pp.BackBufferWidth);
            *outH = static_cast<int>(pp.BackBufferHeight);
        }
        sc->Release();
    }
}

bool EnsureInited(IDirect3DDevice9* device)
{
    if (g_inited && g_device == device)
    {
        return true;
    }
    if (g_inited && g_device != device)
    {
        // Device recreated (e.g. reset) — rebuild against the new one.
        ShutdownImGui();
    }

    HWND hwnd = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (SUCCEEDED(device->GetCreationParameters(&cp)))
    {
        hwnd = cp.hFocusWindow;
    }
    if (hwnd == nullptr)
    {
        hwnd = GetActiveWindow();
    }
    if (hwnd == nullptr)
    {
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // do not write imgui.ini
    ImGui::StyleColorsDark();

    // Load a crisp TTF before the backend builds the font atlas (on first frame).
    g_badgeFont = LoadSystemBadgeFont();

    if (!ImGui_ImplWin32_Init(hwnd))
    {
        ImGui::DestroyContext();
        return false;
    }
    if (!ImGui_ImplDX9_Init(device))
    {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    // Chain our wndproc so ImGui receives input and DELETE toggles the panel.
    // Installed once; the netplay menu's own wndproc hook chains correctly
    // because this one is installed earlier (first EndScene of the session).
    g_prevWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DebugWndProc)));

    g_hwnd = hwnd;
    g_device = device;
    g_inited = true;
    mod::Log("DebugOverlay: ImGui initialised hwnd=0x%p device=0x%p", hwnd, device);
    return true;
}

void DrawAsyncIndicator()
{
    namespace ah = netplay::bridge::async_host;
    if (!ah::IsActive() || !ah::IsMinimized())
    {
        return;
    }

    char msg[160] = {};
    ImU32 textColor = IM_COL32(255, 255, 255, 255);
    if (ah::IsTimedOut())
    {
        std::snprintf(msg, sizeof(msg), "Opponent timed out - %s to rehost", ah::ReturnKeyDisplay());
        textColor = IM_COL32(255, 210, 210, 255);
    }
    else if (ah::IsPeerFoundHeld())
    {
        std::snprintf(msg, sizeof(msg), "OPPONENT FOUND!  Press %s to start", ah::ReturnKeyDisplay());
        textColor = IM_COL32(120, 255, 140, 255);
    }
    else
    {
        std::snprintf(msg, sizeof(msg), "Hosting...  Press %s to return to HOST menu", ah::ReturnKeyDisplay());
    }

    const ImGuiIO& io = ImGui::GetIO();
    ImFont* font = (g_badgeFont != nullptr) ? g_badgeFont : ImGui::GetFont();
    const float baseSize = (g_badgeFont != nullptr) ? kBadgeFontPx : ImGui::GetFontSize();
    const float fontSize = baseSize * g_asyncFontScale;
    const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, msg);
    const float cx = io.DisplaySize.x * g_asyncPosX;
    const float ty = io.DisplaySize.y * g_asyncPosY;
    const ImVec2 pos(cx - textSize.x * 0.5f, ty);
    const float pad = 5.0f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(
        ImVec2(pos.x - pad, pos.y - pad),
        ImVec2(pos.x + textSize.x + pad, pos.y + textSize.y + pad),
        IM_COL32(10, 16, 28, static_cast<int>(g_asyncBgAlpha * 255.0f)),
        4.0f);
    dl->AddText(font, fontSize, pos, textColor, msg);
}

void DrawDebugPanel()
{
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("EFZ Netplay Debug ( \\ to toggle)", &g_panelOpen))
    {
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("DisplaySize: %.0f x %.0f", io.DisplaySize.x, io.DisplaySize.y);
        ImGui::Text("Mouse: %.0f, %.0f", io.MousePos.x, io.MousePos.y);

        ImGui::SeparatorText("Async-host in-battle indicator");
        ImGui::SliderFloat("PosX (frac)", &g_asyncPosX, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("PosY (frac)", &g_asyncPosY, 0.0f, 1.0f, "%.3f");
        ImGui::SliderFloat("Font scale", &g_asyncFontScale, 0.5f, 4.0f, "%.2f");
        ImGui::SliderFloat("BG alpha", &g_asyncBgAlpha, 0.0f, 1.0f, "%.2f");
        ImGui::TextDisabled("Dial these in, then hardcode the defaults\nin debug_overlay.cpp.");

        namespace ah = netplay::bridge::async_host;
        ImGui::SeparatorText("Async-host state");
        ImGui::Text("state=%d active=%d minimized=%d held=%d timedOut=%d",
            static_cast<int>(ah::GetState()),
            ah::IsActive() ? 1 : 0,
            ah::IsMinimized() ? 1 : 0,
            ah::IsPeerFoundHeld() ? 1 : 0,
            ah::IsTimedOut() ? 1 : 0);
    }
    ImGui::End();
}
} // namespace

void Render(IDirect3DDevice9* device)
{
    if (device == nullptr)
    {
        return;
    }

    // Surface filter — only draw on the 640x480 game render target. EndScene also
    // fires for Revival's window-backbuffer present (different size); drawing there
    // put the overlay on the wrong surface. Skip non-game frames entirely (do not
    // even init, so ImGui binds to the correct device).
    {
        int rtW = 0;
        int rtH = 0;
        GetRenderTargetSize(device, &rtW, &rtH);
        if (rtW != kGameRtW || rtH != kGameRtH)
        {
            return;
        }
    }

    namespace ah = netplay::bridge::async_host;
    const bool debugAvailable = netplay::mod_settings::IsDebugMenuEnabled();
    // Suppress the top-middle ImGui badge while the netplay menu is open — the
    // menu already shows the indexed top-right HOSTING badge there. The ImGui
    // badge is for every OTHER screen (title / gameplay / etc.).
    const bool wantIndicator =
        ah::IsActive() && ah::IsMinimized() && !netplay::hooks::IsNetplayMenuActive();
    const bool wantPanel = debugAvailable && g_panelOpen;

    // Initialise when the debug option is on (so DELETE works on any screen) or
    // when the in-battle indicator needs drawing.
    if (!debugAvailable && !wantIndicator && !g_inited)
    {
        return;
    }
    if (!EnsureInited(device))
    {
        return;
    }

    // Device-RESET detection: when EFZ resizes the window or toggles
    // borderless-fullscreen it Reset()s the device, which silently destroys
    // ImGui's D3DPOOL_DEFAULT resources (font texture + vertex/index buffers) and
    // makes the overlay vanish. The 640x480 render target never changes, so we
    // watch the swap-chain (window) backbuffer size and rebuild on a change.
    {
        int bbW = 0;
        int bbH = 0;
        GetSwapChainBackBufferSize(device, &bbW, &bbH);
        if (bbW > 0 && bbH > 0 && (bbW != g_lastBackBufferW || bbH != g_lastBackBufferH))
        {
            if (g_lastBackBufferW != 0 || g_lastBackBufferH != 0)
            {
                mod::Log(
                    "DebugOverlay: window backbuffer %dx%d -> %dx%d (device reset) — rebuilding ImGui objects",
                    g_lastBackBufferW, g_lastBackBufferH, bbW, bbH);
                ImGui_ImplDX9_InvalidateDeviceObjects();
            }
            g_lastBackBufferW = bbW;
            g_lastBackBufferH = bbH;
        }
    }

    if (!wantPanel && !wantIndicator)
    {
        return; // initialised, but nothing to draw this frame
    }

    // One-shot diagnostics: confirm the ImGui/EndScene path actually renders
    // (the in-battle / outside-menu indicator depends on EndScene firing for the
    // game's device). If this never logs while hosting+minimized in a match, the
    // D3D9 EndScene hook is not reaching the game's present.
    {
        static bool s_loggedFirstRender = false;
        if (!s_loggedFirstRender)
        {
            s_loggedFirstRender = true;
            const ImGuiIO& io = ImGui::GetIO();
            mod::Log(
                "DebugOverlay: first ImGui render (wantIndicator=%d wantPanel=%d display=%.0fx%.0f)",
                wantIndicator ? 1 : 0, wantPanel ? 1 : 0,
                io.DisplaySize.x, io.DisplaySize.y);
        }
    }

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    // We draw onto the 640x480 GAME render target, but ImGui_ImplWin32_NewFrame
    // just set io.DisplaySize to the WINDOW client size (e.g. 1920x1080 in
    // borderless fullscreen). If we leave it, ImGui lays out in window space but
    // renders into the 640-wide viewport, squishing everything to ~1/3 — small
    // text (the HOSTING badge) effectively vanishes while the big panel still
    // shows. Override DisplaySize to the render-target size (what training mode
    // does) so layout matches the surface; scale MousePos into that space so the
    // debug panel stays clickable in fullscreen.
    {
        ImGuiIO& io = ImGui::GetIO();
        const float winW = io.DisplaySize.x;
        const float winH = io.DisplaySize.y;
        if (winW > 0.0f && winH > 0.0f
            && io.MousePos.x > -FLT_MAX * 0.5f && io.MousePos.y > -FLT_MAX * 0.5f)
        {
            io.MousePos.x *= static_cast<float>(kGameRtW) / winW;
            io.MousePos.y *= static_cast<float>(kGameRtH) / winH;
        }
        io.DisplaySize = ImVec2(static_cast<float>(kGameRtW), static_cast<float>(kGameRtH));
    }

    ImGui::NewFrame();

    if (wantIndicator)
    {
        DrawAsyncIndicator();
    }
    if (wantPanel)
    {
        DrawDebugPanel();
    }

    ImGui::EndFrame();
    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
}

void ToggleDebugPanel()
{
    if (netplay::mod_settings::IsDebugMenuEnabled())
    {
        g_panelOpen = !g_panelOpen;
    }
}

bool IsDebugPanelActive()
{
    return g_panelOpen;
}
} // namespace netplay::debug_overlay
