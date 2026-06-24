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

1. **Get the mod DLL and assets**
   - Build from source (see **Build** below), or download a release from [GitHub Releases](https://github.com/Aquat1c/InGameNetplay/releases).
   - You need both `efz_netplay_mod.dll` and the bundled `assets\` folder (menu backgrounds, object sheet, optional alert sound, Battle Log portraits, etc.). A release package or a local build's `assets\deploy\` folder contains the files to copy.

2. **Install EFZ Mod Manager** (if you don't have it already)
   - Download: [EFZ Mod Manager spreadsheet](https://docs.google.com/spreadsheets/d/1r0nBAaQczj9K4RG5zAVV4uXperDeoSnXaqQBal2-8Us/edit?usp=sharing)

3. **Place the mod in your game directory**

   The mod expects this layout inside your game folder (where `efz.exe` lives):

   ```text
   EFZ/                              ← your game folder
   ├── efz.exe
   ├── EfzRevival.dll                ← required for online play
   ├── EfzRevival.exe                ← required for online play
   ├── EfzRevival.ini
   ├── EfzModManager.ini             ← mod enable list (edit this)
   └── mods/
       └── efz_netplay_mod/          ← mod folder (name matches the DLL)
           ├── efz_netplay_mod.dll
           └── assets/
               ├── netplay_bgd.dat
               ├── netplay_bgn.dat
               ├── netplay_ob.dat
               ├── res_alert.wav      ← optional
               └── sprites/           ← optional Battle Log portraits
                   ├── akane.png
                   ├── akiko.png
                   └── ...
   ```

   Example full path: `EFZ\mods\efz_netplay_mod\efz_netplay_mod.dll`

   Optional mod-local overrides (same folder as the DLL):

   ```text
   mods\efz_netplay_mod\
     wave\
       bgm\
         bgm08.wav                    ← optional netplay menu BGM override
     system\
       title_ob.dat                   ← optional title object-sheet fallback
   ```

   What the bundled assets are used for:
   - `assets\netplay_bgd.dat` - daytime netplay menu background (09:00–17:59 local PC time)
   - `assets\netplay_bgn.dat` - nighttime netplay menu background (18:00–08:59)
   - `assets\netplay_ob.dat` - netplay menu object/title-sheet UI graphics
   - `assets\res_alert.wav` - custom lobby challenge alert sound
   - `assets\sprites\*.png` - Battle Log character portraits
   - `wave\bgm\bgm08.wav` - optional menu BGM override with proper loop info
   - `system\title_ob.dat` - optional title object-sheet override

   Fallback behavior:
   - If `assets\netplay_ob.dat` is missing, the mod tries other object-sheet candidates and eventually falls back to vanilla `system\title_ob.dat`.
   - If `wave\bgm\bgm08.wav` is missing, the mod falls back to vanilla `wave\bgm\bgm08.wav`.
   - Under Wine / Proton, the same files are also searched through mod-relative fallback paths.

4. **Enable the mod in `EfzModManager.ini`**

   Open `EfzModManager.ini` in your **game folder** (next to `efz.exe`, not inside `mods/`) and add:

   ```ini
   efz_netplay_mod=1
   ```

   If other mods are already listed, add this line alongside them.

5. **Launch the game**

   Launch through **`efz.exe`**. EFZ Mod Manager loads enabled mods automatically on startup.

   > **Important:** Do **not** start the game through `EfzRevival.exe` or `Concerto.exe` when this mod is enabled. InGameNetplay expects to be injected into the main game process and can crash if those executables are used as the entry point.

   After a successful install, a new **`NETPLAY`** option should appear on the title screen.

**First Run**
- Open **NETPLAY** from the title screen to reach the in-game host/join/lobby flow.
- The mod writes `efz_netplay_mod.log` next to the DLL (`mods\efz_netplay_mod\`). File logging can be toggled from **Options → Others** in the netplay menu.
- Nickname, port, and most online settings are read from / saved to `EfzRevival.ini` in the game folder. You do not need to drive the old Revival console window during normal use.
- A `native_host\` subfolder may appear under the mod folder for captured Revival-side logs during host sessions.

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
- Integrated Revival takeover bridge
- Host / Join / Spectate session startup from the in-game menu
- Delay prompt overlay and connected-session handoff back into EFZ
- **Async hosting** - start a host listener, minimize the overlay, and keep using EFZ while waiting (see **Async Hosting** below)
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
  - `AsyncHostReturnKey` - hotkey to return to the HOST menu (or rehost) while async hosting is minimized (default: `F1`)
- `About` modal with version/build information

Netplay menu theming:
- The menu background theme is currently read from `EfzRevival.ini` under `[NetplayMenu]` with `Theme=scroll` or `Theme=classic`.
- The active background is selected from local PC time: `assets\netplay_bgd.dat` from 09:00 through 17:59, and `assets\netplay_bgn.dat` from 18:00 through 08:59.
- The `scroll` theme uses a wrapped horizontal pan and expects the selected background DAT to decode as `320x240`; unsupported background sizes fall back to the classic static draw.

Logging and diagnostics:
- Logger banner includes version and build timestamp
- Optional console and optional file logging
- Crash handler writes crash logs / diagnostics

## Async Hosting

Async hosting lets you **start a host session and keep playing EFZ** while the listener waits for an opponent. The host process stays alive in the background; you are not stuck on the hosting overlay.

### Quick flow

```text
NETPLAY → Host → start hosting
→ full HOSTING panel shows your IP / port
→ D: minimize — listener stays open, small badge appears
→ browse the netplay menu, leave to title, or play offline/practice
→ opponent connects — badge changes to OPPONENT FOUND!
→ return to the full HOST overlay (select Host, or press F1 from outside the menu)
→ normal delay setup runs, then charselect / match as usual
```

### While waiting for an opponent

- The full **HOSTING** panel shows your public IP and port (press **C** to copy).
- **D** minimizes hosting: the overlay collapses to a small top-right badge (`HOSTING`) and you return to the main netplay menu. The listener **stays active**.
- You can browse **Battle Log**, **Options**, and other netplay pages, or leave the netplay menu entirely — hosting is **not** cancelled when you exit the menu.
- Select **Host** again (or re-enter NETPLAY) to restore the full hosting panel.

### When an opponent connects

- The mod **holds** Revival at the delay prompt instead of jumping straight into the delay overlay.
- The badge updates to **OPPONENT FOUND!**
- Once the full HOST overlay is visible again, accept happens automatically and the normal delay-setup / handoff flow continues.
- If you are outside the netplay menu (title screen, practice, etc.), press the **return hotkey** (default **F1**) to drive back to the HOST menu; a held opponent is accepted on arrival.

### Controls and settings

| Action | Input |
|---|---|
| Minimize hosting (keep listening) | **D** on the full HOSTING panel |
| Cancel hosting | **B** / **Esc** on the full HOSTING panel |
| Copy IP:port | **C** on the full HOSTING panel |
| Return to HOST menu from gameplay / title | **F1** (default; configurable) |

Configure the return hotkey under **Options → Others → AsyncHostReturnKey** in the netplay menu (saved to `EfzRevival.ini` as a `DIK_*` keyboard binding).

### Indicators

- **Inside the netplay menu** (minimized): a small indexed badge in the top-right — `HOSTING`, `OPPONENT FOUND!`, or `TIMED OUT`.
- **Outside the netplay menu** (minimized): a top-center on-screen message with the same state (e.g. `Hosting... Press F1 to return to HOST menu`).

### Conflicts and cancellation

- Starting **Join**, **Lobby**, or **Player Rooms** while async hosting is active shows a **Stop hosting?** confirmation. Choose **Stop hosting** to proceed, or **Keep hosting** to stay listening.
- **B** / **Esc** on the full hosting panel cancels the listener.
- If a connected opponent drops before you accept, the mod **auto-rehosts** on the same port so you return to waiting without manual restart.

Companion mods can read async-host state through the shared export (`EFZ_CAP_ASYNC_HOST`: active, minimized, peer found, timed out, host port). See **Shared Netplay Exports** below.

## How Revival Is Used

In-game Netplay still relies on `EfzRevival` as the underlying online/session
backend, but it hides the old external console-driven workflow from the player.

In practical terms, the mod:
- starts and controls `EfzRevival` in the background when a netplay action needs it
- uses Revival for host, join, spectate, delay setup, and online session handling
- synchronizes important settings such as nickname and port with `EfzRevival.ini`
- reads Revival status/errors and converts them into in-game overlays and menu flow
- returns you to the in-game netplay UI after cancel, disconnect, spectate end, or match end

What this means for a regular player:
- you still need a supported `EfzRevival` installation in your EFZ folder
- you do **not** need to manually drive the old Revival console flow during normal use
- the in-game menu is the intended front end, while Revival runs behind it
- always launch through `efz.exe`, not `EfzRevival.exe` or `Concerto.exe`

## Supported Revival Versions

Supported `EfzRevival.dll` versions:
- `1.02e`
- `1.02f`
- `1.02g`
- `1.02h!!!`
- `1.02i!!!`

Notes:
- `1.02h!!!` and `1.02i!!!` are the most tested versions.
- Unsupported Revival builds fail safely with log output instead of applying unknown hooks.

## Shared Netplay Exports

In-game Netplay exposes a stable shared-state interface for companion mods and
tools that run inside the same `EFZ.exe` process.

Public interface:
- Header: `include/efz_netplay_state.h`
- Named shared memory block: `EFZNetplay_State`
- DLL export: `EFZNetplay_GetState()`
- Current ABI version: `6`

Consumer expectations:
- Validate `magic == EFZ_NETPLAY_STATE_MAGIC`
- Check `version` against the maximum layout your consumer supports
- Use `structSize` for forward-compatible reads
- Check `capabilityFlags` before assuming a field group is populated

Exported state currently includes:
- session mode, phase, and local side
- set score and match counter
- local, P1, and P2 nicknames
- ping and rollback/input-delay metrics
- active netplay menu screen and detail subview
- detected Revival version
- activity phase and last end reason
- online character-select state
- match context such as stage, round index, and timer
- async hosting state (listener active, minimized, peer found, timed out, host port)

The shared state is refreshed continuously while the mod is active, which makes
it suitable for rich presence, overlays, stream tooling, and companion mods.

## Runtime Assets

DLL-relative assets:
- `<dll_folder>\\assets\\netplay_bgd.dat`
- `<dll_folder>\\assets\\netplay_bgn.dat`
- `<dll_folder>\\assets\\netplay_ob.dat`
- `<dll_folder>\\assets\\res_alert.wav`
- `<dll_folder>\\assets\\netplay_font_map.txt` (optional)
- `<dll_folder>\\assets\\sprites\\*.png` (Battle Log portraits)

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
- Some builds/packages may include a replacement track. Any such third-party music is not covered by this project's MIT license, and rights to music from `ONE.` or any other third-party title remain with their original copyright holders.

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

Result:
- The selected preset produces `efz_netplay_mod.dll`
- Both standard and XP-compatible builds are supported

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

## Copyright and Licenses

Project status:
- The original project-authored source code and documentation are released under the MIT License.
- That license is intentionally scoped. It applies to the project's own source/docs, not to every file in this repository.
- See the top-level `LICENSE` and `NOTICE` files for scope and attribution requirements.

Third-party code used by this project:
- Mbed TLS is vendored under `third_party/mbedtls` and is provided under a dual `Apache-2.0` or `GPL-2.0-or-later` license.
  See: `third_party/mbedtls/LICENSE`
- MinHook is used for the D3D9/Battle Log hook path and is expected from `third_party/minhook` or `../InGameControlsRebind/third_party/minhook` depending on the checkout.
  Its license is the BSD-style license distributed with MinHook in `LICENSE.txt`.

Other targets:
- EFZ, EfzRevival, and their original binaries/assets are not part of this project's licensing.
- Files such as `EfzRevival.dll`, `EfzRevival.exe`, `efz.exe`, and original game art/audio remain under their respective owners' rights.
- If a release, package, or local install includes music derived from `ONE.`, that audio is third-party material and is not covered by this project's MIT license. The official `ONE.` site lists the work as `© 2022 NEXTON/novamicus`.
