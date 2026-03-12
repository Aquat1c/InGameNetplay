#pragma once
// Version-specific addresses and offsets for EfzRevival.dll and EFZ.exe.
// Each supported Revival build gets its own constexpr profile instance.
// The active profile is selected at runtime via g_activeRevival.
//
// NOTE: This header deliberately uses C++11-compatible constructs
// (nested namespace declarations, aggregate constexpr) so that it
// compiles cleanly under the v141_xp (Windows XP) toolset as well.

#include <cstddef>
#include <cstdint>

namespace netplay { namespace bridge { namespace takeover {

// Compile-time upper bound for error-code null-guard patch buffers.
// Individual profiles store their actual patch size in errorCodeIsZeroPatchSize.
constexpr size_t kMaxErrorCodePatchBytes = 8;

// Maximum number of role-flag / session-pointer offset slots per profile.
constexpr size_t kMaxRoleFlagOffsets    = 4;
constexpr size_t kMaxSessionPtrOffsets  = 4;

// Tournament EXE patch save/restore limits.
constexpr size_t kMaxTournamentExePatches    = 4;
constexpr size_t kMaxTournamentExePatchBytes  = 20;

struct RevivalAddressProfile
{
    // Human-readable version tag for logging (e.g. "1.02e").
    const char* versionTag;

    // -----------------------------------------------------------------------
    // PE header identification
    // -----------------------------------------------------------------------

    // PE TimeDateStamp from the Revival DLL's COFF header.  Used as the
    // primary key for runtime version detection.
    uint32_t peTimestamp;

    // -----------------------------------------------------------------------
    // EFZ executable identifiers
    // -----------------------------------------------------------------------

    // 4-byte fingerprint read from EFZ.exe at address 0x7871F4.
    uint32_t efzFingerprint;

    // Default preferred image base for EfzRevival.dll.
    uint32_t defaultImageBase;

    // -----------------------------------------------------------------------
    // EFZ.exe addresses (absolute virtual addresses)
    // -----------------------------------------------------------------------

    uintptr_t addrGameModeStructTable;
    uintptr_t addrGameModeCurrentIndex;

    // -----------------------------------------------------------------------
    // Revival DLL RVA offsets
    // -----------------------------------------------------------------------

    // Role-flag global variable locations (up to kMaxRoleFlagOffsets).
    uintptr_t roleFlagOffsets[kMaxRoleFlagOffsets];
    size_t    roleFlagOffsetCount;

    // Session-pointer global variable locations (up to kMaxSessionPtrOffsets).
    uintptr_t sessionPtrOffsets[kMaxSessionPtrOffsets];
    size_t    sessionPtrOffsetCount;

    // Global state pointer RVA.
    uintptr_t globalStatePtrOffset;

    // -----------------------------------------------------------------------
    // Additional DLL globals written by init() — used for diagnostic logging
    // -----------------------------------------------------------------------

    // RVA of dword_100A05D4: unknown flag zeroed at top of init().
    uintptr_t initFlagOffset;

    // RVA of byte_100A0289: unknown flag zeroed after version check in init().
    uintptr_t initByteOffset;

    // RVA of word_100A0774: init-once guard for EFZ_Global_InitializeIfNeeded.
    // Non-zero low byte means the global init (which sets the render context,
    // timer, global state ptr, etc.) will be SKIPPED on subsequent init() calls.
    // This is critical for Hypothesis 2 (stale render context).
    uintptr_t initOnceGuardOffset;

    // RVA of dword_100A0764: EfzTimer* pointer, set during global init.
    uintptr_t timerPtrOffset;

    // RVA of dword_100A0760: render context base (= renderContextGlobal - 0x18),
    // used as 'this' for SetTextEnabled.  Set during frame hook init.
    uintptr_t renderContextBaseOffset;

    // Error-code-is-zero function patch target.
    uintptr_t errorCodeIsZeroRva;
    size_t    errorCodeIsZeroPatchSize;

    // "Start init player" function RVA (sub_10072880 in 1.02e).
    uintptr_t startInitPlayerRva;

