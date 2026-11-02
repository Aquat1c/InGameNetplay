#pragma once

#include <cstddef>
#include <cstdint>

namespace netplay { namespace bridge { namespace takeover {

constexpr size_t kMaxErrorCodePatchBytes = 8;

constexpr size_t kMaxRoleFlagOffsets    = 4;
constexpr size_t kMaxSessionPtrOffsets  = 4;

constexpr size_t kMaxTournamentExePatches    = 4;
constexpr size_t kMaxTournamentExePatchBytes  = 20;

enum class RevivalSessionReadinessCheck : uint8_t
{
    None,
    EqualsOne,
    NonZero,
};

constexpr bool RevivalSessionReadinessSatisfied(
    RevivalSessionReadinessCheck check,
    uint32_t value)
{
    return check == RevivalSessionReadinessCheck::None
        || (check == RevivalSessionReadinessCheck::EqualsOne && value == 1u)
        || (check == RevivalSessionReadinessCheck::NonZero && value != 0u);
}

struct RevivalAddressProfile
{
    const char* versionTag;

    uint32_t peTimestamp;

    uint32_t efzFingerprint;

    uint32_t defaultImageBase;

    uintptr_t addrGameModeStructTable;
    uintptr_t addrGameModeCurrentIndex;

    uintptr_t roleFlagOffsets[kMaxRoleFlagOffsets];
    size_t    roleFlagOffsetCount;

    uintptr_t sessionPtrOffsets[kMaxSessionPtrOffsets];
    size_t    sessionPtrOffsetCount;

    uintptr_t globalStatePtrOffset;

    uintptr_t initFlagOffset;

    uintptr_t initByteOffset;

    uintptr_t initOnceGuardOffset;

    uintptr_t timerPtrOffset;

    uintptr_t renderContextBaseOffset;

    uintptr_t errorCodeIsZeroRva;
    size_t    errorCodeIsZeroPatchSize;

    uintptr_t startInitPlayerRva;

    uintptr_t inputSwapPairRva;

    uintptr_t frameHookRva;

    uintptr_t perFrameTickRva;

    uintptr_t sessionOffsetInitComplete;
    uintptr_t sessionOffsetInputDelay;
    uintptr_t sessionOffsetPingMs;

    // [0] AdjustPrediction threshold, [2] PingMs, [3] WaitLoop ping; Revival-owned, never written here.
    uintptr_t sessionOffsetPingStructBase;

    uintptr_t sessionOffsetHelperHandle;
    uintptr_t sessionOffsetHelperPid;
    uintptr_t sessionOffsetActivePlayer;
    uintptr_t sessionOffsetQueuePlayer;
    uintptr_t sessionOffsetHistoryPrimaryPtr;
    uintptr_t sessionOffsetHistorySecondaryPtr;
    uintptr_t sessionOffsetHistoryPrimaryVec;
    uintptr_t sessionOffsetHistorySecondaryVec;

    uintptr_t sessionOffsetCurrentFrame;       // +708 in 1.02e - frame counter
    uintptr_t sessionOffsetGameModeSnapshot;   // +716 - game mode at session start
    uintptr_t sessionOffsetMatchId;            // +712 - match identifier
    uintptr_t sessionOffsetSentinel;           // +1232 - INT_MAX-1 sentinel value

    uintptr_t sessionOffsetP1Name;             // +958 in 1.02e - P1 nickname
    uintptr_t sessionOffsetP2Name;             // +1086 in 1.02e - P2 nickname

    uintptr_t sessionOffsetP1Wins;             // +1224 in 1.02e - P1 win count
    uintptr_t sessionOffsetP2Wins;             // +1228 in 1.02e - P2 win count

    uintptr_t globalStateOffsetFlag4964;
    uintptr_t globalStateOffsetFlag4965;
    uintptr_t globalStateOffsetSessionByte;

    // MSVC std::deque layout on older builds; libstdc++ map/start/finish on 1.02j.
    uintptr_t tournamentInputQueueOffset;

