# In-Game Netplay Menu Integration Plan

## Goal
Add a new `NETPLAY` option to EFZ title menu and open an in-engine netplay submenu.

Current wiring status:
- Confirm on `NETPLAY` enters a custom netplay submenu loop in title state.
- Netplay submenu assets are loaded from DLL-relative `assets`.
- Currently stubbed actions still showing `In progress`:
  - `HOST -> START HOST`
  - `JOIN -> CONNECT`
- Editable actions are now inline in-menu (no modal edit dialog):
  - host port, join address, join port, nickname

This project is a DLL runtime mod (not rebuilding `efz.exe` from `efz.c`).

## Current Asset Direction (Netplay Screen)

Planned assets for the netplay flow:

- Background: `netplay_bg.dat` (game-compatible format)
- BGM: `bgm08.wav` (currently unused in vanilla flow)
- Objects/sprite sheet: optional, but preferred for styled runtime text
- Netplay `.dat` assets should be loaded from a folder near the DLL (not from the game root data path).
  - DLL deployment folder:
    - `EFZ\\mods\\efz_netplay_mod\\efz_netplay_mod.dll`
  - Example deployment path:
    - `EFZ\\mods\\efz_netplay_mod\\assets`
  - Expected lookup model:
    - `<dll_dir>\\assets\\netplay_bg.dat`
    - object candidates:
      - `<dll_dir>\\assets\\netplay_ob.dat`
      - `<dll_dir>\\assets\\netplay_ui_ob.dat`
      - `<dll_dir>\\assets\\config_ob.dat`
      - fallback `system\\title_ob.dat`
  - BGM pathing remains vanilla game pathing from EXE root:
    - `wave\\bgm\\bgm08.wav`

Notes:
- Title menu currently uses `system\\title.dat` + `system\\title_ob.dat`.
- Netplay screen/menu implementation will need its own init/render path to load `netplay_bg.dat` and netplay objects, and trigger `bgm08.wav` in the same style as other menus.
- Current implementation renders netplay labels and live values (nickname/IP/ports) from code using sprite-glyph blits from the loaded object surface.
- Runtime sprite font metadata is loaded from:
  - `<dll_dir>\\assets\\netplay_font_map.txt`
  - template/example: `assets\\netplay_font_map.example.txt`
- Optional plain GDI overlay fallback exists but is disabled by default.

### Canonical `netplay_ob` Row Map (Now Wired)

The netplay menu uses a canonical 8-row slot map in code (`include/netplay/core/menu_model.h`, `src/netplay/core/menu_model.cpp`) so renderer, navigation, and action dispatch all reference the same row slots.

Row slots (config-style lanes):

| Row | Slot Name | Intended text lane |
|---:|---|---|
| `0` | `ROW_HOST` | `HOST` / `START HOST` |
| `1` | `ROW_JOIN` | `JOIN` / `CONNECT` |
| `2` | `ROW_NICKNAME` | `CHANGE NICKNAME` / `NAME` |
| `3` | `ROW_ADDRESS` | `ADDRESS` |
| `4` | `ROW_PORT` | `PORT` |
| `5` | `ROW_RESERVED5` | reserved for future submenu option |
| `6` | `ROW_RESERVED6` | reserved for future submenu option |
| `7` | `ROW_RETURN` | `RETURN TO TITLE` / `BACK` |

Menu-to-row mapping:

| Menu | Navigation entries (in order) | Row slots used |
|---|---|---|
| Main | `HOST`, `JOIN`, `CHANGE NICKNAME`, `RETURN TO TITLE` | `0, 1, 2, 7` |
| Host | `START HOST`, `PORT`, `BACK` | `0, 4, 7` |
| Join | `CONNECT`, `ADDRESS`, `PORT`, `BACK` | `1, 3, 4, 7` |
| Nickname | `NAME`, `BACK` | `2, 7` |

Implementation notes:
- Validation now runs at hook install time and rejects invalid/duplicate row usage per menu.
- Runtime logs include both row index and row name (for example `row=4(ROW_PORT)`), which makes sprite-lane debugging easier.
- Reserved rows `5` and `6` are intentionally free for future options like `MATCH SETTINGS`, `NETWORK TEST`, or confirm/substate lanes.
- In object-sheet-only mode (no sprite font map), unused rows are now masked each frame by overblitting a reserved blank lane so non-navigable labels do not appear on-screen.
- Submenu navigation now uses the game's native slide helper:
  - `performSlideAnimation` (`0x0075DA30`) is called for submenu enter/leave (`slide-out -> switch menu -> slide-in`)
  - compact row rendering consumes native slide offset (`screenContext + 1876`) so motion follows vanilla timing/curve
  - compact row layout removes spacer/empty rows during submenu states