    // RVA of EFZ_Obj_SubStruct448_CleanupPair (sub_1006CAD0 in 1.02e).
    // A __thiscall(void* &dword_100A0760) function that swaps the two
    // adjacent 4-DWORD input-config blocks (P1 ↔ P2 controllers).
    // The swap is a toggle — calling it twice restores the original state.
    // Used to reverse the P2 input swap when a client disconnects.
    uintptr_t inputSwapPairRva;

    // RVA of the frame-hook dispatcher (sub_1006E590 in 1.02e).
    // A void(void) __stdcall function whose first 6 bytes are overwritten
    // with an absolute JMP trampoline into our per-frame hook.
    uintptr_t frameHookRva;

    // RVA of the per-frame tick dispatcher (sub_1006E570 in 1.02e).
    // A __thiscall(void* this) function — first 6 bytes are overwritten
    // with an absolute JMP trampoline into our per-frame tick handler.
    uintptr_t perFrameTickRva;

    // -----------------------------------------------------------------------
    // Session object field offsets (byte offsets from session pointer)
    // -----------------------------------------------------------------------

    uintptr_t sessionOffsetInitComplete;
    uintptr_t sessionOffsetInputDelay;
    uintptr_t sessionOffsetPingMs;

    // Base of the 4-DWORD ping struct used by AdjustPrediction / WaitLoop.
    // Layout: [0]=AdjustPrediction threshold (init 1000000), [1]=unused?,
    //         [2]=PingMs (same as sessionOffsetPingMs), [3]=WaitLoop ping.
    // AdjustPrediction reads field[0] (this offset).
    // WaitLoop reads field[3] (this offset + 12).
    // Populated by EFZ_Rollback_DrainPingSamples from the "Net" shared-
    // memory ring buffer, which is never written to by our mod.
    uintptr_t sessionOffsetPingStructBase;

    uintptr_t sessionOffsetHelperHandle;
    uintptr_t sessionOffsetHelperPid;
    uintptr_t sessionOffsetActivePlayer;
    uintptr_t sessionOffsetQueuePlayer;
    uintptr_t sessionOffsetHistoryPrimaryPtr;
    uintptr_t sessionOffsetHistorySecondaryPtr;
    uintptr_t sessionOffsetHistoryPrimaryVec;
    uintptr_t sessionOffsetHistorySecondaryVec;

    // Additional session fields written by constructors / StartInitPlayer.
    uintptr_t sessionOffsetCurrentFrame;       // +708 in 1.02e — frame counter
    uintptr_t sessionOffsetGameModeSnapshot;   // +716 — game mode at session start
    uintptr_t sessionOffsetMatchId;            // +712 — match identifier
    uintptr_t sessionOffsetSentinel;           // +1232 — INT_MAX-1 sentinel value

    // Raw wchar_t[64] player-name buffers inside the 276-byte config
    // snapshot at sessionOffsetConfigStruct + 14 / + 142.
    // NOT the std::wstring SSO objects at +740/+764 (which require heap
    // indirection for names > 7 wchars and cannot be read as flat arrays).
    uintptr_t sessionOffsetP1Name;             // +958 in 1.02e — P1 nickname
    uintptr_t sessionOffsetP2Name;             // +1086 in 1.02e — P2 nickname

    // Win counters stored in the session object.
    uintptr_t sessionOffsetP1Wins;             // +1224 in 1.02e — P1 win count
    uintptr_t sessionOffsetP2Wins;             // +1228 in 1.02e — P2 win count

    // -----------------------------------------------------------------------
    // Global state offsets (byte offsets from global-state pointer)
    // -----------------------------------------------------------------------

    uintptr_t globalStateOffsetFlag4964;
    uintptr_t globalStateOffsetFlag4965;
    uintptr_t globalStateOffsetSessionByte;

    // -----------------------------------------------------------------------
    // Tournament session layout (byte offsets from session pointer)
    // -----------------------------------------------------------------------