    uintptr_t clearTextRva;

    uintptr_t setTextEnabledRva;

    uintptr_t renderContextGlobalOffset;

    uintptr_t tournamentExePatchAddr[kMaxTournamentExePatches];
    uint8_t   tournamentExePatchSize[kMaxTournamentExePatches];
    size_t    tournamentExePatchCount;

    // Jcc opcode replaced with jmp short (74/75 -> EB).
    static constexpr size_t kMaxExitProcessPatches = 4;
    uintptr_t exitProcessPatchRva[kMaxExitProcessPatches];
    uint8_t   exitProcessPatchOriginal[kMaxExitProcessPatches];
    size_t    exitProcessPatchCount;

    // 6-byte near-Jcc replaced with NOPs; an OR condition has several per call site, all must be patched.
    static constexpr size_t kMaxExitProcessNearJccPatches = 8;
    uintptr_t exitProcessNearJccRva[kMaxExitProcessNearJccPatches];
    size_t    exitProcessNearJccCount;

    uintptr_t rngEngineStateOffset;

    uint32_t launcherExeTimestamp;
    uint32_t launcherExeSizeOfImage;
    uint32_t launcherExeEntryPointRva;
    uint32_t launcherExeFileSize;
    const char* launcherExeSha256;
    uint32_t revivalDllFileSize;
    const char* revivalDllSha256;

    uintptr_t canonicalRoleFlagOffset;
    uintptr_t canonicalSessionPtrOffset;

    uintptr_t onlineSessionVtableRva;
    uintptr_t spectatorSessionVtableRva;
    uintptr_t practiceSessionVtableRva;
    uintptr_t tournamentSessionVtableRva;

    uintptr_t spectatorSessionOffsetHelperHandle;
    uintptr_t spectatorSessionOffsetHelperPid;

    // Legacy Online publishes initComplete == 1; 1.02j Online and Spectator publish a non-zero control field.
    uintptr_t onlineSessionReadinessOffset;
    RevivalSessionReadinessCheck onlineSessionReadinessCheck;
    uintptr_t spectatorSessionReadinessOffset;
    RevivalSessionReadinessCheck spectatorSessionReadinessCheck;

    uintptr_t tournamentExeHookHandlerRva;

    uintptr_t launcherCleanupTerminateCallRva;

    // 1.02j releases share an ABI, but code and vtables move: keep every RVA in the exact profile.
    uintptr_t initExportRva;
    uint32_t revivalDllSizeOfImage;
    uint32_t revivalDllEntryPointRva;
    uintptr_t replaySessionVtableRva;
    uintptr_t tournamentExitGuardRva;
    uintptr_t practicePostInitRva;
    uintptr_t spectatorPostInitRva;
    uintptr_t memorialPauseIntegrationRva;
    uintptr_t rollbackReadInputRva;
    uintptr_t tournamentDeletingDtorRva;
    uintptr_t onlineDeletingDtorRva;
    uintptr_t spectatorDeletingDtorRva;
    uintptr_t replayDeletingDtorRva;
    uintptr_t practiceDeletingDtorRva;

    bool usesProceduralDdrawTextApi;

