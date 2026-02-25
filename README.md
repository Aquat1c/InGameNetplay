# EFZ In-Game Netplay Mod (DLL)

Runtime DLL mod that injects a `NETPLAY` entry into EFZ title menu and runs a custom in-engine netplay submenu loop.

## Current Status
- Buildable Win32 DLL with runtime hook install/remove.
- Logger opens a console and writes to:
  - `<dll_folder>\\efz_netplay_mod.log`
- Title menu hook/patch stack is active (`netplay::InstallHooks()`):
  - Selection range expanded `0..6 -> 0..7`
  - Title dispatch switched to custom 8-entry table
  - Title render and update vtable entries are hooked
  - 8-row panel/highlight geometry patches applied
- Netplay menu entry/exit flow includes:
  - Fade-out/fade-in transition calls
  - Netplay BGM track `8` start on enter
  - BGM stop + title assets restore on leave
  - State reset for menu selection/latches/counters
- Netplay submenu model:
  - Main: `HOST`, `JOIN`, `CHANGE NICKNAME`, `RETURN TO TITLE`
  - Host: `START HOST`, editable `PORT`, `BACK`
  - Join: `CONNECT`, editable `ADDRESS`, editable `PORT`, `BACK`
  - Nickname: editable `NAME`, `BACK`
  - Canonical row slots: `0,1,2,3,4,7` (`5/6` reserved)
- Integrated Revival takeover bridge (no separate `netbridge.dll`):
  - `HostStart` / `JoinConnect` / spectate actions call bridge session start
  - Bridge spawns `EfzRevival.exe` suspended, injects this same DLL, patches IAT stubs, resumes, handshakes init, and reuses local `EfzRevival.dll`
  - Export compatibility is preserved from this DLL: `netbridge_StartNetplaySession`, `netbridge_GetStatus`, `netbridge_CancelSession`
  - Connecting state supports in-menu cancel (`ESC` / controller cancel)
- Inline editing is embedded in-menu (no extra modal input window):
  - `Enter` commits
  - `Esc` cancels edit only
  - Outside edit mode, cancel/ESC follows menu back behavior
  - Validation:
    - Port: `1..65535`
    - Address: alnum + `.:-_`
    - Nickname: printable ASCII, max 20 chars
- Rendering:
  - Preferred: sprite-glyph runtime text from object sheet + `netplay_font_map.txt`
  - Object-sheet-only mode: dynamic field values are drawn directly on menu rows
  - Optional GDI fallback overlay exists (disabled by default)

## Asset Lookup
- DLL-relative assets:
  - `<dll_folder>\\assets\\netplay_bg.dat`
  - `<dll_folder>\\assets\\netplay_ob.dat` (or fallback object candidates)
  - `<dll_folder>\\assets\\netplay_font_map.txt` (optional, template in `assets\\netplay_font_map.example.txt`)
- Object fallback order:
  - `<dll_folder>\\assets\\netplay_ob.dat`
  - `<dll_folder>\\assets\\netplay_ui_ob.dat`
  - `<dll_folder>\\assets\\config_ob.dat`
  - `system\\title_ob.dat`
- BGM path remains vanilla:
  - `wave\\bgm\\bgm08.wav`

## Source Layout
- Hooks:
  - `src/netplay/hooks/menu_hooks.cpp` (shared state + hook entrypoints/thunks)
  - `src/netplay/hooks/title_patch_install.cpp`
  - `src/netplay/hooks/title_flow.cpp`
  - `src/netplay/hooks/title_draw_layers.cpp`
  - `src/netplay/hooks/title_core.cpp`
  - `src/netplay/hooks/title_assets.cpp`
  - `src/netplay/hooks/title_overlay_text.cpp`
  - `src/netplay/hooks/title_patch_helpers.cpp`
- Shared hook internals:
  - `include/netplay/hooks/internal/shared.h`
- Core helpers:
  - `src/netplay/core/*`
- Rendering helpers:
  - `src/netplay/render/*`
- Asset parsing/pathing:
  - `src/netplay/assets/assets.cpp`

## Build (CMake / Visual Studio)
```powershell
cmake --preset release
cmake --build --preset build-release
```

Output:
- `build_release_win32/bin/Release/efz_netplay_mod.dll`
- Canonical build trees used by presets: `build_release_win32/`, `build_xp_win32/`

Notes:
- EFZ is 32-bit; always use `-A Win32`.
- Patches validate expected original bytes before writing.
- Unsupported `efz.exe` builds fail hook install safely with logs.

## XP Compatibility
- The DLL now avoids hard links against `winhttp.dll` and `dbghelp.dll`:
  - Lobby HTTP API loads dynamically at runtime (`WinHTTP`, then `WinINet` fallback).
  - Crash dump API (`MiniDumpWriteDump`) loads dynamically at runtime.
- HTTPS lobby can also use embedded TLS (mbedTLS linked statically into this DLL), so it does not depend on OS TLS support.
- This removes loader failures from those optional components on older systems, but full XP compatibility still depends on the compiler toolset runtime imports.
- Lobby note:
  - Concerto requires modern TLS (1.2+). Legacy XP Schannel cannot negotiate this reliably.
  - On XP-family systems (major version 5), the mod now auto-enables embedded TLS if no explicit backend override is configured.
  - You can still override manually with INI/env keys below.
- Embedded TLS settings:
  - Add to `EfzRevival.ini`:
```ini
[Lobby]
ForceEmbeddedTls=1
TlsVerify=0
```
  - Runtime env overrides:
    - `EFZ_LOBBY_FORCE_EMBEDDED_TLS`
    - `EFZ_LOBBY_TLS_VERIFY`
- Optional fallback proxy endpoint (automatic retry, no force flags required):
  - If the primary lobby endpoint fails, the mod retries once against `ProxyBaseUrl`.
  - Works with both `https://` and `http://` proxy endpoints.
  - Add to `EfzRevival.ini`:
```ini
[Lobby]
ProxyBaseUrl=https://your-lobby-proxy.example
```
  - Runtime env override:
    - `EFZ_LOBBY_PROXY_BASE_URL`
- Optional local proxy/bridge endpoint (XP-friendly plain HTTP):
  - Add to `EfzRevival.ini`:
```ini
[Lobby]
ProxyBaseUrl=http://127.0.0.1:17777
ForceWinInet=1
```
  - Runtime env overrides (useful for quick testing without editing INI):
    - `EFZ_LOBBY_BASE_URL`
    - `EFZ_LOBBY_FORCE_WININET`
- Use the XP preset with the XP toolset installed:
```powershell
cmake --preset xp-release
cmake --build --preset build-xp-release
```
- If `v141_xp` is missing, configure fails with `MSB8020` and XP builds cannot be produced yet.
- Current modern-toolset builds (v143/v180) still import Vista+ kernel APIs and will not load on Windows XP.

See `NETPLAY_MENU_INTEGRATION.md` for detailed reverse-engineering notes, addresses, and menu flow documentation.
