# EFZ In-Game Netplay Mod (DLL)

Runtime DLL mod scaffold for integrating a `NETPLAY` option into EFZ's title menu.

## Current Status
- Project initialized and buildable as a Windows DLL.
- Logger opens a dedicated console window and writes logs to:
  - `<dll_folder>\\efz_netplay_mod.log`
- Title menu runtime patches are implemented in `netplay::InstallHooks()`:
  - Selection range expanded from 7 to 8 entries
  - Menu renderer geometry adjusted for 8 rows
  - Title action switch redirected to an 8-entry custom dispatch table
  - `NETPLAY` case enters an in-engine netplay submenu loop
  - Title update vtable entry is hooked so netplay submenu navigation runs each frame
- Netplay submenu asset loading:
  - `<dll_folder>\\assets\\netplay_bg.dat`
  - menu rows are wired to a canonical 8-slot `netplay_ob` lane map (`0..7`)
  - static row text in `netplay_ob.dat` is optional; runtime sprite text can populate labels/values
  - optional object lookup:
    - `<dll_folder>\\assets\\netplay_ui_ob.dat`
    - `<dll_folder>\\assets\\config_ob.dat`
    - fallback: `system\\title_ob.dat`
  - runtime styled text uses sprite-font mapping file:
    - `<dll_folder>\\assets\\netplay_font_map.txt`
    - example template: `assets\\netplay_font_map.example.txt`
  - netplay labels/values are rendered from runtime state with sprite glyph blits (no static `netplay_ob.dat` text required)
  - BGM uses vanilla pathing (`wave\\bgm\\bgm08.wav`)
- Netplay menu flow now uses a structured submenu model:
  - Main: `HOST`, `JOIN`, `CHANGE NICKNAME`, `RETURN TO TITLE`
  - Host submenu: start-host action + editable host port
  - Join submenu: connect action + editable address + editable port
  - Nickname submenu: editable nickname
  - Canonical row slots: `HOST=0`, `JOIN=1`, `NICKNAME=2`, `ADDRESS=3`, `PORT=4`, `RETURN/BACK=7` (`5/6` reserved)
  - Submenu transitions use the game's native slide helper (`performSlideAnimation`, `0x0075DA30`) with compact rows (no empty placeholders)
  - Cancel in submenus returns to Main; cancel in Main returns to title
- Editable fields use native modal dialogs with input validation:
  - Port range: `1..65535`
  - Join address: alnum plus `.`, `-`, `_`, `:`
  - Nickname: 1-20 visible ASCII chars
- Runtime text mirrors current values each frame:
  - current nickname
  - host port
  - join address/port
- In object-sheet-only mode (no sprite font map), field values are still drawn manually via overlay text on top of menu rows.
- If sprite map is missing, code can optionally fall back to plain GDI overlay (disabled by default).
- Message box helper available:
  - `EFZNetplayShowStubMessageBox(HWND owner)`
  - shows `In progress`.

See `NETPLAY_MENU_INTEGRATION.md` for reverse-engineering notes and hook targets.

## Build (Visual Studio / CMake)

Build from this folder:

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release
```

Output DLL:
- `build/bin/Release/efz_netplay_mod.dll`

Notes:
- EFZ is 32-bit, so use `-A Win32`.
- Patches are validated against expected original bytes before applying.
- If your `efz.exe` build differs, hook install will fail safely and log mismatch details.