- In object-sheet-only mode, dynamic field values are drawn manually each frame:
  - Host: port
  - Join: address + port
  - Nickname: nickname text

### Text Rendering Investigation Notes

- Vanilla front-end menu labels are largely pre-baked into object sheets (`title_ob.dat`, `config_ob.dat`, `replay.dat`) and blitted by fixed rectangles.
- No obvious generic "draw ASCII string" front-end API was identified in this build.
- Numeric-only helpers do exist:
  - `renderNumberDigitByDigit` (`0x0075D260`) from config flow.
  - `renderDigits` (`0x00774A10`) from result flow.
  - `renderNumberWithDigits` (`0x0075DB70`) supports multiple digit sprite sets via metadata, but still only numeric glyphs (`0..9`).
- Conclusion for netplay:
  - For dynamic nickname/IP/port and style consistency, runtime sprite-glyph rendering is the viable path.
  - Static text rows in `netplay_ob.dat` are not required for dynamic fields; a glyph atlas + map is enough.

---

## Binary Verification (`efz.exe`)

Checked directly from `efz.exe` (not only from decompiler output).

- File: `efz.exe`
- Size: `3,743,744` bytes
- Timestamp: `2005-11-25 13:56:50` (local file time)
- SHA-256: `3A1616D6C76D2EEA08A95BB198D853E96CB6B9B3266E277CA1D167426DD66481`
- PE image base: `0x00400000`
- `.text` VA range: `0x00401000..0x007864AE`
- Relocations stripped, so static VA addresses are valid for this binary build.

### Confirmed key function addresses

| Function | VA | RVA/File Offset | Prologue bytes |
|---|---:|---:|---|
| `initializeTitleScreen` | `0x00775D60` | `0x00375D60` | `55 8B EC 83 EC 24 ...` |
| `updateTitleScreenLogic` | `0x00775FB0` | `0x00375FB0` | `55 8B EC 83 EC 0C ...` |
| title menu renderer (`sub_7764B0`) | `0x007764B0` | `0x003764B0` | `55 8B EC 81 EC C4 00 00 00 ...` |
| exit-input check | `0x00776760` | `0x00376760` | `55 8B EC 81 EC 10 01 00 00 ...` |

### Confirmed title vtable entries in `.rdata`

`off_789980` at `VA 0x00789980`:
- `[0] = 0x007764B0` (title menu renderer)
- `[1] = 0x00775FB0` (title update logic)
- `[2] = 0x00776760` (exit input)

This confirms the decompiler naming mismatch: title render path is at `0x007764B0`.

---

## What We Confirmed In `efz.c`

### Title menu data and flow
- Title assets are loaded in `initializeTitleScreen`:
  - `system\\title.dat`
  - `system\\title_ob.dat`
  - refs: `efz.c:205811`, `efz.c:205820`, `efz.c:205826`, `efz.c:205827`
- Title menu selection index is `screenContext + 1084`.
  - init to `0`: `efz.c:205833`
- Title menu logic is `updateTitleScreenLogic` (`sub_775FB0`):
  - ref: `efz.c:205871`
- Current selection wrap range is `0..6`:
  - refs: `efz.c:205931`, `efz.c:205932`, `efz.c:205933`, `efz.c:205934`
- Current cases are:
  - `0 Arcade`
  - `1 VS CPU`
  - `2 VS Human`
  - `3 Practice`
  - `4 Replay`
  - `5 Options`
  - `6 Exit`
  - refs: `efz.c:205966` to `efz.c:206102`

### Title menu render geometry
Decompiler name is misleading (`renderReplaySelectionScreen`) but this function is bound in title vtable and uses title menu coordinates:
- vtable bind: `efz.c:1095`, `efz.c:205805`
- function body: `efz.c:206146`

Menu panel/highlight math currently:
- Panel destination rect: `(0, 0, 120, 98)` -> `efz.c:206214`
- Panel source rect: `(195, 130, 120, 98)` -> `efz.c:206221`
- Highlight destination Y: `98 + 14 * selection` -> `efz.c:206241`
- Highlight source Y: `130 + 14 * selection` -> `efz.c:206250`
- Rect helper (`left,top,width,height -> right=left+width, bottom=top+height`): `efz.c:8026`

