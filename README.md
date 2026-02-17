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
cmake -S . -B build -A Win32
cmake --build build --config Release
```

Output:
- `build/bin/Release/efz_netplay_mod.dll`

Notes:
- EFZ is 32-bit; always use `-A Win32`.
- Patches validate expected original bytes before writing.
- Unsupported `efz.exe` builds fail hook install safely with logs.

See `NETPLAY_MENU_INTEGRATION.md` for detailed reverse-engineering notes, addresses, and menu flow documentation.
