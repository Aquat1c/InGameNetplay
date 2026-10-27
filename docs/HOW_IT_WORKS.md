# How In-game Netplay works

A tour of what the mod does inside the game, for curious users and modders.
No memory addresses - where the code needs them, the pointers below lead to
the files that own them. For install and everyday use, see the
[README](../README.md).

Code references look like `EnsureLocalRevivalLoaded()`
(`src/netplay/bridge/ipc_shared.cpp`) - function or type first, then the file
that owns it. Line numbers move; names do not.

## The short version

EFZ's community netplay runs on **EfzRevival**, an external program that
normally controls the game through a console window: you launch it, answer
prompts in a black box, and it drives the game from outside. This mod moves
all of that inside the game: a **NETPLAY** entry on the title screen, its own
menus, EfzRevival started and controlled for you, its console questions
answered from in-game overlays, and clean returns to the menu when a session
ends. The rollback netcode itself stays EfzRevival's, unchanged.

## Repository map

| Area | What lives there |
| --- | --- |
| `src/netplay/bridge/` | The Revival takeover: process bridge, IPC, exit interception, state export |
| `src/netplay/hooks/` | Title-screen menu, flow control, options editor, battle log |
| `src/netplay/core/` | Menu models, lobby client, HTTP, validation, update check |
| `src/netplay/render/` | Menu drawing, software font, sprite font map, ImGui overlay |
| `src/netplay/interop/` | Mod-interop channel and online custom colors |
| `src/netplay/assets/` | DAT parsing and runtime asset resolution |
| `include/netplay/bridge/revival_addresses.h` | Per-build profiles of EfzRevival internals |
| `include/efz_netplay_state.h` | The public shared-state ABI |
| `include/compat/` | The overlay reset cooperation API |
| `tests/` | Contract tests (C++ and Python) - run them before pushing |

## One game process, two roles

When you start a host, join, or spectate session, the mod quietly starts a
second copy of the game - the **helper** - and loads itself into it. Your
real game then takes over the session. The split:

- `StartSession()` (`src/netplay/bridge/revival_takeover.cpp`) is the front
  door: it spawns the helper suspended, injects the mod DLL
  (`src/netplay/bridge/process_inject.cpp`), patches the helper's import
  table so the Windows calls EfzRevival makes - process creation, remote
  threads, memory writes, console I/O - land in the mod's stand-ins
  (`src/netplay/bridge/iat_stubs.cpp`), and only then resumes the helper.
- `Tick()` (`src/netplay/bridge/revival_takeover.cpp`) is the per-frame
  control plane: it drives the session state machine, watches the helper's
  health, and publishes status for the menus
  (`NetbridgePhase`/`NetbridgeRole` in
  `include/netplay/bridge/session_bridge.h`).
- `session_bridge.cpp` in the same directory is the menu-side wrapper:
  `Initialize()` wires everything up, `SetPhase()` owns state transitions,
  `IsCurrentProcessRevival()` / `IsRunningUnderWine()` decide role-specific
  behavior at load time.
- The two sides talk through a shared-memory mailbox where every message
  carries a rising serial number, so "new message" vs "already handled"
  needs no locks (`src/netplay/bridge/ipc_shared.cpp`; the wire format is
  documented in `include/netplay/interop/overlay_ipc.h`).

Everything EfzRevival prints is captured and parsed
(`src/netplay/bridge/console_capture.cpp`): `ExtractDelayRange()` and
`ExtractIntAfterToken()` read the delay prompt and its ping statistics,
`TryExtractQuitEndpoint()` / `TryExtractDiedEndpoint()` recognize opponents
leaving, and the whole stream is mirrored into an append-only `logEfz.txt`
with one header per session (`RegisterCreatedConsoleFile*()` in
`src/netplay/bridge/iat_stubs.cpp` owns the file-handle bookkeeping).

## Supported builds, and failing closed

EfzRevival's internals differ between versions, so nearly every internal
location is version-specific. The mod never guesses:

- `include/netplay/bridge/revival_addresses.h` holds one
  `RevivalAddressProfile` per known build (1.02e through 1.02j), including
  the exact launcher/companion-DLL identity each build admits.