    uint32_t companionDdrawFileSize;
    const char* companionDdrawSha256;

};

// Zeroed so detection fails closed instead of reusing a nearby profile.
constexpr RevivalAddressProfile kRevival_Unsupported = {
    "unsupported",                                      // versionTag
    0u,                                                  // peTimestamp
    0x4386998Fu,                                        // efzFingerprint (EFZ.exe is unchanged)
    0x10000000u,                                        // defaultImageBase
    0u,                                                  // addrGameModeStructTable
    0u,                                                  // addrGameModeCurrentIndex
    {0u, 0u, 0u, 0u},                                  // roleFlagOffsets
    0u,                                                 // roleFlagOffsetCount
    {0u, 0u, 0u, 0u},                                  // sessionPtrOffsets
    0u,                                                 // sessionPtrOffsetCount
    0u,                                                 // globalStatePtrOffset
    0u,                                                 // initFlagOffset
    0u,                                                 // initByteOffset
    0u,                                                 // initOnceGuardOffset
    0u,                                                 // timerPtrOffset
    0u,                                                 // renderContextBaseOffset
    0u,                                                 // errorCodeIsZeroRva
    0u,                                                 // errorCodeIsZeroPatchSize
    0u,                                                 // startInitPlayerRva
    0u,                                                 // inputSwapPairRva
    0u,                                                 // frameHookRva
    0u,                                                 // perFrameTickRva
    0u,                                                 // sessionOffsetInitComplete
    0u,                                                 // sessionOffsetInputDelay
    0u,                                                 // sessionOffsetPingMs
    0u,                                                 // sessionOffsetPingStructBase
    0u,                                                 // sessionOffsetHelperHandle
    0u,                                                 // sessionOffsetHelperPid
    0u,                                                 // sessionOffsetActivePlayer
    0u,                                                 // sessionOffsetQueuePlayer
    0u,                                                 // sessionOffsetHistoryPrimaryPtr
    0u,                                                 // sessionOffsetHistorySecondaryPtr
    0u,                                                 // sessionOffsetHistoryPrimaryVec
    0u,                                                 // sessionOffsetHistorySecondaryVec
    0u,                                                 // sessionOffsetCurrentFrame
    0u,                                                 // sessionOffsetGameModeSnapshot
    0u,                                                 // sessionOffsetMatchId
    0u,                                                 // sessionOffsetSentinel
    0u,                                                 // sessionOffsetP1Name
    0u,                                                 // sessionOffsetP2Name
    0u,                                                 // sessionOffsetP1Wins
    0u,                                                 // sessionOffsetP2Wins
    0u,                                                 // globalStateOffsetFlag4964
    0u,                                                 // globalStateOffsetFlag4965
    0u,                                                 // globalStateOffsetSessionByte
    0u,                                                 // tournamentInputQueueOffset
    0u,                                                 // clearTextRva
    0u,                                                 // setTextEnabledRva
    0u,                                                 // renderContextGlobalOffset
    {0u, 0u, 0u, 0u},                                  // tournamentExePatchAddr
    {0u, 0u, 0u, 0u},                                  // tournamentExePatchSize
    0u,                                                 // tournamentExePatchCount
    {0u, 0u, 0u, 0u},                                  // exitProcessPatchRva
    {0u, 0u, 0u, 0u},                                  // exitProcessPatchOriginal
    0u,                                                 // exitProcessPatchCount
    {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},                  // exitProcessNearJccRva
    0u,                                                 // exitProcessNearJccCount
    0u,                                                 // rngEngineStateOffset
    0u, 0u, 0u, 0u, nullptr,                           // launcher EXE identity
    0u, nullptr,                                        // Revival DLL file identity
    0u, 0u,                                             // canonical role/session globals
    0u, 0u, 0u, 0u,                                    // session vtable RVAs
    0u, 0u,                                             // spectator helper handle/PID offsets
    0u, RevivalSessionReadinessCheck::None,             // Online readiness
    0u, RevivalSessionReadinessCheck::None,             // Spectator readiness
    0u,                                                 // Tournament EXE hook handler RVA
    0u,                                                 // launcher cleanup TerminateProcess RVA
};

// EfzRevival.dll v1.02e - original release, baseline for all addresses.
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
    0x000A06ECu,                                        // rngEngineStateOffset
    0x5EA87633u, 0x000FD000u, 0x00057F8Fu, 1005056u,   // launcher EXE PE/file identity
    "44CCD28BCFDBD2F407A5DAA03EBB16824D332660CDC07D0A27E69DC4BEB5B19E",
    704000u,
    "0A30E2309D36C323E8085886E4C661684F495DA515F2CE1FE67FB70432F0F515",
    0x000A05D0u, 0x000A02CCu,                          // canonical role/session globals
    0x000902C0u, 0x000902E4u, 0x00090404u, 0x0009044Cu,
    52u, 412u,                                          // spectator helper handle/PID
    1220u, RevivalSessionReadinessCheck::EqualsOne,     // Online initComplete == 1
    0x42Cu, RevivalSessionReadinessCheck::NonZero,      // Spectator control published
    0x0006E260u,                                        // Tournament EXE hook handler
    0x00038E88u,                                        // launcher cleanup TerminateProcess
};

// EfzRevival.dll v1.02f - minor revision, same .data layout as 1.02e.
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
    0x000A06ECu,                                        // rngEngineStateOffset
    0x5F8C58B0u, 0x000FD000u, 0x00057FDFu, 1005056u,
    "B40323D0472A771E9D176F321E8EA47F7BFA1E6A5AD8C38B99649FD4BEA15019",
    704000u,
    "B9D21C86F1403EEC9C6262414F4ABDBB9BC2A7ED05FC90222363C2D15802DC86",
    0x000A05D0u, 0x000A02CCu,
    0x000902C0u, 0x000902E4u, 0x00090404u, 0x0009044Cu,
    52u, 412u,
    1220u, RevivalSessionReadinessCheck::EqualsOne,
    0x42Cu, RevivalSessionReadinessCheck::NonZero,
    0x0006E260u,
    0x00038EC8u,
};

// Custom framestepping 1.02f build: same data layout as stock 1.02f/1.02g
constexpr RevivalAddressProfile kRevival_1_02f_framestepping = {
    "1.02f-framestepping",                            // versionTag
    0x6085E718u,                                       // peTimestamp
    0x4386998Fu,                                       // efzFingerprint
    0x10000000u,                                       // defaultImageBase
    0x00790110u,                                       // addrGameModeStructTable
    0x00790148u,                                       // addrGameModeCurrentIndex
    {0x000A05D0u, 0x000A05F0u, 0u, 0u},                // roleFlagOffsets
    2,                                                 // roleFlagOffsetCount
    {0x000A02CCu, 0u, 0u, 0u},                         // sessionPtrOffsets
    1,                                                 // sessionPtrOffsetCount
    0x000A07B8u,                                       // globalStatePtrOffset
    0x000A05D4u,                                       // initFlagOffset
    0x000A0289u,                                       // initByteOffset
    0x000A0774u,                                       // initOnceGuardOffset
    0x000A0764u,                                       // timerPtrOffset
    0x000A0760u,                                       // renderContextBaseOffset
    0x000021E0u,                                       // errorCodeIsZeroRva
    8u,                                                // errorCodeIsZeroPatchSize
    0x00072AB0u,                                       // startInitPlayerRva
    0x0006CCE0u,                                       // inputSwapPairRva
    0x0006E7A0u,                                       // frameHookRva
    0x0006E780u,                                       // perFrameTickRva
    1220u,                                             // sessionOffsetInitComplete
    688u,                                              // sessionOffsetInputDelay
    936u,                                              // sessionOffsetPingMs
    928u,                                              // sessionOffsetPingStructBase
    700u,                                              // sessionOffsetHelperHandle
    1216u,                                             // sessionOffsetHelperPid
    680u,                                              // sessionOffsetActivePlayer
    684u,                                              // sessionOffsetQueuePlayer
    824u,                                              // sessionOffsetHistoryPrimaryPtr
    828u,                                              // sessionOffsetHistorySecondaryPtr
    788u,                                              // sessionOffsetHistoryPrimaryVec
    800u,                                              // sessionOffsetHistorySecondaryVec
    708u,                                              // sessionOffsetCurrentFrame
    716u,                                              // sessionOffsetGameModeSnapshot
    712u,                                              // sessionOffsetMatchId
    1232u,                                             // sessionOffsetSentinel
    958u,                                              // sessionOffsetP1Name (raw wchar_t[64] in config struct)
    1086u,                                             // sessionOffsetP2Name (raw wchar_t[64] in config struct)
    1224u,                                             // sessionOffsetP1Wins
    1228u,                                             // sessionOffsetP2Wins
    4964u,                                             // globalStateOffsetFlag4964
    4965u,                                             // globalStateOffsetFlag4965
    82563u,                                            // globalStateOffsetSessionByte
    740u,                                              // tournamentInputQueueOffset
    0x0006C280u,                                       // clearTextRva
    0x0006C240u,                                       // setTextEnabledRva
    0x000A0778u,                                       // renderContextGlobalOffset
    {0x763F04u, 0x763E50u, 0x754C1Au, 0x7599EDu},      // tournamentExePatchAddr
    {7, 7, 1, 20},                                     // tournamentExePatchSize
    4,                                                 // tournamentExePatchCount
    {0x0007933Du, 0x0007254Fu, 0u, 0u},                // exitProcessPatchRva
    {0x74u, 0x74u, 0u, 0u},                            // exitProcessPatchOriginal
    2,                                                 // exitProcessPatchCount
    {0x000723C6u, 0x0007274Bu, 0x0007275Eu, 0x00074531u,
     0x00074544u, 0x00074551u, 0u, 0u},               // exitProcessNearJccRva
    6,                                                 // exitProcessNearJccCount
    0x000A06ECu,                                       // rngEngineStateOffset
    0x5F8C58B0u, 0x000FD000u, 0x00057FDFu, 1005056u,
    "B40323D0472A771E9D176F321E8EA47F7BFA1E6A5AD8C38B99649FD4BEA15019",
    704512u,
    "D249BFFC0F879601FFF613C8D6D921FE7109DC617FF2CFB7A7E8C539068C3AA5",
    0x000A05D0u, 0x000A02CCu,
    0x000902C0u, 0x000902E4u, 0x00090404u, 0x0009044Cu,
    52u, 412u,
    1220u, RevivalSessionReadinessCheck::EqualsOne,
    0x42Cu, RevivalSessionReadinessCheck::NonZero,
    0x0006E470u,
    0x00038EC8u,
};

// EfzRevival.dll v1.02g - session objects enlarged (host 0x5D0→0x690)
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
    {0x0007937Du, 0x0007257Fu, 0u, 0u},                 // exitProcessPatchRva
    {0x74u, 0x74u, 0u, 0u},                             // exitProcessPatchOriginal
    2,                                                  // exitProcessPatchCount
    {0x000723F5u, 0x000723FEu, 0x0007277Bu, 0x0007278Eu,
     0x00074561u, 0x00074574u, 0x00074581u, 0u},        // exitProcessNearJccRva
    7,                                                  // exitProcessNearJccCount
    0x000A06ECu,                                        // rngEngineStateOffset
    0x623F85D8u, 0x000FE000u, 0x0005832Fu, 1006080u,
    "949C9AAA28F1DDDE89931DC5303500777939A64396BA9D79486E21DF7D303872",
    704512u,
    "8A5A268DDC419E38D0E5BD6E3974732D2C9C4A17A62FDDE6E2724B8D80D250E7",
    0x000A05D0u, 0x000A02CCu,
    0x000902C0u, 0x000902E4u, 0x00090404u, 0x0009044Cu,
    52u, 412u,
    1220u, RevivalSessionReadinessCheck::EqualsOne,
    0x42Cu, RevivalSessionReadinessCheck::NonZero,
    0x0006E470u,
    0x00038FE8u,
};

