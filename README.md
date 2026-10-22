# In-game Netplay

An in-game netplay mod for **Eternal Fighter Zero -Memorial- with Revival**.

EFZ's community netplay runs on EfzRevival, a program that normally controls
the game from a console window outside the game. This mod moves all of that
inside: it adds a **NETPLAY** entry to the title screen with its own menus,
handles hosting, joining, spectating, lobbies and options in-game, and
answers EfzRevival's console questions for you from overlays. The proven
EfzRevival rollback netcode keeps doing the actual online simulation -
unchanged - but you never see the console.

The built DLL name is `efz_netplay_mod.dll`.

## Inspiration and reference

This project takes direct inspiration from [Concerto EFZ](https://github.com/shiburizu/concerto-efz)
by shiburizu - room lifecycle, challenge flow, backend endpoint semantics
and Revival automation expectations. Concerto itself is not embedded; the
online flow is reimplemented inside EFZ as a DLL mod with direct hook/bridge
integration.

## Installation

1. **Get the mod DLL and assets**
   - Build from source (see **Build** below), or download a release from
     [GitHub Releases](https://github.com/Aquat1c/InGameNetplay/releases).
   - You need both `efz_netplay_mod.dll` and the bundled `assets\` folder
     (menu backgrounds, object sheet, optional alert sound, Battle Log
     portraits). A release package or a local build's `assets\deploy\`
     folder contains the files to copy.

2. **Install EFZ Mod Manager** (if you don't have it already)
   - Download: [EFZ Mod Manager spreadsheet](https://docs.google.com/spreadsheets/d/1r0nBAaQczj9K4RG5zAVV4uXperDeoSnXaqQBal2-8Us/edit?usp=sharing)

3. **Place the mod in your game directory**

   The mod expects this layout inside your game folder (where `efz.exe` lives):

   ```text
   EFZ/                              <- your game folder
   |-- efz.exe
   |-- EfzRevival.dll                <- required for online play
   |-- EfzRevival.exe                <- required for online play
   |-- EfzRevival.ini
   |-- EfzModManager.ini             <- mod enable list (edit this)
   `-- mods/
       `-- efz_netplay_mod/          <- mod folder (name matches the DLL)
           |-- efz_netplay_mod.dll
           `-- assets/
               |-- netplay_bgd.dat
               |-- netplay_bgn.dat
               |-- netplay_ob.dat
               |-- res_alert.wav     <- optional
               `-- sprites/          <- optional Battle Log portraits
                   |-- akane.png
                   |-- akiko.png
                   `-- ...
   ```

   Example full path: `EFZ\mods\efz_netplay_mod\efz_netplay_mod.dll`

   Optional mod-local overrides (same folder as the DLL):

   ```text
   mods\efz_netplay_mod\
     wave\bgm\bgm08.wav              <- optional netplay menu BGM override
     system\title_ob.dat             <- optional title object-sheet fallback
   ```

   What the bundled assets are used for:
   - `assets\netplay_bgd.dat` - daytime netplay menu background (09:00-17:59
     local PC time)
   - `assets\netplay_bgn.dat` - nighttime netplay menu background
     (18:00-08:59)
   - `assets\netplay_ob.dat` - netplay menu object/title-sheet UI graphics
   - `assets\res_alert.wav` - custom lobby challenge alert sound
   - `assets\sprites\*.png` - Battle Log character portraits
   - `wave\bgm\bgm08.wav` - optional menu BGM override with proper loop info
   - `system\title_ob.dat` - optional title object-sheet override

   Fallback behavior:
   - If `assets\netplay_ob.dat` is missing, the mod tries other object-sheet
     candidates and eventually falls back to vanilla `system\title_ob.dat`.
   - If `wave\bgm\bgm08.wav` is missing, the mod falls back to vanilla
     `wave\bgm\bgm08.wav`.
   - Under Wine / Proton, the same files are also searched through
     mod-relative fallback paths.

4. **Enable the mod in `EfzModManager.ini`**

   Open `EfzModManager.ini` in your **game folder** (next to `efz.exe`, not
   inside `mods/`) and add:

   ```ini
   efz_netplay_mod=1
   ```

   If other mods are already listed, add this line alongside them.

5. **Launch the game**

   You can launch through either `efz.exe` or an exact supported
   `EfzRevival.exe` + `EfzRevival.dll` pair; EFZ Mod Manager loads the mod
   into the resulting `efz.exe` process either way.

   - `efz.exe` is the normal entry point for the integrated in-game menu.
   - When launched through `EfzRevival.exe`, the mod detects and adopts the
     already-created Online/Spectator/Tournament session instead of calling
     Revival's exported `init` a second time. Modified, mismatched, or
     unsupported launcher/DLL pairs are left alone - the mod stays passive
     and logs why.
   - Launcher-first Tournament attachment has source/byte-level support for
     the exact 1.02e through 1.02j pairs; live testing has exercised 1.02j
     only so far. Treat the other versions, and the automated natural
     return, as not yet live-qualified.
   - `Concerto.exe` is not a supported entry point.

   After a successful install, a new **NETPLAY** option appears on the title
   screen.

**First run**
- Open **NETPLAY** from the title screen to reach the in-game
  host/join/lobby flow.
- The mod writes `efz_netplay_mod.log` next to the DLL (under
  `mods\efz_netplay_mod\`). File logging can be toggled from
  **Options -> Others**.
- Nickname, port, and most online settings are read from / saved to
  `EfzRevival.ini` in the game folder. You do not need to drive the old
  Revival console window during normal use.
- A `native_host\` subfolder may appear under the mod folder for captured
  Revival-side logs during host sessions.

## Current feature set

Top-level menu: `Host`, `Join`, `Player Rooms`, `Lobby`, `Battle Log`,
`Options`, `Return to Title`.

Online flow:
- Host / Join / Spectate session startup from the in-game menu, with
  delay-prompt overlay and handoff back into EFZ
- **Async hosting** - start a host listener, minimize the overlay, and keep
  using EFZ while waiting (see **Async Hosting** below)
- Cancel / disconnect / recovery paths back into the netplay menu
- Runtime state export for companion mods/tools

Lobby and rooms:
- Global `EFZ` lobby browser, public/private rooms under Player Rooms
- Create / join / refresh room flow, join by room code
- Challenge / accept / spectate flow with an alert sound
- Async networking for room lists and joins so the UI never hitches

Battle Log:
- In-game parser and browser for `BattleLog.txt`
- Summary / browser / filter / set-detail views, win/loss statistics
- Character portrait rendering in browser/detail views

Options:
- Dynamic in-game editor for `EfzRevival.ini` (see **Options editor** in
  [How it works](docs/HOW_IT_WORKS.md))
- Category pages, tooltips, key and pad rebinding with conflict checks
- Mod-owned `Others` settings such as `OfflineVsHumanMode`, `WriteLogFile`,
  `EnableConsole`, `EnableDebugMenu`, `OnlineCustomColors`,
  `HideEmptySetsInBattleLog`, `AsyncHostReturnKey` (default `F1`), and
  `CheckForUpdates` (shows a `[!]` badge on OPTIONS when a newer release is
  out)
- `About` modal with version/build information and the newest GitHub release

Netplay menu theming:
- Background theme read from `EfzRevival.ini` under `[NetplayMenu]`:
  `Theme=scroll` (wrapped horizontal pan, expects a 320x240 background) or
  `Theme=classic` (static draw)
- Day (`netplay_bgd.dat`, 09:00-17:59) / night (`netplay_bgn.dat`,
  18:00-08:59) backgrounds by local PC time
- Optional crisp TrueType text layer (`MenuTtfText`, `MenuTtfFont` under
  `[Others]`)

Logging and diagnostics:
- Logger banner includes version and build timestamp
- Optional console and file logging; crash handler writes crash logs and
  minidumps under `logs\`

## Async hosting

Async hosting lets you start a host session and keep playing EFZ while the
listener waits for an opponent. You are not stuck on the hosting overlay.

```text
NETPLAY -> Host -> start hosting
-> full HOSTING panel shows your IP / port
-> D: minimize - listener stays open, small badge appears
-> browse the netplay menu, leave to title, or play offline/practice
-> opponent connects - badge changes to OPPONENT FOUND!
-> return to the full HOST overlay (select Host, or press F1 from outside the menu)
-> normal delay setup runs, then charselect / match as usual
```

While waiting:
- The full **HOSTING** panel shows your public IP and port (press **C** to
  copy).
- **D** minimizes hosting to a small top-right badge; the listener stays
  active. Hosting is not cancelled when you exit the menu.
- Select **Host** again (or re-enter NETPLAY) to restore the full panel.

When an opponent connects:
- The mod holds Revival at the delay prompt instead of showing it, and the
  badge updates to **OPPONENT FOUND!**
- Once the full HOST overlay is visible again, accept happens automatically.
- From outside the netplay menu, press the return hotkey (default **F1**) to
  drive back to the HOST menu; a held opponent is accepted on arrival.
- If a connected opponent drops before you accept, the mod auto-rehosts on
  the same port so you are listable again without a manual restart.

| Action | Input |
|---|---|
| Minimize hosting (keep listening) | **D** on the full HOSTING panel |
| Cancel hosting | **B** / **Esc** on the full HOSTING panel |
| Copy IP:port | **C** on the full HOSTING panel |
| Return to HOST menu from gameplay / title | **F1** (default; configurable) |

Configure the return hotkey under **Options -> Others -> AsyncHostReturnKey**
(saved to `EfzRevival.ini` as a `DIK_*` keyboard binding). Starting Join,
Lobby, or Player Rooms while async hosting is active asks **Stop
hosting?** first.

Companion mods can read async-host state through the shared export
(`EFZ_CAP_ASYNC_HOST`).

## How Revival is used

The mod still relies on `EfzRevival` as the underlying online/session
backend - it just hides the external console workflow:

- it starts and controls EfzRevival in the background when a netplay action
  needs it, and uses it for host, join, spectate, delay setup, and online
  session handling
- it reads Revival status/errors and turns them into in-game overlays and
  menu flow, and returns you to the in-game UI after cancel, disconnect,
  spectate end, or match end
- it synchronizes settings such as nickname and port with `EfzRevival.ini`

For a regular player this means: you still need a supported EfzRevival
installation in your EFZ folder, and the in-game menu is the intended front
end. How the takeover bridge works internally - the helper process, console
interception, exit interception, teardown rules - is described in
[How it works](docs/HOW_IT_WORKS.md).

## Supported Revival versions

Supported `EfzRevival.dll` versions: `1.02e`, `1.02f`, `1.02f-framestepping`,
`1.02g`, `1.02h`, `1.02i`, `1.02j`.

- `1.02h` and `1.02i` remain the most exercised legacy builds.
- `1.02j` uses its own MinGW-specific object, vtable, and lifecycle profile;
  the newer renderer-split `1.02j` build requires its bundled `Ddraw.dll`.
- Launcher-first admission requires the catalogued exact EXE/DLL fingerprint
  for the selected version; unsupported or modified builds stay passive with
  log output instead of receiving unknown hooks.
- After a committed launcher-first Online/Spectator session ends, the exact
  admitted `EfzRevival.exe` parent is terminated and its exit confirmed
  before another online attempt is admitted, so Revival's native singleton
  is released cleanly.
- Tournament option-6 source and byte contracts cover every version listed
  above, but live validation currently covers `1.02j` only.

## Shared netplay exports

Companion mods and tools inside the same `efz.exe` process can read live
netplay state:

- Header: `include/efz_netplay_state.h` (the contract)
- Named shared memory block: `EFZNetplay_State`
- DLL export: `EFZNetplay_GetState()`

Validate `magic` and `version`, check `capabilityFlags` before assuming a
field group is populated, and use `stateSeq` / `lastUpdateTick` to judge
freshness. Exported state includes session mode/phase, set score, nicknames,
ping and rollback metrics, the active menu screen, detected Revival version,
character-select state, match context, and async hosting state. Battle
metrics are published from the menu side, so during a match they hold the
last pre-handoff values until control returns.

## Lobby backend notes

The lobby follows Concerto-style backend semantics over HTTPS:

- Native Windows auto-prefers the modern Windows HTTP stack, then WinINet
- Wine / Proton and Windows XP auto-prefer the embedded TLS library
- Optional base/proxy overrides via `EfzRevival.ini` and environment
  variables

```ini
[Lobby]
BaseUrl=
ProxyBaseUrl=
ForceWinInet=0
ForceEmbeddedTls=0
TlsVerify=0
```

Environment overrides: `EFZ_LOBBY_BASE_URL`, `EFZ_LOBBY_PROXY_BASE_URL`,
`EFZ_LOBBY_FORCE_WININET`, `EFZ_LOBBY_FORCE_EMBEDDED_TLS`,
`EFZ_LOBBY_TLS_VERIFY`.

## Build

Initialize the tracked mbedTLS dependency after cloning:

```powershell
git submodule update --init --recursive
```

MinHook must be available at `third_party/minhook` or in the sibling
`../InGameControlsRebind/third_party/minhook` checkout.

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

Strict native-tick parity build (A/B check that leaves Revival's native
per-frame tick target untouched):

```powershell
cmake --preset xp-native-tick
cmake --build --preset build-xp-native-tick
```

Notes:
- EFZ is 32-bit; all presets target `Win32`
- Hook/patch install validates expected bytes before patching; unsupported
  EFZ / Revival builds fail safely with log output

## Repository layout

- `src/netplay/hooks/` - title/menu hooks, overlays, flow control, bridge handoff
- `src/netplay/core/` - menu models, lobby client, inline edit, validation, settings
- `src/netplay/render/` - indexed-surface drawing helpers, software font, overlays
- `src/netplay/assets/` - DAT parsing, runtime asset resolution
- `src/netplay/bridge/` - Revival takeover, IPC, exports, process/session bridge
- `src/netplay/interop/` - mod-interop channel, online custom colors
- `include/` - public/internal headers
- `tests/` - C++ contract tests and Python source-contract checks

## Copyright and licenses

- The original project-authored source code and documentation are released
  under the MIT License. That license is intentionally scoped: it applies to
  the project's own source/docs, not to every file in this repository. See
  the top-level `LICENSE` and `NOTICE` files for scope and attribution
  requirements.
- Mbed TLS is vendored under `third_party/mbedtls` (dual `Apache-2.0` or
  `GPL-2.0-or-later`).
- MinHook is used for the D3D9/Battle Log hook path (BSD-style license
  distributed with MinHook).
- EFZ, EfzRevival, and their original binaries/assets are not part of this
  project's licensing. If a release package includes music derived from
  `ONE.`, that audio is third-party material and is not covered by this
  project's MIT license (`© 2022 NEXTON/novamicus`).

---

## For modders and developers

- [How it works](docs/HOW_IT_WORKS.md): a tour of what the
  mod does inside the game - the takeover bridge, session lifecycle,
  determinism rules, lobby, custom colors, state export, and the debug
  probe.
- [DisplayManager reset contract](docs/displaymanager-reset-contract.md):
  how overlay mods cooperate when the display resets
  (`EFZ_QueryOverlayResetApiV1`).
- `include/efz_netplay_state.h`: the shared-state ABI for companion tools.
- `tests/`: the contract tests; keep them passing.

### Comment discipline

Short, load-bearing comments only. Explanations of how subsystems work
belong in `docs/`, not in the code.
