#pragma once

#include <cstdint>
#include <string>

namespace netplay::mod_settings
{
struct Settings
{
    std::string offlineVsHumanMode = "Tournament";
    bool writeLogFile = true;
    bool preserveModLogAcrossLaunches = false;
    bool preserveRevivalLogsAcrossLaunches = false;
    bool enableConsole = false;
    bool enableDebugMenu = false;
    bool verboseBridgePatchLogging = false;
    bool verboseSyncDiagnostics = false;
    bool verboseRevival102jLifecycleLogging = false;
    bool hideEmptySetsInBattleLog = true;
    // Menu text via the TTF game-RT overlay (crisp badge font) instead of
    // the 5x7 indexed-surface font, where a producer supports it (battle
    // log menu, footer tooltip). Falls back to 5x7 automatically when the
    // D3D9/ImGui overlay is unavailable.
    bool menuTtfText = true;
    // TTF face for the menu text overlay. Known values: Yu Gothic, Meiryo,
    // MS Gothic, Noto Sans JP / Noto Sans Mono (bundled mod assets),
    // Segoe UI, Arial, ITC Bolt (mod asset). Yu Gothic is the default: it
    // natively covers Latin + Cyrillic + Japanese AND ships with every
    // Windows 10/11 base install (Meiryo is an optional feature there); the
    // bundled Noto faces guarantee coverage everywhere (incl. Wine) - the
    // Microsoft faces cannot legally be redistributed with the mod.
    std::string menuTtfFontFace = "Yu Gothic";
    // Font face for the in-game hosting-overlay badge ("Hosting... Press F1...").
    // Independent of the menu face so the tip can stand out; ASCII-only text.
    std::string hostingTipFontFace = "Yu Gothic";
    // Keyboard binding (DIK_* form) for the async-hosting "return / rehost"
    // hotkey used while the hosting overlay is minimized in-game.
    std::string asyncHostReturnKey = "DIK_F1";
    // Master switch for the mod-interop overlay channel (peer<->peer cosmetic
    // side data over EfzRevival's own UDP; first payload = online char-select
    // portrait palettes). Default ON: live-proven online, it piggybacks Revival's
    // own socket so it needs no extra port/config and vanilla peers are unaffected
    // (they drop the reserved typeId). The end-user gate is the ini key
    // "OnlineCustomColors" and the debug-menu toggle; disabling it makes the
    // entire subsystem (channel + palettes) inert at every entry point.
    bool onlineCustomColors = true;
    // Dev-only loopback for the overlay channel (Stage-1 test): drives the
    // palette chain with a local echo sink instead of the network, so a SOLO
    // local-play session mirrors P1's chosen palette onto the P2 portrait. Has
    // no effect unless modInteropChannel is also on. Never ship enabled.
    bool modInteropLoopback = false;
    // Stage-2 test transport: a side-channel UDP socket (this is the interim
    // direct/LAN transport; the production path piggybacks Revival's own socket).
    // modInteropPeer = the OTHER client's IP; modInteropPort = the shared side
    // port both clients bind. Used only when modInteropChannel is on and
    // loopback is off. Empty peer disables the socket transport.
    std::string modInteropPeer = "";
    uint16_t modInteropPort = 10801;
    // Which side this client controls (0=P1/host, 1=P2/join). We broadcast this
    // side's palette and apply the peer's. Loopback ignores it (uses side 0).
    int modInteropSide = 0;
};

void Reload();
const Settings& Get();

bool UseTournamentModeForOfflineVsHuman();
bool IsFileLoggingEnabled();
bool PreserveModLogAcrossLaunches();
bool PreserveRevivalLogsAcrossLaunches();
bool IsConsoleEnabled();
bool IsDebugMenuEnabled();
bool IsVerboseBridgePatchLoggingEnabled();
bool IsVerboseSyncDiagnosticsEnabled();
bool IsVerboseRevival102jLifecycleLoggingEnabled();
bool AreAllVerboseLogsEnabled();
bool HideEmptySetsInBattleLogByDefault();
bool IsMenuTtfTextEnabled();
const std::string& MenuTtfFontFace();
const std::string& HostingTipFontFace();
// Async-hosting return/rehost hotkey as a DIK_* binding value (e.g. "DIK_F1").
const std::string& AsyncHostReturnKeyBinding();
// End-user gate (ini key "OnlineCustomColors") for online custom portrait
// colors + the mod-interop overlay channel. When false, the whole subsystem is
// inert (no socket interposition, no handshake, no palette exchange). Default
// ON. See Settings::onlineCustomColors.
bool AreOnlineCustomColorsEnabled();
// Runtime override of the gate (for the debug-menu toggle). Does not persist to
// the ini; a Reload() re-reads the stored value.
void SetOnlineCustomColorsEnabled(bool enabled);
// Dev-only overlay-channel loopback (Stage-1 solo palette test). Requires the
// master gate to also be on. See Settings::modInteropLoopback.
bool IsModInteropLoopbackEnabled();
// Stage-2 side-socket transport config. Empty peer = disabled.
const std::string& ModInteropPeer();
uint16_t ModInteropPort();
int ModInteropSide();
} // namespace netplay::mod_settings
