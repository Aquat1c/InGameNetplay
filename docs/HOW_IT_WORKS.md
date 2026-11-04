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
  needs no locks (`src/netplay/bridge/ipc_shared.cpp`).

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

## Lifecycle ownership

Session startup, menu overlays and native cleanup have separate owners.
The phase types in `include/netplay/bridge/*_state.h` and
`include/netplay/hooks/internal/frontend_lifecycle.h` describe their legal
progress. A timer measures time within a phase; a generation identifies the
request that owns it. Neither can authorize completion of a newer request.

A cancelled startup keeps its worker ownership through stale-result cleanup.
Failure to construct the startup worker reports a failed start and releases
ownership so the menu can retry. Worker completion checks its own generation
before retiring ownership; a detached old worker cannot retire a newer request.
Lobby match cleanup similarly retains the obligation to end the session
after a return or refresh request. Duplicate cancellation and late connection
callbacks cannot reopen that ending session. Frontend return keeps its
continuation claimed through the native call, including when the continuation
has already been consumed.

Host discovery, listener stabilization, delay selection, native synchronization
and gameplay warmup have their own phases. Recovery quarantine first waits its
fixed frame interval, then waits for held input to be released. Console shutdown
drains queued output before stopping; a repeated shutdown request cannot move
a stopped worker back into draining.

The console mailbox still uses its original `LONG` fields and serials.
`console_handoff_policy.h` supplies named views of prompt, delivery and timeout
progress without changing the shared layout. Exact-generation acknowledgement
and ordered serial progress are different checks and remain separate.

Resource ownership stays separate from session progress. A retained native
render context, a suspended exporter, an ImGui backend awaiting a device and
an outstanding patch restoration each carry their own cleanup obligation.
The shared overlay reset gate distinguishes a pending reset from completed
backend invalidation, so repeated reset notifications do not invalidate twice.
Native game states, independent events, input histories and configuration
values retain their existing representation.

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
- The delay overlay shows EfzRevival's measured ping, recommendation and
  allowed range. The complete console prompt is collected before publishing
  its values, including when Revival writes one character at a time. The
  native input reader waits for queued prompt parsing before choosing which
  input to serve, preserving the prompt's original cancellation generation.
  Selection and confirmation use that prompt's limits; unknown limits leave
  confirmation unavailable. If Revival rejects an answer and asks again,
  the overlay returns to selection with the new limits.
- Canceling delay setup sends Revival's quit packet before closing the helper,
  including when the local game has not yet initialized online play. During
  delay setup, received quit packets use Revival's native peer-disconnect
  handler, which closes the pending reader and ends the session. Helper
  validation accepts both the preferred and loader-rebased image headers while
  checking the exact version, relocated callback addresses and native code bytes.
  The overlay reports that the opponent quit or timed out. Terminal sessions
  cannot accept delay input or reopen selection from late console output.
- Console cancellation wakes are relayed for both players. Starting a new
  native reader discards old wakes through the same console-flush boundary as
  Revival, so a retry cannot inherit cancellation intended for its predecessor.

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

Lobby challenges use the same state machine, marked with
`HostOrigin::LobbyChallenge`. The target already holds the published endpoint,
so a challenge that loses its listener ends instead of rehosting. A room with
no challenge has no listener and keeps async hosting idle, so
`g_lobbyBackgrounded` holds it open instead: `TickBackgroundLobby()`
(`src/netplay/hooks/title_flow.cpp`) services it outside the menu, and it runs
only while the online simulation is not. Hosting and rooms exclude each other
through one confirmation prompt, `RequestStopHostingConfirmIfHosting()`, which
reads "Stop hosting?" or "Leave room?" depending on what must end.

Lobby polling holds one TLS connection open with `tls::KeepAliveClient`
(`src/netplay/core/tls_http_client.cpp`), because a new handshake every second
is real work on old CPUs. If the server has dropped the connection, the request
retries once on a fresh one.

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

## Connection check

Once a Host listener is up, `connection_check::Start()`
(`src/netplay/core/connection_check.cpp`) waits a few seconds for EfzRevival to
map its port and register with its hole punching server, then asks the test
server on a background thread. Nothing else in the mod waits for it.

