# In-game Netplay (DLL)

Runtime DLL mod for EFZ that injects a `NETPLAY` entry into the title screen and replaces the old external-flow model with an in-engine netplay menu, integrates all EfzRevival.exe console window functionality into the game, adds room browser, lobby UI, options editor, and battle log browser.

The built DLL name is:
- `efz_netplay_mod.dll`

## Inspiration and Reference

This project takes direct inspiration from Concerto EFZ by shiburizu:
- https://github.com/shiburizu/concerto-efz

Concerto was the main reference for:
- room lifecycle and challenge flow
- backend endpoint semantics
- Revival automation expectations

This project does **not** embed Concerto itself. Instead, it reimplements the relevant online flow inside EFZ as a DLL mod with direct hook/bridge integration.

## Installation

- Build the DLL from source (see Building) or download a release.
- Install EFZ Mod Manager if it's not installed already:
  - EFZ Mod Manager download: https://docs.google.com/spreadsheets/d/1r0nBAaQczj9K4RG5zAVV4uXperDeoSnXaqQBal2-8Us/edit?usp=sharing
- Place `efz_netplay_mod.dll` in your EFZ mods folder, alongside the other mod assets.
  Example path:
  `EFZ\\mods\\efz_netplay_mod\\efz_netplay_mod.dll`
- Edit `EfzModManager.ini` and add:
  - `efz_netplay_mod=1`
- After installing, a new `NETPLAY` option should appear on the title screen.

## Expected Mod Folder Layout

At minimum, the mod expects this structure next to the DLL:

```text
mods\efz_netplay_mod\
  efz_netplay_mod.dll
  assets\
    netplay_bg.dat
    netplay_ob.dat
```

Common optional files:

```text
mods\efz_netplay_mod\
  assets\
    battle_log_icons.dat
    res_alert.wav
    netplay_font_map.txt
  wave\
    bgm\
      bgm08.wav
  system\
    title_ob.dat
```

What they are used for:
- `assets\netplay_bg.dat` - netplay menu background
- `assets\netplay_ob.dat` - netplay menu object/title-sheet UI graphics
- `assets\battle_log_icons.dat` - indexed Battle Log icon sheet
- `assets\res_alert.wav` - custom lobby challenge alert sound
- `assets\netplay_font_map.txt` - optional sprite-font mapping
- `wave\bgm\bgm08.wav` - optional netplay menu BGM override
- `system\title_ob.dat` - optional title object-sheet override

Fallback behavior:
- If `assets\netplay_ob.dat` is missing, the mod tries other object-sheet candidates and eventually falls back to vanilla `system\title_ob.dat`.
- If `wave\bgm\bgm08.wav` is missing, the mod falls back to vanilla `wave\bgm\bgm08.wav`.
- Under Wine / Proton, the same files are also searched through mod-relative fallback paths.

## Current Feature Set

Top-level menu:
- `Host`
- `Join`
- `Player Rooms`
- `Lobby`
- `Battle Log`
- `Options`
- `Return to Title`

Online flow:
- Integrated Revival takeover bridge; no separate `netbridge.dll`
- Host / Join / Spectate session startup from the in-game menu
- Delay prompt overlay and connected-session handoff back into EFZ
- Cancel / disconnect / recovery paths back into the netplay menu
- Runtime state export for companion mods/tools

Lobby and rooms:
- Global `EFZ` lobby browser
- Public/private room flow under `Player Rooms`
- Create / join / refresh room flow
- Challenge / accept / spectate flow
- Async room-list, room-create, and room-join networking to avoid UI hitches
- Deferred re-entry / refresh guards so room presence is not broadcast too early during recovery

Battle Log:
- In-game parser and browser for `BattleLog.txt`
- Summary / browser / filter / set-detail views
- Search/filter by player, opponent, characters, set type, game count, and character changes
- Character portrait rendering in browser/detail views

Options:
- Dynamic in-game editor for `EfzRevival.ini`
- Category-based options UI
- Key and pad rebinding flow
- Mod-owned `Others` settings such as:
  - `OfflineVsHumanMode`
  - `WriteLogFile`
  - `EnableConsole`
  - `EnableDebugMenu`
  - `HideEmptySetsInBattleLog`
- `About` modal with version/build information

Logging and diagnostics:
- Logger banner includes version and build timestamp
- Optional console and optional file logging
- Crash handler writes crash logs / diagnostics

## Supported Revival Versions