// EfzRevival.dll v1.02h - .data shifted +0x20 from e/f/g.
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
    0x000A070Cu,                                        // rngEngineStateOffset
    0x6292935Eu, 0x000FE000u, 0x00058500u, 1007104u,
    "E2C8B136A36F829F4178DAF96655EE952FFA465776AE204219CAFC24B74B488F",
    707584u,
    "0687E74BB40A8AADC91B591284BC962A320078394BDF7D23B9741D54E937F8AA",
    0x000A05F0u, 0x000A02ECu,
    0x00090400u, 0x00090424u, 0x00090544u, 0x0009058Cu,
    52u, 412u,
    1220u, RevivalSessionReadinessCheck::EqualsOne,
    0x42Cu, RevivalSessionReadinessCheck::NonZero,
    0x0006EAE0u,
    0x00038F78u,
};

// EfzRevival.dll v1.02i - largest version (SizeOfImage 0xB3000 vs 0xB2000).
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
    0x000A1788u,                                        // rngEngineStateOffset
    0x63BF27B6u, 0x000FE000u, 0x0005644Fu, 1005568u,
    "13389473CC9D1B1C829AA6252AAD3DF654194ABFA37CB572AB6FA514F971A9E0",
    709632u,
    "18F91D5CDD8F17FB6CDE90E1AAA3C58A374B4AA80E8E906FDD88CB7FEB9D62D0",
    0x000A15FCu, 0x000A15F8u,
    0x000914F0u, 0x00091514u, 0x00091634u, 0x0009167Cu,
    52u, 412u,
    1228u, RevivalSessionReadinessCheck::EqualsOne,
    0x434u, RevivalSessionReadinessCheck::NonZero,
    0x0006EDB0u,
    0x0003A068u,
};