- If the server cannot be reached, nothing is shown: that says nothing about the
  player's connection.
- The server sends one Ping to the caller's address and host port. A Pong means
  direct connections work, whether from port forwarding or from the UPnP mapping
  EfzRevival makes for itself.
- It then sends one Punch through the `HolePunchingServer`. A Ping back from the
  caller's address means that route works. The hole punching server's own reply
  does not count, because it also answers from entries left by earlier sessions.
- The result is added to the address line on the Host screen: `(direct)`,
  `(relay)`, or `(connection test failed)` after one retry. `Press C` still copies
  only the address. IPv6 hosts are not tested, because the server has no IPv6
  address.

The server code never answers the host, so it cannot become a peer, and it only
ever sends to the address the request came from.

### Running your own check server

The server is one Python file with no dependencies, in `server/connection_check/`
next to its test, an example systemd unit and an example Caddy config.

To point the mod at another server, set `ConnectionCheckUrl` under `[Others]` in
`EfzRevival.ini` to the base URL, for example
`https://check.example.com/api/connection-check/v1`. Empty means the built-in
default, which is `kDefaultBaseUrl` in `src/netplay/core/connection_check.cpp`;
change that to move everyone on a new build. The URL must be `https://`, and its
certificate must chain to a root bundled in `tls_http_client.cpp` (Let's Encrypt
and USERTrust are).

The server needs:

- A reverse proxy for HTTPS that sets `X-EFZ-Client-IP` from the real remote
  address and strips any copies sent by clients. The server trusts that header
  and listens only on localhost.
- One UDP port (`--udp-port`) reachable from the internet, for inbound as well as
  outbound traffic: the relay's Ping arrives from a new address, so a firewall
  must allow it.
- Public IPv4. IPv6 callers are answered as untested.

`--relay` names the hole punching server to test, so it should match the
`HolePunchingServer` value players use.

The API is two `GET` requests, returning JSON:

| Path | Response |
|---|---|
| `/status` | `{"state": "ready"}` |
| `/run?port=N` | `{"state": "done", "direct": true, "relay": false}`, or `{"state": "untested", "reason": "..."}` |

Any other answer gives no result and adds nothing to the Host screen. A caller
may run one check every 15 seconds. To test a server, run
`python test_connection_check_service.py` in that folder, then request `/status`
and `/run` from a machine that is hosting.

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
through the `EFZNetplay_CopyState(output, outputSize, minimumVersion)` export.
The copy export returns nonzero after a compatible publication is copied
into caller-owned storage under the publication mutex. Failed calls leave
the destination unchanged. `EFZNetplay_GetState()` remains available for
older consumers, but its raw pointer and the named mapping do not synchronize
with producer writes. The contract, including the
magic/version/capability checks consumers must perform, is
`include/efz_netplay_state.h`; the writer is
`src/netplay/bridge/netplay_state_export.cpp` (`ExportWorkerMain()`,
`QueueUpdate()`, `SuspendForOnlineSimulation()` /
`ResumeControlPlaneUpdates()` around the online handoff). Adding a field
means bumping the ABI version there and populating it in the writer - the
header is the single source of truth.

When Extended Practice is loaded, native session initialization and online
simulation handoff first call its `EFZ_TM_PrepareNetplayV1` export.
Starting a background host listener leaves Practice running; a pending
native initialization retires it before destroying or replacing the session. This drains Practice
writers before Revival can own the online world. After publishing the
resulting state synchronously, `EFZ_TM_FinishNetplayPrepareV1` releases the
pending handoff. The export worker drains before session startup; a failed
drain rejects startup and restores its previous suspension state.
An older training DLL without these exports blocks session start; update
both companion DLLs together.

Instrumenting a running rollback simulation can itself cause desyncs, so
ordinary updates stop at the online handoff. Two narrow paths keep the state
useful during the session: `PublishOnlineBoundary()` reads everything once on a
non-battle screen change, on the game thread, and
`PublishSuspendedHeartbeat()` republishes that snapshot four times a second
from the export worker. The heartbeat reads only the screen index byte, and
must never follow a game or session pointer.

Session fields are not all ready at the same moment, so the sequence number
tells consumers about freshness, not completeness:

- Names come from Revival's session setup, before the handoff. A set keeps its
  last good pair if a boundary read misses them.