    // Byte offset from the tournament session pointer to the auto-nav input
    // deque (MSVC std::deque of 2-byte elements, 8 per block).  The
    // tournament constructor populates this queue with 22 byte-pair entries
    // that simulate controller presses to auto-navigate menus.
    uintptr_t tournamentInputQueueOffset;

    // RVA of EFZ_Render_ClearText (sub_1006C070 in 1.02e).  A void(void)
    // function that clears the Revival text overlay buffer.  Called after
    // switching away from tournament mode to remove stale nickname / win-
    // count text.
    uintptr_t clearTextRva;

    // RVA of EFZ_Render_SetTextEnabled (sub_1006C030 in 1.02e).  A
    // __thiscall(void* contextBase, bool enable) function that enables or
    // disables the EFZ.exe text overlay.  contextBase is &dword_100A0760
    // (i.e. base + renderContextGlobalOffset - 0x18).  The character-select
    // mode transition calls this with enable=false to hide tournament
    // nicknames / win counts.
    uintptr_t setTextEnabledRva;

    // Offset of dword_100A0778 (the EfzRender* global) in the Revival DLL's
    // .data section.  This global is used by EFZ_Render_ClearText and
    // EFZ_Render_AddTextAndClear.  Tournament cleanup corrupts it before
    // calling ExitProcess, so we save/restore it around tournament mode.
    uintptr_t renderContextGlobalOffset;

    // Absolute EXE addresses and sizes of the code patches applied by the
    // tournament session constructor.  Saved before init(3,102) and
    // restored after intercepting the tournament ExitProcess.
    uintptr_t tournamentExePatchAddr[kMaxTournamentExePatches];
    uint8_t   tournamentExePatchSize[kMaxTournamentExePatches];
    size_t    tournamentExePatchCount;

    // Revival DLL ExitProcess call-site patches.
    //
    // The tournament session's per-frame tick calls ExitProcess when it
    // detects game mode 0 (title screen).  Since ExitProcess is __noreturn,
    // the compiler emits no valid code after the call — we cannot return
    // from our IAT stub.  Instead, we patch the conditional-jump bytes
    // that guard each ExitProcess call to unconditional jumps (74/75→EB),
    // making the calls unreachable.  The IAT hook remains as a safety net.
    //
    // Each entry is an RVA relative to the Revival DLL's default image
    // base (0x10000000).  The original byte at that RVA is a Jcc opcode
    // (0x74 = jz, 0x75 = jnz) which is replaced with 0xEB (jmp short).
    static constexpr size_t kMaxExitProcessPatches = 4;
    uintptr_t exitProcessPatchRva[kMaxExitProcessPatches];
    uint8_t   exitProcessPatchOriginal[kMaxExitProcessPatches];
    size_t    exitProcessPatchCount;

