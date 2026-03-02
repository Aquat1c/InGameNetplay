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

    // -----------------------------------------------------------------------
    // Session object field offsets (byte offsets from session pointer)
    // -----------------------------------------------------------------------

    uintptr_t sessionOffsetInitComplete;
    uintptr_t sessionOffsetInputDelay;
    uintptr_t sessionOffsetPingMs;
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

// EfzRevival.dll v1.02e (current default).
constexpr RevivalAddressProfile kRevival_1_02e = {
    "1.02e",                                            // versionTag
    0x4386998Fu,                                        // efzFingerprint
    0x10000000u,                                        // defaultImageBase
    0x00790110u,                                        // addrGameModeStructTable
    0x00790148u,                                        // addrGameModeCurrentIndex
    {0x00A05D0u, 0x00A05F0u, 0x00A15FCu, 0u},          // roleFlagOffsets
    3,                                                  // roleFlagOffsetCount
    {0x00A02CCu, 0x00A02ECu, 0u, 0u},                  // sessionPtrOffsets
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
    1220u,                                              // sessionOffsetInitComplete
    688u,                                               // sessionOffsetInputDelay
    936u,                                               // sessionOffsetPingMs
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
    4964u,                                              // globalStateOffsetFlag4964
    4965u,                                              // globalStateOffsetFlag4965
    82563u,                                             // globalStateOffsetSessionByte
    740u,                                               // tournamentInputQueueOffset
    0x0006C070u,                                        // clearTextRva
    0x0006C030u,                                        // setTextEnabledRva
    0x000A0778u,                                        // renderContextGlobalOffset
    {0x763F04u, 0x763E50u, 0x754C1Au, 0x7599EDu},       // tournamentExePatchAddr
    {7, 7, 1, 20},                                      // tournamentExePatchSize
    4,                                                  // tournamentExePatchCount

    // DLL ExitProcess call-site patches (conditional jump → unconditional).
    //
    // Site 5: sub_10079090 (tournament mode-0 handler)
    //   0x7909D: jz +8 (0x74) → jmp +8 (0xEB) — skips ExitProcess when
    //   sub_1002B150 says results container has data.
    //
    // Site 1: tournament results handler (in sub_100718D0)
    //   0x7215D: jnz +0x2F (0x75) → jmp +0x2F (0xEB) — skips ExitProcess
    //   when EFZ_GameMode_GetCurrentIndex() == 0 && v47 == 1.
    //
    // Site 2: EFZ_Rollback_BatchAdvanceSimple (sub_10072310)
    //   0x7231F: jz +8 (0x74) → jmp +8 (0xEB) — skips ExitProcess
    //   when self[179] (exit flag) is non-zero.
    {0x0007909Du, 0x0007215Du, 0x0007231Fu, 0u},          // exitProcessPatchRva
    {0x74u, 0x75u, 0x74u, 0u},                            // exitProcessPatchOriginal
    3,                                                    // exitProcessPatchCount

    // Near-Jcc (6-byte) ExitProcess call-site patches.
    //
    // Site 3: EFZ_Main_RollbackLoopTick (sub_10072500)
    //   Two Jcc's guarding the "peer died" ExitProcess block at 0x727D7:
    //   0x7251B: 0F 84 B6 02 00 00  jz near +0x2B6 → exit block
    //     (fires when !EFZ_Process_IsActive(handle))
    //   0x7252E: 0F 84 A3 02 00 00  jz near +0x2A3 → exit block
    //     (fires when !EFZ_Queue_IsEmpty(queue))
    //
    // Site 4: sub_100742A0 (spectator/lobby tick)
    //   Three Jcc's guarding the "quit" ExitProcess block at 0x74490:
    //   0x742E1: 0F 84 A9 01 00 00  jz near +0x1A9 → exit block
    //     (fires when !sub_10073970(handle))
    //   0x742F4: 0F 84 96 01 00 00  jz near +0x196 → exit block
    //     (fires when !EFZ_Queue_IsEmpty(queue))
    //   0x74301: 0F 84 89 01 00 00  jz near +0x189 → exit block
    //     (fires when EFZ_GlobalStatus_ComputeFlagMask() == 32)
    {0x0007251Bu, 0x0007252Eu, 0x000742E1u, 0x000742F4u, 0x00074301u, 0u, 0u, 0u},
                                                          // exitProcessNearJccRva
    5,                                                    // exitProcessNearJccCount
};

}}} // namespace netplay::bridge::takeover