### Runtime state machine
- Game loop calls active screen update via state array: `efz.c:2307`
- Slot initialization:
  - state `0`: title (`dword_790110[0]`) -> `efz.c:2198`
  - state `1`: character select (`dword_790114`) -> `efz.c:2212`
  - state `2`: loading (`dword_790118`) -> `efz.c:2226`
  - state `3`: battle (`dword_79011C`) -> `efz.c:2240`
  - state `4`: currently null (`dword_790120 = 0`) -> `efz.c:2241`
  - state `5`: result (`dword_790124`) -> `efz.c:2255`
  - state `6`: config (`dword_790128`) -> `efz.c:2269`
  - state `7`: staff roll (`dword_79012C`) -> `efz.c:2283`
  - state `8`: replay (`dword_790130`) -> `efz.c:2297`

Important: transitioning to state `4` now would hang/freeze because it is null.

---

## Confirmed Patch Points

All addresses below are **verified in the binary**.

### 1) Title selection wrap (`0..6` today)

| Purpose | VA | Current bytes | Current behavior |
|---|---:|---|---|
| upper bound compare | `0x0077612E` | `83 F8 06 7E 0A ...` | `cmp eax, 6` |
| clamp when negative | `0x0077614B` | `... C6 81 3C 04 00 00 06` | writes selection `= 6` |
| switch guard compare | `0x007761D5` | `83 7D F4 06 0F 87 ...` | `cmp [ebp-0Ch], 6` |

For 8 options these immediate `06` values become `07` (if using in-place patching).

### 2) Title menu action dispatch table

Jump-table dispatch:
- dispatcher instruction at `0x007761E2`: `FF 24 8D 87 64 77 00` (`jmp dword ptr [ecx*4 + 0x00776487]`)
- table base at `0x00776487`

Current 7 entries:
- `[0] -> 0x007761E9` (Arcade)
- `[1] -> 0x00776268` (VS CPU)
- `[2] -> 0x007762E3` (VS Human)
- `[3] -> 0x00776352` (Practice)
- `[4] -> 0x007763D1` (Replay)
- `[5] -> 0x00776432` (Options)
- `[6] -> 0x0077645F` (Exit)

Adding a true 8th case requires code hook/cave logic, not just changing one immediate.

### Current DLL implementation mapping

Implemented in `src/netplay/hooks/title_patch_install.cpp` using a custom 8-entry dispatch table:

- `[0] Arcade` -> original case `0x007761E9`
- `[1] VS CPU` -> original case `0x00776268`
- `[2] VS Human` -> original case `0x007762E3`
- `[3] Practice` -> original case `0x00776352`
- `[4] Netplay` -> custom thunk (enters netplay submenu loop)
- `[5] Replay` -> original case `0x007763D1`
- `[6] Options` -> original case `0x00776432`
- `[7] Exit` -> original case `0x0077645F`

### 3) Title menu render geometry constants

| Purpose | VA | Current bytes | Meaning |
|---|---:|---|---|
| menu panel dest rect | `0x00776578` | `6A 62 6A 78 6A 00 6A 00 ...` | `height=0x62 (98)`, `width=0x78 (120)` |
| menu panel src rect | `0x007765A3` | `6A 62 6A 78 68 82 00 00 00 68 C3 00 00 00 ...` | `srcTop=130`, `srcLeft=195`, `height=98`, `width=120` |
| highlight source formula | `0x00776636` | `... 6B C0 0E 83 C0 62 ...` | `14*selection + 98` |
| highlight screen formula | `0x00776682` | `... 6B D2 0E 81 C2 82 00 00 00 ...` | `14*selection + 130` |

For 8 rows, constants that assume `98 = 7 * 14` must be updated to `112 = 8 * 14`.

### Current DLL position adjustments (implemented)

Current implemented geometry patches:
- menu panel destination height: `98 -> 112`
  - VA: `0x00776578`
  - bytes: `6A 62` -> `6A 70`
- menu panel source height: `98 -> 112`
  - VA: `0x007765A3`
  - bytes: `6A 62` -> `6A 70`
- menu panel destination top: `130 -> 123`
  - VA: `0x007765A7`
  - bytes: `68 82 00 00 00` -> `68 7B 00 00 00`
- highlight source base: `98 -> 112`
  - VA: `0x0077664A`
  - bytes: `83 C0 62` -> `83 C0 70`