- `DetectRevivalVersion()` and `EnsureActiveRevivalProfile()`
  (`src/netplay/bridge/revival_takeover.cpp`) fingerprint the loaded DLL and
  pick the matching profile; an unknown or modified build gets the
  zeroed `kRevival_Unsupported` profile and no hooks.
- Every patch site is byte-checked before patching and restored only if the
  bytes still match (`src/netplay/core/patch_utils.cpp`).
- Launching through EfzRevival's own launcher adds stricter checks:
  `EnsureLocalRevivalLoaded()` (`src/netplay/bridge/ipc_shared.cpp`)
  validates the parent program's exact identity before adopting its
  already-running session, re-validates right before hooking, and stays
  passive when anything is ambiguous. The policy is pinned by
  `tests/revival_launch_policy_tests.cpp` and
  `tests/verify_revival_launcher_adoption.py`.

To support a new Revival build, add a profile in `revival_addresses.h`,
teach `DetectRevivalVersion()` to recognize it, and extend the two
launcher-adoption tests.

## Starting a session

- **Host** first discovers your public address on a background thread
  (`HostDiscoveryThreadMain()` in `src/netplay/hooks/title_flow.cpp`), then
  starts the listener.
- **Join** validates what you typed - strict `IPv4:port` or `[IPv6]:port`,
  hostnames resolved fresh - in `src/netplay/core/network_endpoint.cpp`,
  which also refuses to publish non-routable addresses as public ones.
- When both players are ready,
  `HandoffConnectedSessionToVsHumanState()` (`src/netplay/hooks/title_flow.cpp`)
  steps the game into online character select: the mod suspends its own UI
  hooks first, uses warmup counters and bounded retries so a stuck component
  can never freeze the game, and swaps the P1/P2 input configuration for the
  joining side (swapped back on teardown, owned by the takeover code).
- **Spectate** rides the same path via `HandoffSpectateSession()`; a "wait
  for the game to begin" option covers joining before the players start.
- The delay prompt is answered programmatically from the overlay - the same
  question EfzRevival would ask on its console.

## Keeping the game alive

EfzRevival ends sessions by telling the game process to exit. The mod makes
those calls unreachable and recovers instead of dying:

- The helper's `ExitProcess` import is redirected into the mod
  (`src/netplay/bridge/iat_stubs.cpp`); the interception decides between
  pass-through (user-initiated close), in-frame recovery, and deferred
  cleanup at a tick boundary.
- Teardown and recovery are centralized in
  `src/netplay/bridge/revival_memory.cpp`: the old session is destroyed with
  the build-correct destructor before any new `init()`, patched bytes are
  restored transactionally, an offline session is rebuilt so the game is
  never without one, and the game is walked screen-by-screen back to title
  (`src/netplay/bridge/frontend_return.cpp`,
  `src/netplay/bridge/gameplay_exit_recovery.cpp`).
- Local Practice and Tournament sessions are recognized by their exact
  build-specific vtables (`src/netplay/bridge/revival_session_validation.cpp`).
  They do not contain the online delay and ping fields; checking those fields
  could reject a successfully rebuilt local session and stall Tournament exit.
- `RequestInjectedPeerQuitBroadcast()` (revival_takeover.cpp) says goodbye
  to the peer while the helper can still send, so a healthy opponent never
  hangs waiting for us.
- Watchers feed one recovery path: a spectator-Esc key watcher (EfzRevival
  swallows keyboard input while spectating), console-error disconnects, and
  a stall watchdog for a crashed or externally killed helper
  (`PeerProcessExitWatchMain()` in ipc_shared.cpp).
