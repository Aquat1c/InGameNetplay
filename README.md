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
  - `NETPLAY` case now shows an in-game message box
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