- highlight screen base: `130 -> 123`
  - VA: `0x00776696`
  - bytes: `81 C2 82 00 00 00` -> `81 C2 7B 00 00 00`

---

## Asset Notes (Your PNGs)

- `main menu original game.png`: `120x196` (`14 rows * 14px`)
- `main menu objects and netplay.png`: `120x224` (`16 rows * 14px`, corrected)
- `main menu objects and netplay.misaligned.backup.png`: `120x217` (previous misaligned variant kept as backup)

Menu sheets are strict `14px` row-grid assets; non-grid heights cause selected-row drift/artifacts.

---

## DLL Mod Scope (Current)

## Behavior target
- Keep menu functional.
- New `NETPLAY` row is selectable.
- Press confirm on `NETPLAY` -> enter netplay submenu (no state transition).
- Netplay submenu supports up/down + confirm/cancel.
- `Back` in netplay submenu returns to title assets/menu.
- Stub actions (`START HOST`, `CONNECT`) currently show `In progress`.
- Editable fields are handled inline with `Enter`/`Esc`.

## Minimum code changes needed (runtime patch/hook)
1. **Selection range**
   - Change wrap max from `6` to `7` in title update logic.
2. **Menu action switch**
   - Add new case for `NETPLAY`.
   - Suggested index plan:
     - `0 Arcade`
     - `1 VS CPU`
     - `2 VS Human`
     - `3 Practice`
     - `4 Netplay` (new)
     - `5 Replay` (shifted)
     - `6 Options` (shifted)
     - `7 Exit` (shifted)
3. **Render geometry**
   - Increase panel/highlight layout to match 8 rows.
   - Existing step is `14px` per row.
   - Update constants currently tied to `98/130` and `120x98`.

---

## Netplay Entry Action Spec (Current)

When confirm is pressed on title `NETPLAY`:
1. Play confirm SFX (`playSoundEffect(..., 6)` style for consistency).
2. Load netplay assets (`netplay_bg.dat` + netplay objects) from `<dll_dir>\\assets`.
3. Play BGM track `8` (`wave\\bgm\\bgm08.wav` via vanilla pathing).
4. Enter custom netplay submenu loop in title state.
5. Do **not** fade out and do **not** change mode (`gameSystem + 4964`).
6. Return/stay on title state (`0`).

When confirm is pressed inside netplay submenu:
1. If selection is `Back`: restore title assets, stop netplay BGM, return to title menu.
2. Otherwise: show `In progress` message box (placeholder action).

---

## Recommended Hook Strategy

For stability in a DLL mod:
- Hook title update function (`sub_775FB0`) and title render function (`sub_7764B0` in this decomp).
- Prefer trampoline detours over broad byte patches.
- Keep original code path for all non-netplay options.

If a full render hook is too heavy for phase 1:
- Temporary shortcut: reuse one existing option slot behavior and message-box it first.
- Then implement full 8-row render patch in phase 2.

---

## Verification Checklist

1. Title screen appears correctly with new menu objects.
2. Cursor navigates through all 8 options without wrap glitches.
3. Highlight row aligns with text row for all options.
4. `NETPLAY` confirm enters netplay submenu.
5. Netplay submenu confirm on non-back option shows `In progress`.
6. Netplay submenu `Back`/cancel returns to title with original assets.
7. Other title options still route correctly:
   - Arcade/VS/Practice -> character select path
   - Replay -> replay path
   - Options -> config path
   - Exit -> WM_CLOSE path
8. No softlock when idling, confirming, or rapidly scrolling.

---

## Current Implementation Layout

Hook and state logic has been split out of the old monolithic file:

1. Hook entrypoints and shared hook state:
   - `src/netplay/hooks/menu_hooks.cpp`
   - `include/netplay/hooks/internal/shared.h`
2. Hook patch install/remove:
   - `src/netplay/hooks/title_patch_install.cpp`
   - `src/netplay/hooks/title_patch_helpers.cpp`
3. Netplay flow and input state machine:
   - `src/netplay/hooks/title_flow.cpp`
   - `src/netplay/hooks/title_core.cpp`
4. Rendering pipeline:
   - `src/netplay/hooks/title_draw_layers.cpp`
   - `src/netplay/hooks/title_overlay_text.cpp`
   - `src/netplay/render/*`
5. Asset/path/dat handling:
   - `src/netplay/hooks/title_assets.cpp`
   - `src/netplay/assets/assets.cpp`
6. Menu model/constants/validation/inline edit:
   - `src/netplay/core/*`
   - `include/netplay/core/*`

---

