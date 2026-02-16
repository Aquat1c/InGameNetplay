# In-Game Netplay Menu Integration Plan

## Goal
Add a new `NETPLAY` option to EFZ title menu and, for now, make it show:

`MessageBoxA(hwnd, "In progress", "Netplay", MB_OK | MB_ICONINFORMATION);`

This project is a DLL runtime mod (not rebuilding `efz.exe` from `efz.c`).

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

Implemented in `src/netplay_menu_hooks.cpp` using a custom 8-entry dispatch table:

- `[0] Arcade` -> original case `0x007761E9`
- `[1] VS CPU` -> original case `0x00776268`
- `[2] VS Human` -> original case `0x007762E3`
- `[3] Practice` -> original case `0x00776352`
- `[4] Netplay` -> custom thunk (shows `In progress`)
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
- highlight source base: `98 -> 112`
  - VA: `0x0077664A`
  - bytes: `83 C0 62` -> `83 C0 70`

With a correctly aligned menu object sheet, destination top remains `130` and highlight screen base remains `130`.

---

## Asset Notes (Your PNGs)

- `main menu original game.png`: `120x196` (`14 rows * 14px`)
- `main menu objects and netplay.png`: `120x224` (`16 rows * 14px`, corrected)
- `main menu objects and netplay.misaligned.backup.png`: `120x217` (previous misaligned variant kept as backup)

Menu sheets are strict `14px` row-grid assets; non-grid heights cause selected-row drift/artifacts.

---

## DLL Mod Scope (Phase 1: Stub Only)

## Behavior target
- Keep menu functional.
- New `NETPLAY` row is selectable.
- Press confirm on `NETPLAY` -> show `In progress`.
- Return to title menu (no state transition).

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

## Stub Action Spec (Netplay Case)

When confirm is pressed on Netplay:
1. Play confirm SFX (`playSoundEffect(..., 6)` style for consistency).
2. Show message box:
   - title: `Netplay`
   - text: `In progress`
3. Do **not** fade out.
4. Do **not** change mode (`gameSystem + 4964`).
5. Return/stay on title state (`0`).

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
4. `NETPLAY` confirm shows `In progress`.
5. Other options still route correctly:
   - Arcade/VS/Practice -> character select path
   - Replay -> replay path
   - Options -> config path
   - Exit -> WM_CLOSE path
6. No softlock when idling, confirming, or rapidly scrolling.

---

## Next Implementation Deliverables

1. `netplay_menu_hooks.cpp`:
   - detours for title update/render
   - netplay stub messagebox branch
2. `menu_constants.h`:
   - centralized menu row count/height/source-dest rect constants
3. Optional debug log (`OutputDebugStringA`) around menu index and transitions.