Supported `EfzRevival.dll` versions:
- `1.02e`
- `1.02f`
- `1.02g`
- `1.02h`
- `1.02i`

Notes:
- `1.02h` and `1.02i` are the most tested versions.
- Unsupported Revival builds fail safely with log output instead of applying unknown hooks.

## Runtime Assets

DLL-relative assets:
- `<dll_folder>\\assets\\netplay_bg.dat`
- `<dll_folder>\\assets\\netplay_ob.dat`
- `<dll_folder>\\assets\\battle_log_icons.dat`
- `<dll_folder>\\assets\\res_alert.wav`
- `<dll_folder>\\assets\\netplay_font_map.txt` (optional)

Object fallback order:
- `<dll_folder>\\assets\\netplay_ob.dat`
- `<dll_folder>\\assets\\netplay_ui_ob.dat`
- `<dll_folder>\\assets\\config_ob.dat`
- `system\\title_ob.dat`

BGM override:
- Vanilla track used by the menu: `wave\\bgm\\bgm08.wav`
- Mod-local override supported from:
  - `<dll_folder>\\wave\\bgm\\bgm08.wav`
  - mod-relative fallback paths under Wine / Proton

Linux / Wine / Proton notes:
- Menu/background `.dat` assets are resolved through mod-relative fallback paths
- Wine / Proton auto-select embedded TLS for lobby reliability
- BGM override probing also supports mod-local relative paths under Wine / Proton

## Build

Configure and build:

```powershell
cmake --preset release
cmake --build --preset build-release
```

XP-compatible build:

```powershell
cmake --preset xp-release
cmake --build --preset build-xp-release
```

Output:
- `build_release_win32/bin/Release/efz_netplay_mod.dll`
- `build_xp_win32/bin/Release/efz_netplay_mod.dll`

Notes:
- EFZ is 32-bit; builds target `Win32`
- Hook/patch install validates expected bytes before patching
- Unsupported EFZ / Revival builds fail safely with log output

## Lobby Backend Notes

The lobby implementation follows Concerto-style backend semantics.

Current behavior:
- Primary lobby flow uses the Concerto-style HTTPS backend
- Legacy Windows auto-prefers `WinINet`
- Wine / Proton auto-prefers embedded TLS
- Optional base/proxy overrides are available through `EfzRevival.ini` and environment variables

Relevant INI keys:

```ini
[Lobby]
BaseUrl=
ProxyBaseUrl=
ForceWinInet=0
ForceEmbeddedTls=0
TlsVerify=0
```

Relevant environment overrides:
- `EFZ_LOBBY_BASE_URL`
- `EFZ_LOBBY_PROXY_BASE_URL`
- `EFZ_LOBBY_FORCE_WININET`
- `EFZ_LOBBY_FORCE_EMBEDDED_TLS`
- `EFZ_LOBBY_TLS_VERIFY`

## Repository Layout

Core areas:
- `src/netplay/hooks/` - title/menu hooks, overlays, flow control, bridge handoff
- `src/netplay/core/` - menu models, lobby client, inline edit, validation, settings
- `src/netplay/render/` - indexed-surface drawing helpers, software font, overlays
- `src/netplay/assets/` - DAT parsing, runtime asset resolution
- `src/netplay/bridge/` - Revival takeover, IPC, exports, process/session bridge
- `include/` - public/internal headers
- `shared_documentation/` - reverse-engineering notes, Concerto references, and implementation writeups

## Copyright and Licenses

Project status:
- This repository currently does not include a top-level license file for the In-game Netplay project itself.
- Unless and until one is added, do not assume the project source is released under a standalone open-source license.

Third-party code used by this project:
- Mbed TLS is vendored under `third_party/mbedtls` and is provided under a dual `Apache-2.0` or `GPL-2.0-or-later` license.
  See: `third_party/mbedtls/LICENSE`
- MinHook is used for the D3D9/Battle Log hook path and is expected from `third_party/minhook` or `../InGameControlsRebind/third_party/minhook` depending on the checkout.
  Its license is the BSD-style license distributed with MinHook in `LICENSE.txt`.

Game assets and reverse-engineered targets:
- EFZ, EfzRevival, and their original binaries/assets are not part of this project's licensing.
- Files such as `EfzRevival.dll`, `EfzRevival.exe`, `efz.exe`, and original game art/audio remain under their respective owners' rights.
- Users are expected to provide their own legally obtained game/mod files.