## Full Menu Navigation Flow (Merged)

This section merges the previous standalone `MENU_NAVIGATION_FLOW.md` content into this integration document.

### Global screen dispatcher

- Active state index: `byte_790148` (`efz.c:2304`, `efz.c:2307`)
- Screen slots: `dword_790110[]` family (`efz.c:2198` to `efz.c:2297`)
- Main loop behavior:
  - call `currentScreen->update` (vtable slot `+4`)
  - assign update return value to `byte_790148`
  - refs: `efz.c:2304` to `efz.c:2308`

Effective model:
`next_state = screens[current_state]->update(screens[current_state]);`

### Screen slot map (`byte_790148`)

| State ID | Global slot | Init function | Vtable |
|---|---|---|---|
| `0` | `dword_790110[0]` | `initializeTitleScreen` (`efz.c:2187`) | `off_789980` (`efz.c:1095`) |
| `1` | `dword_790114` | `initializeCharacterSelectScreenEx` (`efz.c:2201`) | `off_7892C0` (`efz.c:1087`) |
| `2` | `dword_790118` | `initializeLoadingScreen` (`efz.c:2215`) | `off_7896B4` (`efz.c:1091`) |
| `3` | `dword_79011C` | `initializeBattleUIScreen` (`efz.c:2229`) | `off_7895DC` (`efz.c:1090`) |
| `4` | `dword_790120` | null (`efz.c:2241`) | none |
| `5` | `dword_790124` | `initializeVictoryScreen` (`efz.c:2244`) | `off_789874` (`efz.c:1093`) |
| `6` | `dword_790128` | `initializeConfigScreen` (`efz.c:2258`) | `off_7895A8` (`efz.c:1089`) |
| `7` | `dword_79012C` | `initializeStaffRollScreen` (`efz.c:2272`) | `off_7898C0` (`efz.c:1094`) |
| `8` | `dword_790130` | `initializeReplayScreen` (`efz.c:2286`) | `off_7897DC` (`efz.c:1092`) |

Important:
- State `4` is null; transitioning there will dead-end/crash.

### Front-end vtable contract

All menu/front-end vtables are effectively:

- `vtable[0]`: render
- `vtable[1]`: update (returns next state ID)
- `vtable[2]`: auxiliary input/hotkey callback

Main loop calls `vtable[1]`; each update typically invokes `vtable[2]`.

### Common menu input bytes (after `processPlayerInput`)

Per-player offsets in game-system input block:

- `+12`: horizontal
- `+14`: vertical
- `+16`: confirm
- `+18`: cancel/back
- `+20`, `+22`: extra buttons

Used throughout title/replay/config/charselect update paths.

### Title menu flow (state `0`)

#### Init
- `initializeTitleScreen` sets vtable `off_789980` (`efz.c:205805`)
- loads:
  - `system\\title.dat`
  - `system\\title_ob.dat`
- key fields:
  - selection: `screenContext + 1084`
  - selection anim counter: `+1086`
  - input debounce flags: `+1088`, `+1089`
  - inactivity counter: `+1092`

#### Update
- `updateTitleScreenLogic` (`efz.c:205871`)
- per frame:
  - calls auxiliary callback (`efz.c:205905`)
  - processes both players
  - vertical move updates selection index
  - original wrap range is `0..6`
  - confirm key dispatches switch/jumptable by current selection

#### Original selection actions
- `0` Arcade -> return `1` (charselect)
- `1` VS CPU -> return `1`
- `2` VS Human -> return `1`
- `3` Practice -> return `1`
- `4` Replay -> return `8` (replay menu)
- `5` Options -> return `6` (config)
- `6` Exit -> `PostMessage(..., WM_CLOSE, ...)`

Refs: `efz.c:205964` to `efz.c:206126`.

#### Idle timeout path
- on inactivity threshold, title forces mode and returns `2` (`efz.c:206115` to `efz.c:206126`).

#### Title auxiliary callback
- `checkExitReplayScreen` (`efz.c:206283`)
- ESC posts `WM_CLOSE` (`efz.c:206299` to `efz.c:206302`).

### Character Select flow (state `1`)

- Init assets (`initializeCharacterSelectScreenEx`):
  - `system\\chr_sel_bg.dat`
  - `system\\chr_sel_ob.dat`
  - `system\\stage\\all.dat`
- Update (`updateCharacterSelectScreen`) returns:
  - `1` stay charselect
  - `2` loading
  - `0` title
  - `8` replay menu