// EfzRevival.dll v1.02j - MinGW refactor build.
constexpr RevivalAddressProfile kRevival_1_02j = {
    "1.02j",                                            // versionTag
    0x6A36A6AEu,                                        // peTimestamp
    0x4386998Fu,                                        // efzFingerprint
    0x70000000u,                                        // defaultImageBase
    0x00790110u,                                        // addrGameModeStructTable
    0x00790148u,                                        // addrGameModeCurrentIndex
    {0x0014EC40u, 0u, 0u, 0u},                          // roleFlagOffsets
    1,                                                  // roleFlagOffsetCount
    {0x0014E980u, 0u, 0u, 0u},                          // sessionPtrOffsets
    1,                                                  // sessionPtrOffsetCount
    0x0014E924u,                                        // globalStatePtrOffset (patch ctx + 0x64)
    0x0014EC44u,                                        // initFlagOffset / screen cache
    0x0014ED64u,                                        // initByteOffset
    0u,                                                 // initOnceGuardOffset (no old standalone guard)
    0x0014E914u,                                        // timerPtrOffset (patch ctx + 0x54)
    0x0014E8C0u,                                        // renderContextBaseOffset
    0u,                                                 // errorCodeIsZeroRva (old MSVC helper absent)
    0u,                                                 // errorCodeIsZeroPatchSize
    0x000533B0u,                                        // startInitPlayerRva
    0x000532A0u,                                        // inputSwapPairRva
    0x00040200u,                                        // frameHookRva
    0x00040AD0u,                                        // perFrameTickRva
    0u,                                                 // sessionOffsetInitComplete (field removed)
    0x310u,                                             // sessionOffsetInputDelay
    0x448u,                                             // sessionOffsetPingMs
    0x440u,                                             // sessionOffsetPingStructBase
    0x31Cu,                                             // sessionOffsetHelperHandle
    0x560u,                                             // sessionOffsetHelperPid
    0x308u,                                             // sessionOffsetActivePlayer
    0x30Cu,                                             // sessionOffsetQueuePlayer
    0x398u,                                             // sessionOffsetHistoryPrimaryPtr
    0x39Cu,                                             // sessionOffsetHistorySecondaryPtr
    0x374u,                                             // sessionOffsetHistoryPrimaryVec
    0x380u,                                             // sessionOffsetHistorySecondaryVec
    0x324u,                                             // sessionOffsetCurrentFrame
    0x32Cu,                                             // sessionOffsetGameModeSnapshot
    0x328u,                                             // sessionOffsetMatchId
    0x570u,                                             // sessionOffsetSentinel
    0x45Eu,                                             // sessionOffsetP1Name
    0x4DEu,                                             // sessionOffsetP2Name
    0x568u,                                             // sessionOffsetP1Wins (+0x564 is the current game number, not a win count)
    0x56Cu,                                             // sessionOffsetP2Wins
    4964u,                                              // globalStateOffsetFlag4964
    4965u,                                              // globalStateOffsetFlag4965
    82563u,                                             // globalStateOffsetSessionByte
    0x340u,                                             // tournamentInputQueueOffset
    0x00077DC0u,                                        // clearTextRva
    0x00077B10u,                                        // setTextEnabledRva
    0x0014E8D8u,                                        // renderContextGlobalOffset (patch ctx + 0x18)
    {0x763F04u, 0x763E50u, 0x754C1Au, 0x7599EDu},      // tournamentExePatchAddr
    {7, 7, 1, 20},                                      // tournamentExePatchSize
    4,                                                  // tournamentExePatchCount
    {0u, 0u, 0u, 0u},                                   // exitProcessPatchRva
    {0u, 0u, 0u, 0u},                                   // exitProcessPatchOriginal
    0,                                                  // exitProcessPatchCount
    {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},                  // exitProcessNearJccRva
    0,                                                  // exitProcessNearJccCount
    0x0014E984u,                                        // rngEngineStateOffset
    0x6A3696C8u, 0x00294000u, 0x00001460u, 2673664u,
    "194710A1C3DFEA3C50AC7CC5779794B986F6DF0F7009FFD5696552467F9CC027",
    1914880u,
    "CEDD606869EE6CFA227B385AF27DC92ABC13FE9C2D432935BCBE3B789468A169",
    0x0014EC40u, 0x0014E980u,
    0x0016FEF0u, 0x0016FF20u, 0x0016FF80u, 0x0016FEB0u,
    0x64u, 0x2E0u,
    0x658u, RevivalSessionReadinessCheck::NonZero,
    0x588u, RevivalSessionReadinessCheck::NonZero,
    0x0003CE80u,
    0x00089A11u,
    0x0012E2B0u, 0x001D9000u, 0x000011F0u,
    0x0016FF50u,
    0x0004683Fu,
    0x0007DE40u,
    0x0005E640u,
    0x000777A0u,
    0x00052740u,
    0x00048550u,
    0x00058580u,
    0x00062900u,
    0x00075CD0u,
    0x00080630u,
    false,
    0u, nullptr,
};