- During an online session only screen-derived fields change between
  boundaries. Scores, ping and delay update between games.
- Scores increment on entering the results screen, and Revival's handler can
  run on the tick after the screen change is first seen, so each boundary
  publishes twice. Live win counters can also read 0 for a moment during a
  transition, so the export keeps a high-water value until a new session.
- Outside battle the live delay field holds a ping estimate, written on
  entering character select. The export reports the configured delay, which
  entering battle applies.
- On character select, a player still navigating is read from the character
  grid, because the selected-character field is stale until the cursor first
  moves. The Random slot and a running Random spinner report the unknown value.
  The spinner timer is not a lock; a player's state byte decides that. Character
  IDs are the engine's own numbering, which differs from Extended Practice's.

## Crash safety and logging

`src/crash_handler.cpp` writes minidumps and readable reports under `logs\`.
`src/logger.cpp` formats on the calling thread and hands off to a dedicated
writer thread through a bounded queue (drop-oldest on overflow), so the
game thread never waits on disk.

## Engine notes for modders

The code carries only short warnings. This is the reasoning behind them.

### Frame hooks

- Revival's per-frame tick can be built by different compilers. The MinGW
  build of 1.02j does not preserve the registers Microsoft's x86 convention
  expects it to, and was seen returning with a changed `EDI`. The call goes
  through a small seam that saves those registers itself, so the wrapper never
  jumps through stack data on its next instruction.
- Patching a function's entry overwrites a complete span of instructions, and
  that span differs by build. Copied spans can hold relative calls and jumps
  whose displacements were correct only at the original address, so they are
  fixed up in the copy. Each copy is tracked per hook site, because fixing the
  same copy twice would push its targets further off with every call.
- Revival's `init()` reinstalls its frame hook on every run by copying the
  current site bytes into a new trampoline without relocating them. A second
  run would copy the previous jump and land somewhere wild, so the bytes are
  saved before each `init()` and restored after it. Only the growing chain is
  undone. The separate jump 1.02j writes once per process must stay, or later
  sessions lose their frame driver.
- A session must not be destroyed while the tick is using it as `this`. That
  is why destructive cleanup waits for the post-tick boundary.
- Revival serializes the floating-point control word into its sync packets, and
  two machines can start in different precision states. The baseline is
  Revival's own netplay state, not a capture from the current process.

### Session boundaries and teardown

- `BeginSessionBoundary()` (`src/netplay/bridge/session_lifecycle.cpp`) runs
  once at the start of each managed session under the takeover mutex. It bumps
  a session epoch and resets latches that must not cross sessions. Left armed,
  the gameplay-exit suppression latch would skip the next session's teardown
  on whichever player returned to the menu.
- A match end calls `CancelSession()` twice. Everything is idempotent except
  rebuilding the local session, so the epoch recognizes the repeat and skips it.
- A client's P1/P2 input swap can be stranded by an interrupted session. It is
  reversed before local play and before the next `init()`, before the helper
  spawns, so nothing can race it.
- Shared input rings are flushed while the child is still suspended. Once it
  resumes, every input frame it writes must reach the DLL.
- Exit recovery waits for the old helper generation to die before releasing the
  peer slot. An immediate rehost would otherwise collide with the old singleton.

### Blocking Revival's exits

- Short conditional jumps guarding an exit become unconditional. Compound
  conditions use longer jumps into the exit block, and those are replaced with
  no-ops. An OR condition has several per call site, and all must be patched.
- The redirected `ExitProcess` import is the safety net if an unpatched site
  still reaches it: it cleans up and suspends instead of exiting. Its recovery
  jump belongs to one thread, so any other caller parks and lets the game
  thread recover.
- Tournament mode patches game code when constructed and needs those patches
  undone for in-process mode switching. Its cleanup also corrupts Revival's
  render pointer before exiting, so that pointer is saved and restored around
  the mode. Teardown happens at the post-tick boundary, never on an unknown
  stack. Its input queue has a different container layout on the 1.02j build,
  and an empty queue at the title screen is itself the exit trigger.
- Fake session vtables must match each original method's stack cleanup, or the
  stack corrupts.

### Launcher-first sessions

`src/netplay/bridge/external_launcher_guard.cpp` protects a game started by
EfzRevival's launcher when the launcher's cleanup would otherwise kill it. The
launcher's exact identity must match (PE fields, size, and two SHA-256 digests)
before its session is reused. A session counts as fully built when its witness
field is set, and which field depends on the build and role.

The guard's shared state is torn down without ever waiting, because process
detach may already have killed a reader. Readers count themselves before loading
the pointer, so closing admission and then detaching accounts for all of them.
If a reader may remain, the old bundle is left deferred: leaking it is safer
than unmapping memory in use. A failed guard stays a plain passthrough and the
parent process stays inert.

### Reading the console

- Revival moves the cursor instead of writing newlines, so a change of cursor
  row counts as one. Lines written this way complete only at that point; passing
  on a partial number could read `10` from `10800`.
- More specific text matches run before general ones.
- Handles are never remembered as "ignored", because Windows can reuse the
  number for a file the mod does track.
- Native Host cancellation and the delay prompt are tracked by exact console
  input generations. The prompt text appears before the real delay reader
  replaces the generic one, and timeout recovery waits for its generation to be
  actually consumed. The helper's console consumer must be running before the
  helper's main thread resumes, so early prompts cannot outrun it.

### Mod-interop channel

The mod runs in both processes: the game side applies palettes, and the helper
owns Revival's UDP socket. Frames ride that already-connected socket, so there
is no extra port.

- A datagram is a 2-byte envelope (flag, type), a frame header starting with the
  magic `EFZM`, then the body. Revival handles its known types and silently drops
  the rest, so an unmodded peer ignores the reserved type.
- `include/netplay/interop/overlay_ipc.h` defines two single-producer,
  single-consumer rings: one from helper to game, one back. The slot's bytes are
  published before the index advances. One slot stays open so equal indexes
  always mean empty, and a full ring drops the frame.
- The block is separate from the takeover's mailbox, because a cosmetic feature
  must never disturb session control. Its name is versioned so a layout change
  cannot attach to an older block.
- The helper's hooks run on Revival's network threads and preserve the last
  error, which the networking library reads right after each call. A send is
  recorded before the call because its completion can be dequeued first. Hooks
  are never removed: racing live threads is riskier than leaving them inert.
- Hello probes stop after about a second without an Ack, and the channel then
  stays silent for the session.

### Palette translation

`src/netplay/interop/palette_remap.cpp` turns a raw sprite palette into portrait
colors, and must match the offline palette tool exactly. Black is meaningful in
the first 20 slots, where it can be an outline. Black beyond them means unset,
so an all-black or partly unset area is skipped. Only the raw `.pal` colors are
sent; the receiver rebuilds the engine's palette itself.

The result screen frees the character objects that the saved character-select
pointer refers to, so palette work uses it only until the screen is left, and
always on the game thread.

### Menu text and ImGui

- Text sites wait for fonts to be loaded, but the backend would build its atlas
  only after text is submitted. The mod builds it first, otherwise the first
  menu visit stays on the bitmap font.
- Being already unchained from the window procedure counts as a successful
  teardown. Returning failure there made the online handoff retry every frame.
- The fade before an online match runs before the text layer shuts down. If a
  teardown then fails, the palette is restored, and retries skip the fade so a
  failing handoff cannot keep blocking the game thread.
- Each frame of a fade appears under the palette set by the previous step.

### Lobby rules

These match Concerto's behavior, and breaking them produces ghost entries or
double matches:

- The poll is the room's keepalive, so it runs through matches and spectating.
- Spectating does not leave the room.
- A match or challenge sends End immediately when it finishes.
- A player has one registration: leave the old one before joining again.
- A goodbye never waits on a connection that may have gone stale.

XP uses the bundled TLS client because its system library cannot negotiate the
server's TLS, and Wine uses it for its own WinHTTP edge cases.

### Other small rules

- The update check keeps its state in its own file. The options menu rewrites
  `EfzRevival.ini` from a cached snapshot and would drop anything added to it.
- `Tr()` lookup tables live for the whole process, so returned pointers stay
  valid when the language changes.
- Revival's Debug logs are unbuffered disk writes during play. The mod needs
  none of them, because opponent-quit detection reads console output.

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