- Auxiliary callback: `handleControlSettingsInput`
  - ESC sets exit flag
  - F1/F2/F3/F10 control preset ops

Refs: `efz.c:190435`, `efz.c:190623`, `efz.c:191329`, `efz.c:192492`.

### Loading flow (state `2`)

- Update (`updateLoadingScreen`) builds loading image path, sets stage, returns `3`.
- Ref: `efz.c:202155` to `efz.c:202207`.

### Battle flow (state `3`)

- Update (`updateBattleScreenLogic`) normally returns `3`.
- Transition returns include:
  - `5` (to result)
  - `8` (title in replay-end paths)
  - one cleanup path with `0/1` depending on mode check
- Auxiliary callback: `HandleBattleHotkeysAndPracticeToggles_765660`
  - ESC exit, F4/F5/F6/F7/F8 logic, replay param read/write

Refs: `efz.c:198184`, `efz.c:198292`, `efz.c:198269`, `efz.c:198317`, `efz.c:199090`.

### Result flow (state `5`)

- Init (`initializeVictoryScreen`) loads:
  - `system\\winner.dat`
  - `system\\result.dat`
- Update (`updateResultScreenLogic`) returns:
  - `5` stay result
  - `2` charselect
  - `1` alternate front-end path
  - `7` staff roll
  - `0` title
- Auxiliary callback: `checkForStaffRollExitKey` (ESC exit flag)

Refs: `efz.c:204226`, `efz.c:204277`, `efz.c:205453`.

### Config flow (state `6`)

- Init (`initializeConfigScreen`) loads:
  - `system\\config_bg.dat`
  - `system\\config_ob.dat`
  - `system\\score.dat`
- Update (`sub_75A4C0`) returns:
  - `6` stay config
  - `0` title
- Auxiliary callback: `checkForExitInput` (ESC sets exit flag)

Refs: `efz.c:192853`, `efz.c:192927`, `efz.c:193156`, `efz.c:194372`.

### Staff roll flow (state `7`)

- Init (`initializeStaffRollScreen`) loads `system\\stfr.dat`.
- Update (`updateStaffRollScreen`) returns:
  - `7` stay staff roll
  - `0` title

Refs: `efz.c:205562`, `efz.c:205603`.

### Replay file menu flow (state `8`)

- Init (`initializeReplayScreen`) loads:
  - `system\\replay_bg.dat`
  - `system\\replay.dat`
- Update (`updateReplayScreenLogic`) local state machine at `screenContext + 1129`:
  - `0` file select
  - `1` opening transition
  - `2` option select
  - `3` closing transition
  - `4` confirm dialog
- Update returns:
  - `8` stay replay menu
  - `2` replay playback path
  - `1` replay recording-start path
  - `0` title
- Auxiliary callback: `checkReplayExitInput` (ESC sets exit flag)

Refs: `efz.c:203021`, `efz.c:203256`, `efz.c:204076`.

### Reusable animation patterns (for netplay submenu)

This section captures reusable front-end animation logic already present in vanilla replay/config flows.

#### Replay submenu animation/state machine (source: `system\\replay.dat`)

- Core local state byte: `screenContext + 1129` (`efz.c:203069`, `efz.c:203287`)
  - `0`: file list
  - `1`: open submenu animation
  - `2`: submenu active
  - `3`: close submenu animation
  - `4`: confirm dialog
- Submenu open/close anim variables:
  - phase/angle: `double at +1864`
  - row offset accumulator: `double at +1872`
  - open progression to state `2`: `efz.c:203360`..`efz.c:203375`
  - close progression back to state `0`: `efz.c:203461`..`efz.c:203476`

Replay submenu render pattern (`renderReplayScreen`):
- Cosine eased slide coordinate:
  - `xOffset = 319 - (160 - cos(phase) * 160)` (`efz.c:203770`..`efz.c:203773`)
- 3-row submenu is drawn at destination rows:
  - `xOffset + 112`, `xOffset + 128`, `xOffset + 144` (`efz.c:203790`, `efz.c:203821`, `efz.c:203852`)
- Each row has selected/unselected source lanes in `replay.dat`:
  - row0: selected `y=716`, unselected `y=852` (`efz.c:203778`, `efz.c:203783`)
  - row1: selected `y=868`, unselected `y=732` (`efz.c:203809`, `efz.c:203814`)
  - row2: selected `y=884`, unselected `y=748` (`efz.c:203840`, `efz.c:203845`)