- On a real crash, `src/crash_handler.cpp` writes a minidump and report
  under `logs\`, once per session, and unwinds one specific netplay failure
  (opponent disconnect at the worst moment mid-match) to the title screen
  instead of a crash dialog.

Teardown is idempotent (running it twice is recognized and harmless) and
never runs mid-frame. If the game dies anyway, a Windows job object takes
the helper and its console windows down with it.

## Rollback determinism

Online EFZ is rollback netcode: both games must simulate identically or they
desync. The mod treats that as its first constraint:

- EfzRevival's own per-frame tick always runs first and exactly once; all
  mod work happens after it, on the same thread (the per-frame wrapper in
  `revival_memory.cpp`).
- Floating-point control words are normalized so both machines compute
  identically.
- Background work - lobby polling, log flushing, diagnostics - runs below
  the game thread's priority and never queues onto the simulation path.
- Per-frame diagnostic logging is off by default; it can stall the game by
  itself.

The `xp-native-tick` preset (`CMakePresets.json`) is the strict A/B check:
it leaves EfzRevival's native per-frame tick target completely untouched.
`tests/verify_simulation_parity.py` pins these rules in source - if you
change the ordering, that test is designed to fail.

## The in-game menu

The mod hooks the title screen and adds its own menu: HOST, JOIN, PLAYER
ROOMS, LOBBY, BATTLE LOG, OPTIONS.

- Flow control and handoffs live in `src/netplay/hooks/title_flow.cpp`;
  the title-screen patch itself (adding the NETPLAY entry) in
  `title_patch_install.cpp`, with per-build patch helpers in
  `title_patch_helpers.cpp`.
- The screen model - pages, rows, navigation, inline editors - is
  `src/netplay/core/menu_model.cpp` / `include/netplay/core/menu_model.h`.
  A new menu screen starts there, gets routing in `title_flow.cpp`, and
  drawing in `title_overlay_text.cpp` / `title_draw_layers.cpp`.
- Drawing targets the game's native 640x480 paletted surface
  (`src/netplay/render/draw_surface.cpp`, `menu_overlay.cpp`), with
  day/night backgrounds chosen by local clock and asset lookup in
  `src/netplay/assets/assets.cpp`.
- Text: a tiny bitmap font built into the DLL
  (`src/netplay/render/software_font.cpp`), an optional TrueType layer
  (`MenuTtfText`, `MenuTtfFont` under `[Others]` in `EfzRevival.ini`) with
  Windows fonts and a bundled-Noto fallback, and a GDI path for scripts the
  main renderer cannot shape. Font files are located through
  `src/netplay/render/sprite_font_map.cpp`.
- TrueType menu text is drawn above the paletted surface, so it does not
  follow the palette by itself. The game's screen fades are bracketed so the
  text dims step by step with the palette, and every menu panel registers its
  rectangle so text underneath it is cut out rather than showing through.
  Before an online match the fade into character select runs first, and the
  text layer is shut down only after it.
- A Dear ImGui layer (`src/netplay/hooks/debug_overlay.cpp`) draws the
  in-match hosting badge and a developer panel. It cooperates with
  display-changing mods through the reset contract - see
  [displaymanager-reset-contract.md](displaymanager-reset-contract.md) and
  `include/compat/overlay_reset_api.h`.

### Translations

The menus can be shown in Japanese: `Language` under `[Others]` in
`EfzRevival.ini`, or OPTIONS > INTERFACE in game. Translations are compiled
into the DLL (`src/netplay/core/i18n.cpp`, table in `i18n_ja.cpp`).

- The English text is the key. Text the mod draws goes through
  `netplay::i18n::Tr("...")` at the point where it is displayed; the
  English original stays in logs, `EfzRevival.ini`, the shared state export
  and anything the code compares or parses.
- Literals in static tables that are built before a language is chosen
  (menu labels, bridge error strings) are marked `I18N_KEY("...")` and
  passed through `Tr()` where they are drawn.
- A translated `printf` format must use the same conversions in the same
  order as the English one; `tests/i18n_tests.cpp` checks every entry,
  along with valid UTF-8 and unique keys.
- Translated text is drawn by the TrueType layer, or by the GDI path when
  the bitmap font is in use. The ImGui font atlas bakes exactly the
  characters the active table uses, and the game's ASCII-only sprite font is
  skipped while a translation is active.
- Adding a language: add a table file next to `i18n_ja.cpp`, list it in
  `kTables` in `i18n.cpp`, and add the name to the `Language` choices in
  `options_menu.cpp`.

## Async hosting

You can start a host session and keep playing EFZ while the listener waits.
The whole feature is a small state machine in
`src/netplay/bridge/async_hosting.cpp`: `OnHostStarted()` arms it, **D**
calls `SetMinimized()`, `Tick()` watches for a connecting opponent, and the
delay prompt is *held* rather than shown until you return (default **F1**,
rebindable as `AsyncHostReturnKey`). Accepting just releases the hold so the
ordinary delay flow takes over. `AutoRehost()` restarts the listener on the
same port when a held peer drops. Companion mods read the state through the
`EFZ_CAP_ASYNC_HOST` capability group of the shared export.

## Lobby and rooms

Player Rooms and the Lobby talk to a community lobby server over HTTPS:

- `LobbySession` (`include/netplay/core/lobby_client.h`) is the client; its
  background thread polls every half-second (one second on XP). The poll
  doubles as the heartbeat that keeps room membership alive, so it never
  stops mid-match; replies carry generation counters so stale answers are
  discarded.
- Endpoint selection and overrides: `GetLobbyEndpointConfig()` and
  `NormalizeBaseUrl()` (`src/netplay/core/lobby_client.cpp`), with the
  `[Lobby]` INI keys and `EFZ_LOBBY_*` environment variables listed in the
  README.
- HTTPS goes through three interchangeable transports chosen per system:
  Windows HTTP, WinINet, or the bundled TLS library
  (`src/netplay/core/tls_http_client.cpp`; XP and Wine/Proton force the
  bundled one because their stacks cannot handle modern encryption).
- Public-address discovery races lookup services for IPv4 and IPv6 in
  parallel; results are advisory only. Local connectivity is probed without
  sending packets by `src/netplay/core/network_capability.cpp`.

## Battle Log

An in-game browser for the `BattleLog.txt` the game writes:
`src/netplay/hooks/battle_log_menu.cpp` parses play sessions into sets and
games and renders the summary, browser, filters, and detail pages, with
character portraits drawn through the end-of-frame hook shared with the
overlay. `HideEmptySetsInBattleLog` and `BattleLogTtfText` under `[Others]`
tune it.

## Options editor

A live editor for `EfzRevival.ini` inside the game
(`src/netplay/hooks/options_menu.cpp`): it detects the file encoding
(`ReadWideTextFile()` / `WriteWideTextFile()`), groups settings into pages,
and rewrites only changed lines so comments and untouched values survive.
The mod-owned `Others` section is admitted and seeded by
`src/netplay/core/mod_settings.cpp` - that pair of files is where a new
mod setting goes (INI key there, curated entry and tooltip in
`options_menu.cpp`).

## Update check

Once per launch the mod asks GitHub for the newest release in the background
(`Start()` / `WorkerThreadProc()` / `FetchLatestReleaseJson()` in
`src/netplay/core/update_check.cpp`) and shows a `[!]` badge on OPTIONS
while a newer version is out; opening About acknowledges it
(`StateFilePath()` remembers against which release). Version ordering,
including pre-release suffixes, is `src/netplay/core/version_compare.cpp`.
The HTTP uses the bundled TLS client with pinned root certificates.

## Online custom colors

With `OnlineCustomColors` on, each player's EDIT COLOR palette is shared and
shown on both screens - character select, spectating, and the win screen:

- Transport: the palette frames ride EfzRevival's own network connection
  through a reserved message type the vanilla game ignores. The channel and
  its handshake (including staying silent against unmodded peers) are
  `OverlayChannel` (`include/netplay/interop/overlay_channel.h`,
  `src/netplay/interop/overlay_channel.cpp`); the piggyback sink and
  per-side sequencing are in `src/netplay/interop/palette_charselect.cpp`
  (`PiggybackSink()`, `PiggybackPoll()`, `DeriveSideSource()`).
- Application: `ApplyRowToCharSelectPortrait()` and `ApplyRowToWinScreen()`
  (`src/netplay/interop/palette_apply.cpp`) write the portrait palettes;
  per-character sprite-to-portrait slot translation uses the tables in
  `include/netplay/interop/palette_mapping.h` and the color math in
  `src/netplay/interop/palette_remap.cpp`. Character select sits outside
  the rollback simulation, so these live writes cannot desync a match.
- Launcher-first Practice uses the character-select render boundary when
  Revival's frame hook is absent. It adds no polling thread or battle work.
  Palette files stay cached until the character/color choice changes or
  character select is entered again. Changes to the selection, EDIT COLOR,
  or a received palette trigger updates; native portrait loads and color
  reloads refresh the stock capture. Steady frames do not reread files, scan
  portrait banks, remap colors, or write palettes.
- Each side keeps its own restore state, including when the other side is an
  Arcade CPU or temporarily enters stage selection. A failed restore retains
  that state for a retry. Arcade's final custom-color confirmation is applied
  once before the native fade starts; nested fade renders do no palette work.
- Palette hooks can chain through an existing five-byte jump to executable
  code in a loaded module when the remaining native entry bytes match. This
  preserves Training Mode's fade/audio hook. Unrecognized entry patches keep
  the palette hooks disabled and identify the affected function in the log.
- Received frames must have the exact declared size, use the plain envelope,
  and fit within 142 bytes. Palette rows require 120 color bytes and valid,
  consistent player, character, slot, source, and key fields. Storage and
  application check the row again; received bytes never select a file,
  function, or write address. Oversized IPC frames are dropped whole.
- `PaletteLoad` logs each actual file read attempt: filename, size, header
  offset, bytes read, custom/stock result, and any Windows error. Missing files
  log the open error. Character-select entry logs the working directory and
  Windows code page. Cached frames produce no load logs. Received-palette
  logs are limited to one per side per second; rejected-frame logs to one
  per five seconds with a cumulative count. This validates data, not peer
  identity; the palette channel does not authenticate UDP senders.
- A loopback transport (`ModInteropLoopback`) exists for offline testing.

## State export for companion mods

A versioned snapshot of what is happening - session mode and phase, scores,
nicknames, ping, menu page, character picks, end reasons, async-host status
- is published to a named shared-memory block (`EFZNetplay_State`) and
through the `EFZNetplay_GetState()` export. The contract, including the
magic/version/capability checks consumers must perform, is
`include/efz_netplay_state.h`; the writer is
`src/netplay/bridge/netplay_state_export.cpp` (`ExportWorkerMain()`,
`QueueUpdate()`, `SuspendForOnlineSimulation()` /
`ResumeControlPlaneUpdates()` around the online handoff). Adding a field
means bumping the ABI version there and populating it in the writer - the
header is the single source of truth.

## Crash safety and logging

`src/crash_handler.cpp` writes minidumps and readable reports under `logs\`.
`src/logger.cpp` formats on the calling thread and hands off to a dedicated
writer thread through a bounded queue (drop-oldest on overflow), so the
game thread never waits on disk.

## Building and testing

- Presets in `CMakePresets.json`: `release` (standard), `xp-release`
  (Windows XP), `xp-native-tick` (strict determinism A/B). All target
  Win32. mbedTLS comes in via the submodule; MinHook from
  `third_party/minhook`.
- There is no CI on the remote - run the contract tests locally before
  pushing: `python tests/verify_*.py` from the repo root, and the C++ test
  targets built by the presets (`interop_tests`, `overlay_reset_gate_tests`,
  `overlay_context_scope_tests`, `revival_launch_policy_tests`,
  `network_endpoint_tests`, `console_handoff_contract_tests`).
- The Python tests pin source-level contracts (ordering, presence of safety
  steps). If one fails after your change, the test is usually right.

## Notes for maintainers

- Everything fails closed: unknown or modified Revival builds are never
  hooked, launcher sessions are adopted only on exact identity matches.
- Teardown is idempotent, never mid-frame, and restores only
  still-matching patch bytes.
- Nothing runs before EfzRevival's native tick on the simulation thread.
- Comment discipline in this repo: short, load-bearing comments only - the
  explanations live in this document, not in the code.
