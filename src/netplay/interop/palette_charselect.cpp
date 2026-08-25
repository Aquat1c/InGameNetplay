#include "netplay/interop/palette_charselect.h"

#include <windows.h>

#include <MinHook.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include "logger.h"
#include "netplay/core/mod_settings.h"
#include "netplay/interop/overlay_channel.h"
#include "netplay/interop/overlay_protocol.h"
#include "netplay/interop/palette_apply.h"
#include "netplay/interop/palette_socket.h"
#include "netplay/interop/palette_source.h"

namespace netplay::interop::charselect
{
namespace
{
namespace P = netplay::interop::protocol;

// Memorial efz.exe RVAs (version-invariant).
constexpr std::uintptr_t kRvaReloadCharacterPalette = 0x003595D0u;
constexpr std::uintptr_t kRvaInitCharacterSelect = 0x003598D0u;
constexpr std::uintptr_t kRvaInitResultScreen = 0x00374B00u;   // win screen
constexpr std::uintptr_t kVaCurrentScreenIndex = 0x00790148u;
constexpr int kScreenCharSelect = 1;

// Result-screen field offsets (gameData = *(resultScreenCtx + 28)).
constexpr std::uint32_t kWinnerIndexOffset = 4940u;   // 0 = P1 won, 1 = P2 won

// Char-select object + game state offsets.

// Char-select object field offsets.
constexpr std::uint32_t kCharIdBase = 1340u;   // + side  (P1 1340, P2 1341)
constexpr std::uint32_t kColorBase = 1342u;    // + side  (P1 1342, P2 1343)

// Side this client controls (broadcasts) and applies the peer's onto. 0=P1 for
// loopback; the socket transport uses the configured ModInteropSide.
int g_localSide = 0;
bool g_useSocket = false;   // true = socket transport, false = loopback

// Polling thread (matches the original mod): reloadCharacterPalette does NOT
// fire while EDIT COLOR is active, so a 60Hz thread drives the exchange + apply
// during char-select. g_csObj is the current char-select object (from the init
// hook). g_paletteMutex serializes channel + palette writes between this thread
// and the reload hook. g_lastCustom tracks per-side custom state so we revert to
// stock exactly once on the custom->stock edge.
std::atomic<std::uint32_t> g_csObj{0};
std::mutex g_paletteMutex;
std::thread g_pollThread;
std::atomic<bool> g_pollStop{false};
std::atomic<bool> g_pollRunning{false};
bool g_lastCustom[2] = {false, false};

// Confirmed custom row per side, cached from char-select and held through the
// match so the win screen can apply the winner's palette. Flushed at the next
// char-select init (i.e. after the win screen ends, for the next game). Guarded
// by g_paletteMutex. A CLEAR row (slotByte==0) means "no custom" -> no win apply.
P::PaletteBlobBody g_matchRow[2] = {};

using ReloadPalette_t = int(__thiscall*)(void* gameContext, char playerIndex);
using InitCharSelect_t = int(__thiscall*)(void* screenContext);
using InitResultScreen_t = int(__thiscall*)(void* screenContext);

ReloadPalette_t g_origReload = nullptr;
InitCharSelect_t g_origInit = nullptr;
InitResultScreen_t g_origResult = nullptr;
void* g_reloadTarget = nullptr;
void* g_initTarget = nullptr;
void* g_resultTarget = nullptr;
bool g_installed = false;
bool g_minhookReady = false;

// Loopback echo sink: copy the outbound frame, feed it straight back to the
// channel. PaletteBlobs get their side flipped so our P1 selection lands as the
// "peer" P2 row; Hello/Ack pass through so the handshake completes locally.
bool LoopbackSink(const std::uint8_t* datagram, std::size_t len, void*)
{
    std::uint8_t copy[P::kMaxFrameBytes];
    if (datagram == nullptr || len == 0 || len > sizeof(copy))
    {
        return true;
    }
    std::memcpy(copy, datagram, len);

    // Echo everything back UNCHANGED - including the PaletteBlob's side - so our
    // own row is cached as the "peer" row for our OWN side and applied to our
    // portrait while we hold a custom color. (Real online caches the remote's
    // row on the remote side; the apply path is identical.)
    OverlayChannel::Instance().OnInboundDatagram(copy, len);
    return true;
}

bool ReadCharAndColor(std::uint32_t csObj, int side,
                      std::uint8_t* charId, std::uint8_t* colorSlot)
{
    bool ok = false;
    __try
    {
        *charId = *reinterpret_cast<const std::uint8_t*>(
            csObj + kCharIdBase + static_cast<std::uint32_t>(side));
        *colorSlot = *reinterpret_cast<const std::uint8_t*>(
            csObj + kColorBase + static_cast<std::uint32_t>(side));
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    return ok;
}

// Read the EDIT-COLOR (custom color selected) flag for `side` BEFORE the
// original reload runs. The original consumes/clears it during processing, which
// is exactly why reading it afterward always saw 0 - the core of the earlier
// bug. gameData = *(csObj+28); flag = gameData + 4920 + 4*side (offline-mod
// verified). SEH-guarded.
bool ReadEditColorFlag(std::uint32_t csObj, int side)
{
    bool on = false;
    __try
    {
        const std::uint32_t gd =
            *reinterpret_cast<const std::uint32_t*>(csObj + 28u);
        if (gd != 0)
        {
            on = *reinterpret_cast<const std::uint8_t*>(
                     gd + 4920u + 4u * static_cast<std::uint32_t>(side)) != 0;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        on = false;
    }
    return on;
}

// Char object pointer for a side (gameContext[side+3] = *(csObj+12+4*side)).
// Guards the revert reload so it never derefs an unselected char object.
bool CharObjValid(std::uint32_t csObj, int side)
{
    bool valid = false;
    __try
    {
        valid = *reinterpret_cast<const std::uint32_t*>(
                    csObj + 12u + 4u * static_cast<std::uint32_t>(side)) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { valid = false; }
    return valid;
}

int ReadCurrentScreen()
{
    int screen = -1;
    __try { screen = *reinterpret_cast<const int*>(kVaCurrentScreenIndex); }
    __except (EXCEPTION_EXECUTE_HANDLER) { screen = -1; }
    return screen;
}

// True once stage select has begun. Memorial's char-select player-state machine
// (csObj+1182+side) mapped from the decomp update loop + stage renderer:
//   5        initial character grid (browsing)
//   0,1,2    navigation / transitions
//   3        EDIT COLOR (custom palette selection)
//   6        character confirmed
//   7        stage-select ENTRY: sets both to 7 and loads system\stage\all.dat
//            -> idx 175 + the stage's own palette -> idx 191..254, setPaletteRange
//   4        stage-select STABLE state (7 falls straight to 4; the stage wheel
//            renderer at 191851 groups {4,7,8}); also 9->4 and 10->4
//   8,9,10   stage sub-states / transitions
// So the STAGE set is {4,7,8,9,10}; the portrait phases are {0,1,2,3,5,6}. Once
// EITHER side is in a stage state we must stop pushing the portrait palette or
// our 60Hz re-push overwrites the game's stage palette in 175-254 every 16ms
// ("blit on the all.dat/x.dat page"). The earlier >=7 gate MISSED state 4 - the
// state you sit in for essentially all of stage select - which is why it kept
// corrupting. (State 4 is a char-select value ONLY for a CPU player in offline
// arcade/VS-CPU modes, never in online netplay, so it is safe to treat as stage
// here.) Stage states are always set for BOTH sides together, so checking either
// is correct. On a fault we assume stage (safe: skip).
bool InStageFlow(std::uint32_t csObj)
{
    bool inStage = true;
    __try
    {
        const std::uint8_t s0 = *reinterpret_cast<const std::uint8_t*>(csObj + 1182u);
        const std::uint8_t s1 = *reinterpret_cast<const std::uint8_t*>(csObj + 1183u);
        inStage = (s0 == 4u || s0 >= 7u || s1 == 4u || s1 >= 7u);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { inStage = true; }
    return inStage;
}

// True once the char-select screen is fading out (to the netplay menu on cancel,
// or into battle). Screen-level exit flag at csObj+45 (kOffsetScreenExitState);
// the game sets it non-zero when the fade begins. If we keep re-pushing our
// custom palette during the fade, entries 175-254 stay at full brightness while
// the rest of the screen fades - the portrait "keeps up itself". So we must back
// off the moment the exit flag is set. On a fault we assume leaving (safe: skip).
bool ScreenLeavingCharSelect(std::uint32_t csObj)
{
    bool leaving = true;
    __try
    {
        leaving = *reinterpret_cast<const std::uint8_t*>(csObj + 45u) != 0u;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { leaving = true; }
    return leaving;
}

// Diagnostic snapshot of the raw bytes the gates depend on, so a live log can
// confirm the actual char-select vs stage-select state values (verify 1182/45).
struct CsDiag { std::uint8_t s0, s1, exitFlag; bool ok; };
CsDiag ReadCsDiag(std::uint32_t csObj)
{
    CsDiag d{};
    __try
    {
        d.s0 = *reinterpret_cast<const std::uint8_t*>(csObj + 1182u);
        d.s1 = *reinterpret_cast<const std::uint8_t*>(csObj + 1183u);
        d.exitFlag = *reinterpret_cast<const std::uint8_t*>(csObj + 45u);
        d.ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { d.ok = false; }
    return d;
}

// Process one side: broadcast our row, apply the custom palette (ours or the
// peer's), or revert to stock on the custom->stock edge. Caller holds
// g_paletteMutex. origAlreadyRan: true from the reload hook (orig built stock);
// false from the poll thread (must rebuild stock itself to revert).
void ProcessSideLocked(std::uint32_t csObj, int side, bool origAlreadyRan)
{
    OverlayChannel& ch = OverlayChannel::Instance();
    if (!ch.IsActive() || csObj == 0 || (side != 0 && side != 1))
    {
        return;
    }
    // Stop all palette work once we leave active selection: stage select (the
    // preview shares our hardware indices - the BME wrong-surface bug) OR the
    // screen fading out (our re-push would fight the game's fade and pin the
    // portrait lit). Clear the latch so we don't fire a spurious revert on the
    // way out and so the fade proceeds straight from what is currently shown.
    if (InStageFlow(csObj) || ScreenLeavingCharSelect(csObj))
    {
        g_lastCustom[0] = false;
        g_lastCustom[1] = false;
        return;
    }
    P::PaletteBlobBody applyRow{};
    bool custom = false;
    if (side == g_localSide)
    {
        // Our own selection: broadcast + apply a custom .pal ONLY while EDIT
        // COLOR is active; otherwise a CLEAR row so both we and the peer fall
        // back to stock. This is the original mod's flag gating.
        const bool editColor = ReadEditColorFlag(csObj, side);
        std::uint8_t charId = 0xFF, colorSlot = 0xFF;
        if (ReadCharAndColor(csObj, side, &charId, &colorSlot)
            && source::CharFolderName(charId) != nullptr)
        {
            P::PaletteBlobBody local{};
            if (editColor)
            {
                (void)source::LoadLocalRow(std::string(),
                                           static_cast<std::uint8_t>(side),
                                           charId, colorSlot, &local);
            }
            else
            {
                local = source::AssembleRow(static_cast<std::uint8_t>(side),
                                            charId, colorSlot, false, nullptr);
            }
            ch.SubmitLocalPaletteRow(local);
            applyRow = local;
            custom = (local.slotByte != 0);
        }
    }
    else
    {
        // Peer side: apply the received row; its custom-ness already encodes the
        // peer's EDIT-COLOR gating (they only broadcast custom while editing).
        custom = ch.GetPeerPaletteRow(side, &applyRow) && applyRow.slotByte != 0;
    }

    // Remember this side's latest confirmed row for the win screen. The last
    // update before stage-flow (the gate above) is the locked-in selection; a
    // CLEAR row here correctly disables the win-screen apply for that side.
    g_matchRow[side] = applyRow;

    if (custom)
    {
        (void)apply::ApplyRowToCharSelectPortrait(csObj, side, applyRow);
        if (!g_lastCustom[side])
        {
            mod::Log("PaletteCharSelect: apply side=%d char=%u slot=%u",
                     side, applyRow.charId, applyRow.colorSlot);
        }
        g_lastCustom[side] = true;
    }
    else
    {
        // custom->stock edge: revert. The hook path already ran orig (stock
        // stands); the poll path must rebuild stock itself (guarded).
        if (g_lastCustom[side] && !origAlreadyRan && CharObjValid(csObj, side))
        {
            (void)g_origReload(reinterpret_cast<void*>(csObj),
                               static_cast<char>(side));
            mod::Log("PaletteCharSelect: revert side=%d", side);
        }
        g_lastCustom[side] = false;
    }
}

int __fastcall HookReload(void* gameContext, void* /*edx*/, char playerIndex)
{
    const int result = g_origReload(gameContext, playerIndex);
    if (OverlayChannel::Instance().IsActive())
    {
        std::lock_guard<std::mutex> lock(g_paletteMutex);
        ProcessSideLocked(reinterpret_cast<std::uint32_t>(gameContext),
                          static_cast<int>(playerIndex), true);
    }
    return result;
}

// 60Hz driver: reloadCharacterPalette does not fire while EDIT COLOR is active,
// so this thread runs the exchange + apply during char-select (matches the
// original mod's polling thread).
void PollThreadMain()
{
    mod::Log("PaletteCharSelect: poll thread started");
    while (!g_pollStop.load(std::memory_order_acquire))
    {
        const std::uint32_t csObj = g_csObj.load(std::memory_order_acquire);
        if (csObj != 0 && ReadCurrentScreen() == kScreenCharSelect
            && OverlayChannel::Instance().IsActive())
        {
            std::lock_guard<std::mutex> lock(g_paletteMutex);
            OverlayChannel& ch = OverlayChannel::Instance();
            if (g_useSocket)
            {
                socket_transport::Poll(ch);
            }
            ch.Tick(static_cast<std::uint32_t>(GetTickCount()));

            // ~2 Hz diagnostic: ground-truth for the gate decisions + peer rows.
            static unsigned s_diag = 0;
            if ((++s_diag % 30u) == 1u)
            {
                const CsDiag d = ReadCsDiag(csObj);
                P::PaletteBlobBody pr0{}, pr1{};
                const bool has0 = ch.GetPeerPaletteRow(0, &pr0);
                const bool has1 = ch.GetPeerPaletteRow(1, &pr1);
                mod::Log("PaletteDiag: state=[%u,%u] exit=%u stageGate=%d leaveGate=%d "
                         "localSide=%d peer0(has=%d slot=%u) peer1(has=%d slot=%u)",
                         d.s0, d.s1, d.exitFlag,
                         InStageFlow(csObj) ? 1 : 0,
                         ScreenLeavingCharSelect(csObj) ? 1 : 0, g_localSide,
                         has0 ? 1 : 0, pr0.slotByte, has1 ? 1 : 0, pr1.slotByte);
            }

            ProcessSideLocked(csObj, 0, false);
            ProcessSideLocked(csObj, 1, false);
        }
        Sleep(16);
    }
    mod::Log("PaletteCharSelect: poll thread stopped");
}

int __fastcall HookInit(void* screenContext, void* /*edx*/)
{
    const int result = g_origInit(screenContext);
    std::lock_guard<std::mutex> lock(g_paletteMutex);
    OverlayChannel& ch = OverlayChannel::Instance();
    ch.End();                       // fresh session per char-select entry
    if (g_useSocket)
    {
        ch.SetSendSink(&socket_transport::SendSink, nullptr);
    }
    else
    {
        ch.SetSendSink(&LoopbackSink, nullptr);
    }
    ch.Begin(g_localSide);          // no-op unless the master flag is on
    ch.ResetPaletteExchangeForRematch();
    g_lastCustom[0] = false;
    g_lastCustom[1] = false;
    // Flush the match palette cache now (the previous game's win screen, if any,
    // has already consumed it) so this game starts from a clean stock baseline.
    g_matchRow[0] = P::PaletteBlobBody{};
    g_matchRow[1] = P::PaletteBlobBody{};
    g_csObj.store(reinterpret_cast<std::uint32_t>(screenContext),
                  std::memory_order_release);
    mod::Log("PaletteCharSelect: char-select init, armed active=%d side=%d "
             "transport=%s socketUp=%d",
             ch.IsActive() ? 1 : 0, g_localSide,
             g_useSocket ? "socket" : "loopback",
             socket_transport::IsRunning() ? 1 : 0);
    return result;
}

// Read the winner side (0/1) and gameData from the result-screen context.
// gameData = *(screenContext+28); winner = *(gameData+4940). POD-only so it can
// live in a __try alongside no C++ unwinding. SEH-guarded.
bool ReadWinner(void* screenContext, std::uint32_t* gameDataOut, int* winnerOut)
{
    bool ok = false;
    __try
    {
        const std::uint32_t gd = *reinterpret_cast<const std::uint32_t*>(
            reinterpret_cast<std::uintptr_t>(screenContext) + 28u);
        if (gd != 0)
        {
            const std::uint8_t w =
                *reinterpret_cast<const std::uint8_t*>(gd + kWinnerIndexOffset);
            if (w == 0u || w == 1u)
            {
                *gameDataOut = gd;
                *winnerOut = static_cast<int>(w);
                ok = true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

// Win (result) screen: apply the winner's cached custom row. Runs on the game
// thread AFTER the original built the screen. Cosmetic + outside the rollback
// parity island (the battle is over), so determinism-neutral. Only applies when
// the winner actually locked in a custom color (slotByte!=0) - which is what
// avoids the offline mod's "shows even with no custom color" artifact.
int __fastcall HookResult(void* screenContext, void* /*edx*/)
{
    const int result = g_origResult(screenContext);
    if (g_installed)
    {
        std::uint32_t gameData = 0;
        int winner = -1;
        if (ReadWinner(screenContext, &gameData, &winner))
        {
            P::PaletteBlobBody row{};
            {
                std::lock_guard<std::mutex> lock(g_paletteMutex);
                row = g_matchRow[winner];
            }
            if (row.slotByte != 0)
            {
                (void)apply::ApplyRowToWinScreen(gameData, winner, row);
                mod::Log("PaletteCharSelect: winscreen apply winner=%d char=%u "
                         "slot=%u", winner, row.charId, row.colorSlot);
            }
            else
            {
                mod::Log("PaletteCharSelect: winscreen winner=%d stock "
                         "(no custom cached)", winner);
            }
        }
    }
    return result;
}
} // namespace

bool Install()
{
    if (g_installed)
    {
        return true;
    }
    if (!netplay::mod_settings::IsModInteropChannelEnabled())
    {
        return false;   // master flag off: do not touch the game
    }
    // Pick a transport: loopback (Stage-1 solo) OR the side-socket (Stage-2,
    // when a peer IP is configured). With neither there is no sink and the
    // channel could never confirm, so skip the install entirely.
    const bool loopback = netplay::mod_settings::IsModInteropLoopbackEnabled();
    const std::string& peer = netplay::mod_settings::ModInteropPeer();
    if (loopback)
    {
        g_useSocket = false;
        g_localSide = 0;
    }
    else if (!peer.empty())
    {
        g_localSide = netplay::mod_settings::ModInteropSide();
        if (!socket_transport::Start(peer.c_str(),
                                     netplay::mod_settings::ModInteropPort()))
        {
            mod::Log("PaletteCharSelect: socket transport failed to start "
                     "(peer=%s) - install skipped", peer.c_str());
            return false;
        }
        g_useSocket = true;
    }
    else
    {
        mod::Log("PaletteCharSelect: interop on but no transport "
                 "(loopback off, no ModInteropPeer) - install skipped");
        return false;
    }

    if (!g_minhookReady)
    {
        const MH_STATUS s = MH_Initialize();
        if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED)
        {
            mod::Log("PaletteCharSelect: MH_Initialize failed status=%d",
                     static_cast<int>(s));
            return false;
        }
        g_minhookReady = true;
    }

    const std::uintptr_t base =
        reinterpret_cast<std::uintptr_t>(GetModuleHandleA(nullptr));
    g_reloadTarget = reinterpret_cast<void*>(base + kRvaReloadCharacterPalette);
    g_initTarget = reinterpret_cast<void*>(base + kRvaInitCharacterSelect);
    g_resultTarget = reinterpret_cast<void*>(base + kRvaInitResultScreen);

    if (MH_CreateHook(g_reloadTarget, reinterpret_cast<void*>(&HookReload),
                      reinterpret_cast<void**>(&g_origReload)) != MH_OK
        || MH_CreateHook(g_initTarget, reinterpret_cast<void*>(&HookInit),
                         reinterpret_cast<void**>(&g_origInit)) != MH_OK
        || MH_CreateHook(g_resultTarget, reinterpret_cast<void*>(&HookResult),
                         reinterpret_cast<void**>(&g_origResult)) != MH_OK)
    {
        mod::Log("PaletteCharSelect: MH_CreateHook failed");
        return false;
    }
    if (MH_EnableHook(g_reloadTarget) != MH_OK
        || MH_EnableHook(g_initTarget) != MH_OK
        || MH_EnableHook(g_resultTarget) != MH_OK)
    {
        mod::Log("PaletteCharSelect: MH_EnableHook failed");
        return false;
    }

    // Start the 60Hz poll driver (idle until g_csObj is set + we are in
    // char-select).
    g_pollStop.store(false, std::memory_order_release);
    g_pollThread = std::thread(&PollThreadMain);
    g_pollRunning.store(true, std::memory_order_release);

    g_installed = true;
    mod::Log("PaletteCharSelect: hooks installed transport=%s side=%d "
             "reload=%p init=%p result=%p", g_useSocket ? "socket" : "loopback",
             g_localSide, g_reloadTarget, g_initTarget, g_resultTarget);
    return true;
}

void Uninstall()
{
    // Stop the poll thread first so it cannot touch the channel/socket mid-teardown.
    if (g_pollRunning.load(std::memory_order_acquire))
    {
        g_pollStop.store(true, std::memory_order_release);
        if (g_pollThread.joinable())
        {
            g_pollThread.join();
        }
        g_pollRunning.store(false, std::memory_order_release);
    }
    OverlayChannel::Instance().End();
    socket_transport::Stop();
    if (!g_installed)
    {
        return;
    }
    if (g_reloadTarget != nullptr) MH_DisableHook(g_reloadTarget);
    if (g_initTarget != nullptr) MH_DisableHook(g_initTarget);
    if (g_resultTarget != nullptr) MH_DisableHook(g_resultTarget);
    g_installed = false;
}
} // namespace netplay::interop::charselect