- Confirm dialog overlay (state `4`):
  - dialog background source `y=768` -> dest `y=112` (`efz.c:203722`, `efz.c:203728`)
  - YES/NO line source `y=820 + 16 * confirmSelection` -> dest `y=160` (`efz.c:203746`, `efz.c:203754`)

Replay object-sheet setup from init:
- Predefined rect slots include submenu/confirm lanes:
  - `(0,479,90,24)`, `(0,503,90,24)`, `(0,527,90,24)`, `(0,551,90,24)`, `(0,599,90,24)`
  - refs: `efz.c:203204`..`efz.c:203230`

Netplay reuse implication:
- For netplay HOST/JOIN/etc secondary screens, this replay pattern is the closest vanilla template for:
  - opening a subpanel without changing global screen state
  - sliding 3-option action rows with selected/unselected lanes
  - in-panel YES/NO confirmation overlays

#### Config menu animation/state machine (source: `system\\config_ob.dat` + `system\\score.dat`)

- Core menu selection: `screenContext + 1096` (0..7) (`efz.c:193048`..`efz.c:193052`)
- Submode byte: `screenContext + 1101` (`efz.c:192940`, `efz.c:192959`)
  - `0`: normal config list
  - `1`: character stats view
  - `2`: loading image/gallery view

Config submenu entry/exit examples:
- Enter character stats (`case 6`, confirm):
  - `performSlideAnimation(..., 0)`
  - load score palette/data
  - set `+1101 = 1`
  - `loadCharacterDataAndGraphics`, `setPalette`
  - `performSlideAnimation(..., 1)`
  - refs: `efz.c:193132`..`efz.c:193142`
- Exit character stats back to config list:
  - slide out/in pair around palette restore to `config_ob.dat`
  - refs: `efz.c:193004`..`efz.c:193009`
- Loading/gallery mode interactions (`+1101 == 2`):
  - browse with `fadeToLoadingScreen`
  - exit via `performFadeOutAnimation` + palette reload + `performDualRangeFadeEffect`
  - refs: `efz.c:193026`, `efz.c:193032`..`efz.c:193037`

Useful generic animation helpers (already in vanilla):
- `performRotatingTransitionAnimation` (`0x0075D580`) (`efz.c:194458`)
- `fadeToLoadingScreen` (`0x0075D660`) (`efz.c:194513`)
- `performFadeOutAnimation` (`0x0075D8D0`) (`efz.c:194625`)
- `performDualRangeFadeEffect` (`0x0075D970`) (`efz.c:194676`)
- `performSlideAnimation` (`0x0075DA30`) (`efz.c:194733`)

These helpers all:
- render per-step internally
- apply palette or position transforms each step
- frame-limit around ~16ms cadence

Netplay reuse implication:
- We can build a netplay submenu stack using the same pattern as config:
  - top-level netplay list (`submode=0`)
  - per-action subpanels (`submode=1/2/...`)
  - slide/fade transitions between submodes without changing global state ID.

### Detailed Substate/Flag Reference (for future patches)

Offsets below are screen-context local. The same offset can mean different things on different screens.

#### Shared lifecycle flags (`+44` / `+45`)

- `+44` is the per-screen lifecycle/init mode flag.
- `+45` is the per-screen exit/transition request flag.
- Semantics are screen-specific, not global:
  - title: `+44` has explicit modes `0/1/2/3` (`efz.c:205878`..`efz.c:205897`)
  - replay: `+45 == 2` is a special "record/start path" exit (`efz.c:203557`..`efz.c:203564`)
  - config/result/charselect mostly use `+45 == 1` as normal exit request

Refs: `efz.c:190634`..`efz.c:190640`, `efz.c:192934`..`efz.c:192953`, `efz.c:203262`..`efz.c:203275`, `efz.c:204283`..`efz.c:204301`, `efz.c:205878`..`efz.c:205897`.

#### Title screen (`updateTitleScreenLogic`)

- Function: `efz.c:205871`
- Main menu fields:
  - `+1084`: current row
  - `+1086`: row animation counter
  - `+1088/+1089`: per-player vertical input latch
  - `+1092`: inactivity counter
- Lifecycle (`+44`) values:
  - `0`: normal update loop
  - `1`: initial palette setup
  - `2`: fade-in path
  - `3`: palette set + fade-in path

Refs: `efz.c:205833`..`efz.c:205842`, `efz.c:205878`..`efz.c:205897`, `efz.c:205931`..`efz.c:205955`.

#### Character select (`updateCharacterSelectScreen`)