    // Near-Jcc (6-byte) ExitProcess call-site patches.
    //
    // Compound-condition ExitProcess guards use 6-byte near-Jcc instructions
    // (0F 84 rel32 = jz near, 0F 85 rel32 = jnz near) that jump INTO the
    // ExitProcess block.  These cannot use the single-byte 74/75→EB patch.
    // Instead each 6-byte instruction is replaced with 6 NOP bytes (0x90),
    // making the ExitProcess block unreachable from that branch.
    //
    // For compound OR conditions (e.g. `if (!A || !B)`) there are multiple
    // Jcc instructions per call site; ALL must be patched.
    static constexpr size_t kMaxExitProcessNearJccPatches = 8;
    uintptr_t exitProcessNearJccRva[kMaxExitProcessNearJccPatches];
    size_t    exitProcessNearJccCount;
};

// ---------------------------------------------------------------------------
// Known profiles
// ---------------------------------------------------------------------------

// EfzRevival.dll v1.02e — original release, baseline for all addresses.
constexpr RevivalAddressProfile kRevival_1_02e = {
    "1.02e",                                            // versionTag
    0x5EA876B0u,                                        // peTimestamp
    0x4386998Fu,                                        // efzFingerprint
    0x10000000u,                                        // defaultImageBase
    0x00790110u,                                        // addrGameModeStructTable
    0x00790148u,                                        // addrGameModeCurrentIndex
    {0x000A05D0u, 0x000A05F0u, 0x000A15FCu, 0u},       // roleFlagOffsets
    3,                                                  // roleFlagOffsetCount
    {0x000A02CCu, 0x000A02ECu, 0u, 0u},                // sessionPtrOffsets
    2,                                                  // sessionPtrOffsetCount
    0x000A07B8u,                                        // globalStatePtrOffset
    0x000A05D4u,                                        // initFlagOffset
    0x000A0289u,                                        // initByteOffset
    0x000A0774u,                                        // initOnceGuardOffset
    0x000A0764u,                                        // timerPtrOffset
    0x000A0760u,                                        // renderContextBaseOffset
    0x000021E0u,                                        // errorCodeIsZeroRva
    8u,                                                 // errorCodeIsZeroPatchSize
    0x00072880u,                                        // startInitPlayerRva
    0x0006CAD0u,                                        // inputSwapPairRva
    0x0006E590u,                                        // frameHookRva
    0x0006E570u,                                        // perFrameTickRva
    1220u,                                              // sessionOffsetInitComplete
    688u,                                               // sessionOffsetInputDelay
    936u,                                               // sessionOffsetPingMs
    928u,                                               // sessionOffsetPingStructBase
    700u,                                               // sessionOffsetHelperHandle
    1216u,                                              // sessionOffsetHelperPid
    680u,                                               // sessionOffsetActivePlayer
    684u,                                               // sessionOffsetQueuePlayer
    824u,                                               // sessionOffsetHistoryPrimaryPtr
    828u,                                               // sessionOffsetHistorySecondaryPtr
    788u,                                               // sessionOffsetHistoryPrimaryVec
    800u,                                               // sessionOffsetHistorySecondaryVec
    708u,                                               // sessionOffsetCurrentFrame
    716u,                                               // sessionOffsetGameModeSnapshot
    712u,                                               // sessionOffsetMatchId
    1232u,                                              // sessionOffsetSentinel
    958u,                                               // sessionOffsetP1Name (raw wchar_t[64] in config struct)
    1086u,                                              // sessionOffsetP2Name (raw wchar_t[64] in config struct)
    1224u,                                              // sessionOffsetP1Wins
    1228u,                                              // sessionOffsetP2Wins
    4964u,                                              // globalStateOffsetFlag4964
    4965u,                                              // globalStateOffsetFlag4965
    82563u,                                             // globalStateOffsetSessionByte
    740u,                                               // tournamentInputQueueOffset
    0x0006C070u,                                        // clearTextRva
    0x0006C030u,                                        // setTextEnabledRva
    0x000A0778u,                                        // renderContextGlobalOffset
    {0x763F04u, 0x763E50u, 0x754C1Au, 0x7599EDu},      // tournamentExePatchAddr
    {7, 7, 1, 20},                                      // tournamentExePatchSize
    4,                                                  // tournamentExePatchCount
    {0x0007909Du, 0x0007215Du, 0x0007231Fu, 0u},        // exitProcessPatchRva
    {0x74u, 0x75u, 0x74u, 0u},                          // exitProcessPatchOriginal
    3,                                                  // exitProcessPatchCount
    {0x0007251Bu, 0x0007252Eu, 0x000742E1u, 0x000742F4u, 0x00074301u, 0u, 0u, 0u},
    5,                                                  // exitProcessNearJccCount
};

// EfzRevival.dll v1.02f — minor revision, same .data layout as 1.02e.
// Code shift of +0x30 for most (but not all) functions.
constexpr RevivalAddressProfile kRevival_1_02f = {
    "1.02f",                                            // versionTag
    0x5F8C58A3u,                                        // peTimestamp
    0x4386998Fu,                                        // efzFingerprint
    0x10000000u,                                        // defaultImageBase
    0x00790110u,                                        // addrGameModeStructTable
    0x00790148u,                                        // addrGameModeCurrentIndex
    {0x000A05D0u, 0x000A05F0u, 0u, 0u},                // roleFlagOffsets
    2,                                                  // roleFlagOffsetCount
    {0x000A02CCu, 0u, 0u, 0u},                          // sessionPtrOffsets
    1,                                                  // sessionPtrOffsetCount
    0x000A07B8u,                                        // globalStatePtrOffset
    0x000A05D4u,                                        // initFlagOffset
    0x000A0289u,                                        // initByteOffset
    0x000A0774u,                                        // initOnceGuardOffset
    0x000A0764u,                                        // timerPtrOffset
    0x000A0760u,                                        // renderContextBaseOffset
    0x000021E0u,                                        // errorCodeIsZeroRva
    8u,                                                 // errorCodeIsZeroPatchSize
    0x000728B0u,                                        // startInitPlayerRva
    0x0006CAD0u,                                        // inputSwapPairRva
    0x0006E590u,                                        // frameHookRva
    0x0006E570u,                                        // perFrameTickRva
    1220u,                                              // sessionOffsetInitComplete
    688u,                                               // sessionOffsetInputDelay
    936u,                                               // sessionOffsetPingMs
    928u,                                               // sessionOffsetPingStructBase
    700u,                                               // sessionOffsetHelperHandle
    1216u,                                              // sessionOffsetHelperPid
    680u,                                               // sessionOffsetActivePlayer
    684u,                                               // sessionOffsetQueuePlayer
    824u,                                               // sessionOffsetHistoryPrimaryPtr
    828u,                                               // sessionOffsetHistorySecondaryPtr
    788u,                                               // sessionOffsetHistoryPrimaryVec
    800u,                                               // sessionOffsetHistorySecondaryVec
    708u,                                               // sessionOffsetCurrentFrame
    716u,                                               // sessionOffsetGameModeSnapshot
    712u,                                               // sessionOffsetMatchId
    1232u,                                              // sessionOffsetSentinel
    958u,                                               // sessionOffsetP1Name (raw wchar_t[64] in config struct)
    1086u,                                              // sessionOffsetP2Name (raw wchar_t[64] in config struct)
    1224u,                                              // sessionOffsetP1Wins
    1228u,                                              // sessionOffsetP2Wins
    4964u,                                              // globalStateOffsetFlag4964
    4965u,                                              // globalStateOffsetFlag4965
    82563u,                                             // globalStateOffsetSessionByte
    740u,                                               // tournamentInputQueueOffset
    0x0006C070u,                                        // clearTextRva
    0x0006C030u,                                        // setTextEnabledRva
    0x000A0778u,                                        // renderContextGlobalOffset
    {0x763F04u, 0x763E50u, 0x754C1Au, 0x7599EDu},      // tournamentExePatchAddr
    {7, 7, 1, 20},                                      // tournamentExePatchSize
    4,                                                  // tournamentExePatchCount
    {0x000790CDu, 0x0007218Du, 0x0007234Fu, 0u},        // exitProcessPatchRva
    {0x74u, 0x75u, 0x74u, 0u},                          // exitProcessPatchOriginal
    3,                                                  // exitProcessPatchCount
    {0x0007254Bu, 0x0007255Eu, 0x00074311u, 0x00074324u, 0x00074331u, 0u, 0u, 0u},
    5,                                                  // exitProcessNearJccCount
};

// EfzRevival.dll v1.02g — session objects enlarged (host 0x5D0→0x690),
// same .data layout as e/f.  Tournament results ExitProcess guard
// encoding changed from 2-byte short JNZ to 6-byte near JNZ.
constexpr RevivalAddressProfile kRevival_1_02g = {
    "1.02g",                                            // versionTag
    0x6240CE73u,                                        // peTimestamp
    0x4386998Fu,                                        // efzFingerprint
    0x10000000u,                                        // defaultImageBase
    0x00790110u,                                        // addrGameModeStructTable
    0x00790148u,                                        // addrGameModeCurrentIndex
    {0x000A05D0u, 0x000A05F0u, 0u, 0u},                // roleFlagOffsets
    2,                                                  // roleFlagOffsetCount
    {0x000A02CCu, 0u, 0u, 0u},                          // sessionPtrOffsets
    1,                                                  // sessionPtrOffsetCount
    0x000A07B8u,                                        // globalStatePtrOffset
    0x000A05D4u,                                        // initFlagOffset
    0x000A0289u,                                        // initByteOffset
    0x000A0774u,                                        // initOnceGuardOffset
    0x000A0764u,                                        // timerPtrOffset
    0x000A0760u,                                        // renderContextBaseOffset
    0x000021E0u,                                        // errorCodeIsZeroRva
    8u,                                                 // errorCodeIsZeroPatchSize
    0x00072AE0u,                                        // startInitPlayerRva
    0x0006CCE0u,                                        // inputSwapPairRva
    0x0006E7A0u,                                        // frameHookRva
    0x0006E780u,                                        // perFrameTickRva
    1220u,                                              // sessionOffsetInitComplete
    688u,                                               // sessionOffsetInputDelay
    936u,                                               // sessionOffsetPingMs
    928u,                                               // sessionOffsetPingStructBase
    700u,                                               // sessionOffsetHelperHandle
    1216u,                                              // sessionOffsetHelperPid
    680u,                                               // sessionOffsetActivePlayer
    684u,                                               // sessionOffsetQueuePlayer
    824u,                                               // sessionOffsetHistoryPrimaryPtr
    828u,                                               // sessionOffsetHistorySecondaryPtr
    788u,                                               // sessionOffsetHistoryPrimaryVec
    800u,                                               // sessionOffsetHistorySecondaryVec
    708u,                                               // sessionOffsetCurrentFrame
    716u,                                               // sessionOffsetGameModeSnapshot
    712u,                                               // sessionOffsetMatchId
    1232u,                                              // sessionOffsetSentinel
    958u,                                               // sessionOffsetP1Name (raw wchar_t[64] in config struct)
    1086u,                                              // sessionOffsetP2Name (raw wchar_t[64] in config struct)
    1224u,                                              // sessionOffsetP1Wins
    1228u,                                              // sessionOffsetP2Wins
    4964u,                                              // globalStateOffsetFlag4964
    4965u,                                              // globalStateOffsetFlag4965
    82563u,                                             // globalStateOffsetSessionByte
    740u,                                               // tournamentInputQueueOffset
    0x0006C280u,                                        // clearTextRva
    0x0006C240u,                                        // setTextEnabledRva
    0x000A0778u,                                        // renderContextGlobalOffset
    {0x763F04u, 0x763E50u, 0x754C1Au, 0x7599EDu},      // tournamentExePatchAddr
    {7, 7, 1, 20},                                      // tournamentExePatchSize
    4,                                                  // tournamentExePatchCount
    // In 1.02g+ the tournament results guard uses near-JNZ (0F 85),
    // so Site 1 moves from short-Jcc to near-Jcc leaving 2 short sites.
    {0x0007937Du, 0x0007257Fu, 0u, 0u},                 // exitProcessPatchRva
    {0x74u, 0x74u, 0u, 0u},                             // exitProcessPatchOriginal
    2,                                                  // exitProcessPatchCount
    {0x000723F5u, 0x000723FEu, 0x0007277Bu, 0x0007278Eu,
     0x00074561u, 0x00074574u, 0x00074581u, 0u},        // exitProcessNearJccRva
    7,                                                  // exitProcessNearJccCount
};

// EfzRevival.dll v1.02h — .data shifted +0x20 from e/f/g.
// Code shift +0x880 from 1.02e baseline (non-uniform).
constexpr RevivalAddressProfile kRevival_1_02h = {
    "1.02h",                                            // versionTag
    0x62929371u,                                        // peTimestamp
    0x4386998Fu,                                        // efzFingerprint
    0x10000000u,                                        // defaultImageBase
    0x00790110u,                                        // addrGameModeStructTable
    0x00790148u,                                        // addrGameModeCurrentIndex
    {0x000A05F0u, 0x000A0610u, 0u, 0u},                // roleFlagOffsets
    2,                                                  // roleFlagOffsetCount
    {0x000A02ECu, 0u, 0u, 0u},                          // sessionPtrOffsets
    1,                                                  // sessionPtrOffsetCount
    0x000A07E4u,                                        // globalStatePtrOffset
    0x000A05F4u,                                        // initFlagOffset
    0x000A02A9u,                                        // initByteOffset
    0x000A0794u,                                        // initOnceGuardOffset
    0x000A0784u,                                        // timerPtrOffset
    0x000A0780u,                                        // renderContextBaseOffset
    0x000021E0u,                                        // errorCodeIsZeroRva
    8u,                                                 // errorCodeIsZeroPatchSize
    0x00073220u,                                        // startInitPlayerRva
    0x0006D320u,                                        // inputSwapPairRva
    0x0006EE10u,                                        // frameHookRva
    0x0006EDF0u,                                        // perFrameTickRva
    1220u,                                              // sessionOffsetInitComplete
    688u,                                               // sessionOffsetInputDelay
    936u,                                               // sessionOffsetPingMs
    928u,                                               // sessionOffsetPingStructBase
    700u,                                               // sessionOffsetHelperHandle
    1216u,                                              // sessionOffsetHelperPid
    680u,                                               // sessionOffsetActivePlayer
    684u,                                               // sessionOffsetQueuePlayer
    824u,                                               // sessionOffsetHistoryPrimaryPtr
    828u,                                               // sessionOffsetHistorySecondaryPtr
    788u,                                               // sessionOffsetHistoryPrimaryVec
    800u,                                               // sessionOffsetHistorySecondaryVec
    708u,                                               // sessionOffsetCurrentFrame
    716u,                                               // sessionOffsetGameModeSnapshot
    712u,                                               // sessionOffsetMatchId
    1232u,                                              // sessionOffsetSentinel
    958u,                                               // sessionOffsetP1Name (raw wchar_t[64] in config struct)
    1086u,                                              // sessionOffsetP2Name (raw wchar_t[64] in config struct)
    1224u,                                              // sessionOffsetP1Wins
    1228u,                                              // sessionOffsetP2Wins
    4964u,                                              // globalStateOffsetFlag4964
    4965u,                                              // globalStateOffsetFlag4965
    82563u,                                             // globalStateOffsetSessionByte
    740u,                                               // tournamentInputQueueOffset
    0x0006C8C0u,                                        // clearTextRva
    0x0006C880u,                                        // setTextEnabledRva
    0x000A0798u,                                        // renderContextGlobalOffset
    {0x763F04u, 0x763E50u, 0x754C1Au, 0x7599EDu},      // tournamentExePatchAddr
    {7, 7, 1, 20},                                      // tournamentExePatchSize
    4,                                                  // tournamentExePatchCount
    {0x00079BFDu, 0x00072CAFu, 0u, 0u},                 // exitProcessPatchRva
    {0x74u, 0x74u, 0u, 0u},                             // exitProcessPatchOriginal
    2,                                                  // exitProcessPatchCount
    {0x00072B25u, 0x00072B2Eu, 0x00072EABu, 0x00072EBEu,
     0x00074CA1u, 0x00074CB4u, 0x00074CC1u, 0u},        // exitProcessNearJccRva
    7,                                                  // exitProcessNearJccCount
};

// EfzRevival.dll v1.02i — largest version (SizeOfImage 0xB3000 vs 0xB2000).
// .data shifted non-uniformly (~+0x1028-0x1030) from e.
// Session objects grew by 8 bytes; all field offsets from byte 488 shift +8.
constexpr RevivalAddressProfile kRevival_1_02i = {
    "1.02i",                                            // versionTag
    0x63BF27EAu,                                        // peTimestamp
    0x4386998Fu,                                        // efzFingerprint
    0x10000000u,                                        // defaultImageBase
    0x00790110u,                                        // addrGameModeStructTable
    0x00790148u,                                        // addrGameModeCurrentIndex
    {0x000A15FCu, 0x000A1620u, 0u, 0u},                // roleFlagOffsets
    2,                                                  // roleFlagOffsetCount
    {0x000A15F8u, 0u, 0u, 0u},                          // sessionPtrOffsets
    1,                                                  // sessionPtrOffsetCount
    0x000A17F4u,                                        // globalStatePtrOffset
    0x000A1600u,                                        // initFlagOffset
    0x000A12B1u,                                        // initByteOffset
    0x000A17A4u,                                        // initOnceGuardOffset
    0x000A1794u,                                        // timerPtrOffset
    0x000A1790u,                                        // renderContextBaseOffset
    0x000021E0u,                                        // errorCodeIsZeroRva
    8u,                                                 // errorCodeIsZeroPatchSize
    0x00073690u,                                        // startInitPlayerRva
    0x0006D5F0u,                                        // inputSwapPairRva
    0x0006F0E0u,                                        // frameHookRva
    0x0006F0C0u,                                        // perFrameTickRva
    1228u,                                              // sessionOffsetInitComplete (+8)
    696u,                                               // sessionOffsetInputDelay (+8)
    944u,                                               // sessionOffsetPingMs (+8)
    936u,                                               // sessionOffsetPingStructBase (+8)
    708u,                                               // sessionOffsetHelperHandle (+8)
    1224u,                                              // sessionOffsetHelperPid (+8)
    688u,                                               // sessionOffsetActivePlayer (+8)
    692u,                                               // sessionOffsetQueuePlayer (+8)
    832u,                                               // sessionOffsetHistoryPrimaryPtr (+8)
    836u,                                               // sessionOffsetHistorySecondaryPtr (+8)
    796u,                                               // sessionOffsetHistoryPrimaryVec (+8)
    808u,                                               // sessionOffsetHistorySecondaryVec (+8)
    716u,                                               // sessionOffsetCurrentFrame (+8)
    724u,                                               // sessionOffsetGameModeSnapshot (+8)
    720u,                                               // sessionOffsetMatchId (+8)
    1240u,                                              // sessionOffsetSentinel (+8)
    966u,                                               // sessionOffsetP1Name (+8, raw wchar_t[64] in config struct)
    1094u,                                              // sessionOffsetP2Name (+8, raw wchar_t[64] in config struct)
    1232u,                                              // sessionOffsetP1Wins (+8)
    1236u,                                              // sessionOffsetP2Wins (+8)
    4964u,                                              // globalStateOffsetFlag4964
    4965u,                                              // globalStateOffsetFlag4965
    82563u,                                             // globalStateOffsetSessionByte
    748u,                                               // tournamentInputQueueOffset (+8)
    0x0006CB90u,                                        // clearTextRva
    0x0006CB50u,                                        // setTextEnabledRva
    0x000A17A8u,                                        // renderContextGlobalOffset
    {0x763F04u, 0x763E50u, 0x754C1Au, 0x7599EDu},      // tournamentExePatchAddr
    {7, 7, 1, 20},                                      // tournamentExePatchSize
    4,                                                  // tournamentExePatchCount
    {0x0007A1DDu, 0x0007311Fu, 0u, 0u},                 // exitProcessPatchRva
    {0x74u, 0x74u, 0u, 0u},                             // exitProcessPatchOriginal
    2,                                                  // exitProcessPatchCount
    {0x00072F8Eu, 0x00072F97u, 0x0007331Bu, 0x0007332Eu,
     0x00075231u, 0x00075244u, 0x00075251u, 0u},        // exitProcessNearJccRva
    7,                                                  // exitProcessNearJccCount
};

// Table of all known profiles, for DetectRevivalVersion() iteration.
constexpr const RevivalAddressProfile* kAllRevivalProfiles[] = {
    &kRevival_1_02e,
    &kRevival_1_02f,
    &kRevival_1_02g,
    &kRevival_1_02h,
    &kRevival_1_02i,
};
constexpr size_t kRevivalProfileCount =
    sizeof(kAllRevivalProfiles) / sizeof(kAllRevivalProfiles[0]);
}}} // namespace netplay::bridge::takeover