// EfzRevival.dll v1.02j - newer renderer-split compile profile.
constexpr RevivalAddressProfile kRevival_1_02j_20260802 = {
    "1.02j",                                            // versionTag / ABI family
    0x6A6F0F7Bu,                                        // peTimestamp
    0x4386998Fu,                                        // efzFingerprint
    0x70000000u,                                        // defaultImageBase
    0x00790110u,                                        // addrGameModeStructTable
    0x00790148u,                                        // addrGameModeCurrentIndex
    {0x0014EC40u, 0u, 0u, 0u},                          // roleFlagOffsets
    1,                                                  // roleFlagOffsetCount
    {0x0014E980u, 0u, 0u, 0u},                          // sessionPtrOffsets
    1,                                                  // sessionPtrOffsetCount
    0x0014E920u,                                        // globalStatePtrOffset
    0x0014EC44u,                                        // initFlagOffset / screen cache
    0x0014ED64u,                                        // initByteOffset
    0u,                                                 // initOnceGuardOffset
    0x0014E8C4u,                                        // timerPtrOffset
    0x0014E8C0u,                                        // renderContextBaseOffset / patch context
    0u,                                                 // errorCodeIsZeroRva
    0u,                                                 // errorCodeIsZeroPatchSize
    0x000542B0u,                                        // startInitPlayerRva
    0x000541A0u,                                        // inputSwapPairRva
    0x000418E0u,                                        // frameHookRva
    0x000421D0u,                                        // perFrameTickRva
    0u,                                                 // sessionOffsetInitComplete
    0x310u,                                             // sessionOffsetInputDelay
    0x448u,                                             // sessionOffsetPingMs
    0x440u,                                             // sessionOffsetPingStructBase
    0x31Cu,                                             // sessionOffsetHelperHandle
    0x560u,                                             // sessionOffsetHelperPid
    0x308u,                                             // sessionOffsetActivePlayer
    0x30Cu,                                             // sessionOffsetQueuePlayer
    0x398u,                                             // sessionOffsetHistoryPrimaryPtr
    0x39Cu,                                             // sessionOffsetHistorySecondaryPtr
    0x374u,                                             // sessionOffsetHistoryPrimaryVec
    0x380u,                                             // sessionOffsetHistorySecondaryVec
    0x324u,                                             // sessionOffsetCurrentFrame
    0x32Cu,                                             // sessionOffsetGameModeSnapshot
    0x328u,                                             // sessionOffsetMatchId
    0x570u,                                             // sessionOffsetSentinel
    0x45Eu,                                             // sessionOffsetP1Name
    0x4DEu,                                             // sessionOffsetP2Name
    0x568u,                                             // sessionOffsetP1Wins
    0x56Cu,                                             // sessionOffsetP2Wins
    4964u,                                              // globalStateOffsetFlag4964
    4965u,                                              // globalStateOffsetFlag4965
    82563u,                                             // globalStateOffsetSessionByte
    0x340u,                                             // tournamentInputQueueOffset
    0x00078B80u,                                        // clearTextRva (procedural Ddraw thunk)
    0x000788D0u,                                        // setTextEnabledRva (__stdcall bool)
    0u,                                                 // renderContextGlobalOffset (removed)
    {0x763F04u, 0x763E50u, 0x754C1Au, 0x7599EDu},      // tournamentExePatchAddr
    {7, 7, 1, 20},                                      // tournamentExePatchSize
    4,                                                  // tournamentExePatchCount
    {0u, 0u, 0u, 0u},                                  // exitProcessPatchRva
    {0u, 0u, 0u, 0u},                                  // exitProcessPatchOriginal
    0,                                                  // exitProcessPatchCount
    {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u},                  // exitProcessNearJccRva
    0,                                                  // exitProcessNearJccCount
    0x0014E984u,                                        // rngEngineStateOffset
    0x6A6F0E2Bu, 0x00294000u, 0x00001460u, 2673664u,
    "7C7707322B7CDA0B1479DFE97A7260D9E971BA162DFCE755C894B9626FEB1C42",
    1911296u,
    "62D9ED74B7931E2BC7EA208109425BC25207C82FACA91A96A55C778F8D568F32",
    0x0014EC40u, 0x0014E980u,
    0x0016FBD0u, 0x0016FC00u, 0x0016FC60u, 0x0016FB90u,
    0x64u, 0x2E0u,
    0x658u, RevivalSessionReadinessCheck::NonZero,
    0x588u, RevivalSessionReadinessCheck::NonZero,
    0x0003CEC0u,
    0x00089A11u,
    0x0012DB70u, 0x001D8000u, 0x000011F0u,
    0x0016FC30u,
    0x0004777Fu,
    0x0007F080u,
    0x0005F540u,
    0x00078620u,
    0x00053640u,
    0x00049490u,
    0x00059480u,
    0x00063800u,
    0x00076B70u,
    0x00081870u,
    true,
    1314816u,
    "0E072342D87D240E18969BC3663032E0E370EABA24D1FBB8E6C1F6216AD9F603",
};

constexpr const RevivalAddressProfile* kAllRevivalProfiles[] = {
    &kRevival_1_02e,
    &kRevival_1_02f,
    &kRevival_1_02f_framestepping,
    &kRevival_1_02g,
    &kRevival_1_02h,
    &kRevival_1_02i,
    &kRevival_1_02j,
    &kRevival_1_02j_20260802,
};
constexpr size_t kRevivalProfileCount =
    sizeof(kAllRevivalProfiles) / sizeof(kAllRevivalProfiles[0]);
}}} // namespace netplay::bridge::takeover