- Function: `efz.c:190623`
- Per-player phase byte:
  - P1: `+1182`
  - P2: `+1183`
- Observed phase values:
  - `0`: grid navigation
  - `1`: zoom-in after character confirm
  - `2`: zoom-out/cancel return
  - `3`: color selection
  - `4`: stage selection
  - `5`: initial camera/opening transition
  - `6`: character confirmed/locked
  - `7`: stage preview transition
  - `8`: return-from-stage transition (checked in logic; assignment path is indirect in this decomp)
  - `9`: stage-next animation step
  - `10`: stage-prev animation step
- Other useful fields:
  - `+1180/+1181`: per-player directional latch
  - `+1160`: camera zoom progress
  - `+1164`: stage preview transition progress
  - `+1168`: stage next/prev animation progress
  - `+1172/+1174`: portrait zoom timers
  - `+1344`: random stage timer

Refs: `efz.c:190496`..`efz.c:190497`, `efz.c:190701`..`efz.c:191019`, `efz.c:191118`..`efz.c:191296`, `efz.c:191257`..`efz.c:191296`.

#### Config menu (`sub_75A4C0`)

- Function: `efz.c:192927`
- Primary fields:
  - `+1096`: main row (0..7)
  - `+1097`: character index (stats submode)
  - `+1099/+1100`: input latch bytes
  - `+1101`: config submode
  - `+1880`: loading gallery index
- Submode byte (`+1101`) values:
  - `0`: main config list
  - `1`: character stats panel
  - `2`: loading/gallery panel

Refs: `efz.c:192940`..`efz.c:193037`, `efz.c:193048`..`efz.c:193152`, `efz.c:193532`..`efz.c:193642`.

#### Replay file menu (`updateReplayScreenLogic`)

- Function: `efz.c:203256`
- State machine byte `+1129`:
  - `0`: file list
  - `1`: submenu opening animation
  - `2`: submenu active (PLAY / RECORD / RETURN)
  - `3`: submenu closing animation
  - `4`: YES/NO confirmation dialog
- Selection bytes:
  - `+1130`: replay slot
  - `+1131`: submenu row
  - `+1132`: YES/NO selection
  - `+1133`: character filter
- Animation bytes/values:
  - `+1102/+1103`: per-player input latches
  - `double +1864`: open/close phase
  - `double +1872`: row offset accumulator

Refs: `efz.c:203069`, `efz.c:203287`..`efz.c:203526`, `efz.c:203557`..`efz.c:203572`, `efz.c:203719`..`efz.c:203754`.

#### Result screen (`updateResultScreenLogic`)

- Function: `efz.c:204277`
- Substate byte `+1118`:
  - `0`: intro/entrance sequence
  - `1`: post-intro idle (before continue/stats decision)
  - `2`: continue prompt (YES/NO)
  - `3`: detailed stats panel
- Related fields:
  - `+1119`: YES/NO selection
  - `+1120`: input latch for YES/NO toggle
  - animation timers around `+1092..+1116` (panel movement, counters, entrances)

Refs: `efz.c:204286`, `efz.c:204391`, `efz.c:204410`..`efz.c:204505`, `efz.c:204902`..`efz.c:204938`.

#### Staff roll (`updateStaffRollScreen`)

- Function: `efz.c:205603`
- Fields:
  - `double +1080`: roll progression timer
  - `+1088`: skip/end request flag

Refs: `efz.c:205605`..`efz.c:205650`.

#### Loading screen (`updateLoadingScreen`)

- Function: `efz.c:202155`
- Effectively one-shot transition logic (no meaningful local substate enum), returns `3` after load/fade/setup.

Ref: `efz.c:202155`..`efz.c:202207`.

### Netplay implementation impact

For in-game netplay menu integration, title flow changes are still centered on:

1. Selection wrap/clamp (`updateTitleScreenLogic`)
2. Selection action dispatch table/cases
3. Title menu panel/highlight geometry in renderer (`0x007764B0`)

The new netplay screen itself should follow the same 3-callback screen model as other states:

- init: load `netplay_bg.dat` and netplay object sheet
- update: process navigation + confirm/cancel
- aux callback: keyboard escape/back/global shortcut handling
- render: compose background + menu objects/highlight

### Ambiguity note

`gameSystem + 4964` mode values are mostly mapped through behavior, but some values (notably `2` vs `3`) are context-dependent in this decompiled build and should be validated by runtime logging when implementing full netplay screen transitions.
