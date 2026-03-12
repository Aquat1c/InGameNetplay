# In-game Netplay (DLL)

Runtime DLL mod for EFZ that injects a `NETPLAY` entry into the title screen and replaces the old external-flow model with an in-engine netplay menu, integrated Revival takeover, room browser, lobby UI, options editor, and battle log browser.

The built DLL name remains:
- `efz_netplay_mod.dll`

## Inspiration and Reference

This project takes direct inspiration from Concerto EFZ by shiburizu:
- https://github.com/shiburizu/concerto-efz

Concerto was the main reference for:
- lobby and player-room UX
- room lifecycle and challenge flow
- backend endpoint semantics
- Revival automation expectations

This project does **not** embed Concerto itself. Instead, it reimplements the relevant online flow inside EFZ as a DLL mod with direct hook/bridge integration.

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
- `shared_documentation/` - reverse-engineering notes and implementation writeups

Useful docs:
- `NETPLAY_STATE_EXPORT.md`
- `NETPLAY_MENU_INTEGRATION.md`
- `shared_documentation/EFZ_Concerto_Lobby_Reverse_Engineering.md`
- `shared_documentation/BATTLE_LOG_MENU_ASSESSMENT.md`
- `shared_documentation/PUBLIC_ROOMS_IMPLEMENTATION_PLAN.md`
