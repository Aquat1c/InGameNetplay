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
    // Experimental two-peer rollback tracer. Enabled by default while the
    // Awake-Nayuki RNG investigation is active; capture remains inert until a
    // compatible peer completes its handshake, and an explicit INI 0 is
    // still honored.
    bool desyncDetection = true;
    // Bounded best-effort flush of frozen desync forensic evidence on
    // terminal exit paths that never reach normal session teardown (window
    // close / emergency quit).  desync3 lost a peer-acknowledged forensic
    // onset because window-close skipped the deferred disk I/O.  Default on
    // while the RNG investigation is active; disable with
    // [Others] ExperimentalEmergencyEvidenceFlush=0.
    bool experimentalEmergencyEvidenceFlush = true;
    // Snapshot SAVE/LOAD boundary markers in the desync monitor's type-47
    // trace: MinHook detours on Revival's rollback savestate save/restore
    // entries (profile-verified RVAs + prologue signatures; 1.02h only,
    // fail-closed elsewhere).  desync4 proved the host's restore of the
    // effect-creation frame is non-idempotent; these rows show what each
    // Load restored vs what the preceding Save captured.  Disable with
    // [Others] ExperimentalSnapshotBoundaryMarkers=0.
    bool experimentalSnapshotBoundaryMarkers = true;
    // Full-verbosity capture dump: every type-47 trace row additionally
    // samples both characters' PAT-row-selecting vintage (move ID, anim
    // frame/tick, freeze counter, contact result) and every effect pass
    // publishes a char_context row with both characters' positions.
    // Disable with [Others] ExperimentalCaptureVerboseDump=0 once the
    // investigation concludes.
    bool experimentalCaptureVerboseDump = true;
    // Per-call RNG tracer: MinHook detour on Revival's rand() replacement
    // that records every logical game rand() draw with its efz.exe callsite
    // (return address), the minstd engine state before/after, per-pass
    // ordinal, and the effect slot in context, dumped to rng_trace.csv.
    // This is the per-CALL attribution the state columns cannot give - it
    // names the exact callsite that made an extra/different draw across a
    // rollback re-execution.  Hot path; disable with
    // [Others] ExperimentalRngCallTrace=0.
    bool experimentalRngCallTrace = true;
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
bool IsDesyncDetectionEnabled();
bool IsEagerZeroFrameGraphicsRestoreEnabled();
bool IsDeferredConsoleParseEnabled();
bool IsEmergencyEvidenceFlushEnabled();
bool IsSnapshotBoundaryMarkersEnabled();
bool IsCaptureVerboseDumpEnabled();
bool IsRngCallTraceEnabled();
bool IsMenuTtfTextEnabled();
const std::string& MenuTtfFontFace();
const std::string& HostingTipFontFace();
// Async-hosting return/rehost hotkey as a DIK_* binding value (e.g. "DIK_F1").
const std::string& AsyncHostReturnKeyBinding();
} // namespace netplay::mod_settings
