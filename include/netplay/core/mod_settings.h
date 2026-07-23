#pragma once

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
    // Batch timing stabilizer extra constant-cost workload, in KB scanned per
    // batch dispatch. The stabilizer wrap is a probabilistic desync suppressor
    // whose strength scales with the constant per-tick cost it adds (the
    // heavy full-dump capture suppressed hardest). This is the tunable dial:
    // 0 = wrap does its normal sampling only; N = also deterministically scan
    // N KB of a scratch buffer each batch (fixed cost, no game state, cannot
    // add variable timing). Raise it if the desync still reproduces; lower it
    // if it costs FPS. Clamped to [0, 1024] - the scratch buffer is 1 MB.
    // [Others] BatchStabilizerWorkKB (default 64).
    int batchStabilizerWorkKb = 64;
    // Defer console-capture PARSING off the writing thread. Revival writes
    // console/log text from its simulation (rollback) thread; the capture
    // parse (line assembly + keyword scans + prompt/error detection) used to
    // run synchronously in that write call. The vanilla-capture verdict
    // showed stock stays idempotent under mild batch skew, so the mod's
    // per-tick synchronous work on the rollback thread is the prime
    // re-execution perturbation - this moves the whole parse onto the
    // managed-log worker (FIFO-ordered; synchronous fallback when the worker
    // is unavailable). Kill switch: [Others] DeferredConsoleParse=0.
    bool deferredConsoleParse = true;
    // Eagerly re-enable Revival's suppressed graphics primitives after an
    // ordinary zero-frame online battle tick (remote-input starvation).
    // Stock Revival leaves them disabled until the next positive rollback
    // batch. Render-patch policy parity and the low-overhead path are the
    // production default; terminal/frontend recovery restores are unaffected. See
    // Explicit opt-in key: ExperimentalEagerZeroFrameGraphicsRestore.
    // docs/NAYUKI_AWAKE_AIR_THROW_RNG_DESYNC.md.
    bool eagerZeroFrameGraphicsRestore = false;
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
bool IsEagerZeroFrameGraphicsRestoreEnabled();
int BatchStabilizerWorkKb();
bool IsDeferredConsoleParseEnabled();
bool IsMenuTtfTextEnabled();
const std::string& MenuTtfFontFace();
const std::string& HostingTipFontFace();
// Async-hosting return/rehost hotkey as a DIK_* binding value (e.g. "DIK_F1").
const std::string& AsyncHostReturnKeyBinding();
} // namespace netplay::mod_settings
