#include "netplay/interop/palette_charselect.h"

#include <windows.h>

#include <MinHook.h>

#include <cstring>

#include "logger.h"
#include "netplay/bridge/session_bridge.h"
#include "netplay/core/mod_settings.h"
#include "netplay/interop/overlay_channel.h"
#include "netplay/interop/overlay_ipc.h"
#include "netplay/interop/overlay_protocol.h"
#include "netplay/interop/palette_apply.h"
#include "netplay/interop/palette_socket.h"
#include "netplay/interop/palette_source.h"

namespace netplay::interop::charselect
{
namespace
{
namespace P = netplay::interop::protocol;

// efz.exe RVAs (version-invariant).
constexpr std::uintptr_t kRvaReloadCharacterPalette = 0x003595D0u;
constexpr std::uintptr_t kRvaInitCharacterSelect = 0x003598D0u;
constexpr std::uintptr_t kRvaInitResultScreen = 0x00374B00u;   // win screen
constexpr std::uintptr_t kVaCurrentScreenIndex = 0x00790148u;
constexpr int kScreenCharSelect = 1;
constexpr int kScreenBattle = 3;

// Result-screen field offsets (gameData = *(resultScreenCtx + 28)).
constexpr std::uint32_t kWinnerIndexOffset = 4940u;   // 0 = P1 won, 1 = P2 won

// Char-select object field offsets.
constexpr std::uint32_t kCharIdBase = 1340u;   // + side  (P1 1340, P2 1341)
constexpr std::uint32_t kColorBase = 1342u;    // + side  (P1 1342, P2 1343)

// 0 = P1, 1 = P2, -1 = spectator (controls neither); set together with g_sideSource.
int g_localSide = 0;

enum class SideSource
{
    OnlinePlayer,   // one side is ours; the other fills from the peer's row
    Spectator,      // NEITHER side is ours - both fill from host/client, else stock
    Offline,        // both players sit at this machine; both sides are local
};
SideSource g_sideSource = SideSource::OnlinePlayer;

const char* SideSourceName(SideSource s)
{
    switch (s)
    {
    case SideSource::Spectator: return "spectator";
    case SideSource::Offline:   return "offline";
    default:                    return "online";
    }
}

enum class Transport { Loopback, Socket, Piggyback };
Transport g_transport = Transport::Loopback;

const char* TransportName(Transport t)
{
    switch (t)
    {
    case Transport::Socket: return "socket";
    case Transport::Piggyback: return "piggyback";
    default: return "loopback";
    }
}

void DeriveSideSource()
{
    // Mirrors takeover_internal.h kLocalRoleSpectate.
    constexpr int kRoleFlagSpectate = 1;

    const netplay::bridge::NetbridgeStatus st = netplay::bridge::GetStatus();
    const auto role = static_cast<netplay::bridge::NetbridgeRole>(st.role);

    if (role == netplay::bridge::NetbridgeRole::Spectate
        || role == netplay::bridge::NetbridgeRole::JoinSpectate
        || st.roleFlag == kRoleFlagSpectate)
    {
        g_sideSource = SideSource::Spectator;
        g_localSide = -1;
        return;
    }
    if (role == netplay::bridge::NetbridgeRole::Host)
    {
        g_sideSource = SideSource::OnlinePlayer;
        g_localSide = 0;
        return;
    }
    if (role == netplay::bridge::NetbridgeRole::Join)
    {
        g_sideSource = SideSource::OnlinePlayer;
        g_localSide = 1;
        return;
    }
    g_sideSource = SideSource::Offline;
    g_localSide = (st.activePlayer == 0 || st.activePlayer == 1)
        ? st.activePlayer
        : 0;
}

bool SideIsLocal(int side)
{
    switch (g_sideSource)
    {
    case SideSource::Spectator: return false;
    case SideSource::Offline:   return true;
    default:                    return side == g_localSide;
    }
}

// g_csObj is valid only from HookInit until the screen leaves char-select; the result screen frees the character objects it points at.
std::uint32_t g_csObj = 0;
bool g_lastCustom[2] = {false, false};

std::uint32_t g_stock[2][apply::kPortraitEntries] = {};
bool g_stockValid[2] = {false, false};

constexpr DWORD kLocalRowRefreshMs = 250;
struct LocalRowCache
{
    bool valid;
    std::uint8_t charId;
    std::uint8_t colorSlot;
    DWORD loadedTick;
    P::PaletteBlobBody row;
};
LocalRowCache g_localRowCache[2] = {};

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

bool LoopbackSink(const std::uint8_t* datagram, std::size_t len, void*)
{
    std::uint8_t copy[P::kMaxFrameBytes];
    if (datagram == nullptr || len == 0 || len > sizeof(copy))
    {
        return true;
    }
    std::memcpy(copy, datagram, len);

    OverlayChannel::Instance().OnInboundDatagram(copy, len);
    return true;
}

bool PiggybackSink(const std::uint8_t* datagram, std::size_t len, void*)
{
    ipc::OverlayIpcBlock* b = ipc::Block();
    if (b == nullptr || datagram == nullptr || len == 0
        || len > ipc::kSlotBytes)
    {
        return false;
    }
    return ipc::Push(b->toHelper, datagram, static_cast<std::uint32_t>(len));
}

void PiggybackPoll(OverlayChannel& ch)
{
    ipc::OverlayIpcBlock* b = ipc::Block();
    if (b == nullptr) return;
    const bool hostHub = (g_sideSource == SideSource::OnlinePlayer
                          && g_localSide == 0);
    std::uint8_t frame[P::kMaxFrameBytes];
    for (int i = 0; i < 32; ++i)
    {
        const std::uint32_t n = ipc::Pop(b->toGame, frame, sizeof(frame));
        if (n == 0u) break;
        ch.OnInboundDatagram(frame, static_cast<std::size_t>(n));

        if (hostHub)
        {
            P::Kind k = P::Kind::Invalid;
            P::PaletteBlobBody body{};
            if (P::ParseKind(frame, static_cast<std::size_t>(n), &k)
                && k == P::Kind::PaletteBlob
                && P::ParsePaletteBlob(frame, static_cast<std::size_t>(n), &body)
                && body.side != 0)
            {
                (void)ipc::Push(b->toHelper, frame, n);
                static unsigned s_relay = 0;
                if (++s_relay <= 3u || (s_relay % 60u) == 0u)
                {
                    mod::Log("PaletteCharSelect: host relay client row -> peers "
                             "side=%u slot=%u seq=%u", body.side, body.slotByte,
                             body.seq);
                }
            }
        }
    }
}

void DiscardInboundFrames()
{
    ipc::OverlayIpcBlock* b = ipc::Block();
    if (b == nullptr) return;
    std::uint8_t frame[P::kMaxFrameBytes];
    for (std::uint32_t i = 0; i < ipc::kRingSlots; ++i)
    {
        (void)ipc::Pop(b->toGame, frame, sizeof(frame));
    }
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

// Must read before the original reload (it consumes/clears the flag); flag = *(csObj+28) + 4920 + 4*side.
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

int ReadCurrentScreen()
{
    int screen = -1;
    __try { screen = *reinterpret_cast<const int*>(kVaCurrentScreenIndex); }
    __except (EXCEPTION_EXECUTE_HANDLER) { screen = -1; }
    return screen;
}

// Player-state machine (csObj+1182+side): portrait phases {0,1,2,3,5,6}; stage
// states {4,7,8,9,10} load the stage palette into 175-254, so portrait pushes
// must stop once either side is stage (4 is CPU-player-only in offline modes).
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

// csObj+45 goes non-zero as char-select starts fading out; re-pushing during the fade pins 175-254 at full brightness.
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

P::PaletteBlobBody LoadLocalRowCached(int side, std::uint8_t charId,
                                      std::uint8_t colorSlot)
{
    LocalRowCache& c = g_localRowCache[side];
    const DWORD now = GetTickCount();
    if (!c.valid || c.charId != charId || c.colorSlot != colorSlot
        || now - c.loadedTick >= kLocalRowRefreshMs)
    {
        c.row = P::PaletteBlobBody{};
        (void)source::LoadLocalRow(std::string(), static_cast<std::uint8_t>(side),
                                   charId, colorSlot, &c.row);
        c.valid = true;
        c.charId = charId;
        c.colorSlot = colorSlot;
        c.loadedTick = now;
    }
    return c.row;
}

// origAlreadyRan: true when the reload hook already rebuilt stock; false from the per-frame driver (restores captured stock).
void ProcessSide(std::uint32_t csObj, int side, bool origAlreadyRan)
{
    OverlayChannel& ch = OverlayChannel::Instance();
    if (!ch.IsActive() || csObj == 0 || (side != 0 && side != 1))
    {
        return;
    }
    if (InStageFlow(csObj) || ScreenLeavingCharSelect(csObj))
    {
        g_lastCustom[0] = false;
        g_lastCustom[1] = false;
        return;
    }
    P::PaletteBlobBody applyRow{};
    bool custom = false;
    if (SideIsLocal(side))
    {
        const bool editColor = ReadEditColorFlag(csObj, side);
        std::uint8_t charId = 0xFF, colorSlot = 0xFF;
        if (ReadCharAndColor(csObj, side, &charId, &colorSlot)
            && source::CharFolderName(charId) != nullptr)
        {
            const P::PaletteBlobBody local = editColor
                ? LoadLocalRowCached(side, charId, colorSlot)
                : source::AssembleRow(static_cast<std::uint8_t>(side),
                                      charId, colorSlot, false, nullptr);
            if (side == g_localSide)
            {
                ch.SubmitLocalPaletteRow(local);
            }
            applyRow = local;
            custom = (local.slotByte != 0);
        }
    }
    else
    {
        custom = ch.GetPeerPaletteRow(side, &applyRow) && applyRow.slotByte != 0;
    }

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
        if (g_lastCustom[side] && !origAlreadyRan)
        {
            const bool reverted = g_stockValid[side]
                && apply::RestorePortraitEntries(csObj, side, g_stock[side]);
            mod::Log("PaletteCharSelect: revert side=%d%s", side,
                     reverted ? "" : " skipped (no stock capture)");
        }
        g_lastCustom[side] = false;
    }
}

int __fastcall HookReload(void* gameContext, void* /*edx*/, char playerIndex)
{
    const int result = g_origReload(gameContext, playerIndex);
    const int side = static_cast<int>(playerIndex);
    if (g_csObj != 0 && (side == 0 || side == 1))
    {
        const std::uint32_t csObj = reinterpret_cast<std::uint32_t>(gameContext);
        g_stockValid[side] =
            apply::CapturePortraitEntries(csObj, side, g_stock[side]);
        if (OverlayChannel::Instance().IsActive())
        {
            ProcessSide(csObj, side, true);
        }
    }
    return result;
}

int __fastcall HookInit(void* screenContext, void* /*edx*/)
{
    g_csObj = 0;
    const int result = g_origInit(screenContext);
    OverlayChannel& ch = OverlayChannel::Instance();
    ch.End();                       // fresh session per char-select entry
    g_sideSource = SideSource::OnlinePlayer;
    switch (g_transport)
    {
    case Transport::Socket:
        ch.SetSendSink(&socket_transport::SendSink, nullptr);
        break;
    case Transport::Piggyback:
        ch.SetSendSink(&PiggybackSink, nullptr);
        DiscardInboundFrames();
        DeriveSideSource();
        break;
    default:
        ch.SetSendSink(&LoopbackSink, nullptr);
        break;
    }
    ch.Begin(g_localSide);          // no-op unless the master flag is on
    ch.ResetPaletteExchangeForRematch();
    g_lastCustom[0] = false;
    g_lastCustom[1] = false;
    g_stockValid[0] = false;        // captures belong to the previous characters
    g_stockValid[1] = false;
    g_localRowCache[0].valid = false;
    g_localRowCache[1].valid = false;
    g_matchRow[0] = P::PaletteBlobBody{};
    g_matchRow[1] = P::PaletteBlobBody{};
    g_csObj = reinterpret_cast<std::uint32_t>(screenContext);
    mod::Log("PaletteCharSelect: char-select init, armed active=%d side=%d "
             "src=%s transport=%s ipcAttached=%d",
             ch.IsActive() ? 1 : 0, g_localSide,
             SideSourceName(g_sideSource),
             TransportName(g_transport), ipc::IsAttached() ? 1 : 0);
    return result;
}

// gameData = *(screenContext+28); winner = *(gameData+4940) (0 = P1, 1 = P2).
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

int __fastcall HookResult(void* screenContext, void* /*edx*/)
{
    const int result = g_origResult(screenContext);
    if (g_installed)
    {
        std::uint32_t gameData = 0;
        int winner = -1;
        if (ReadWinner(screenContext, &gameData, &winner))
        {
            const P::PaletteBlobBody row = g_matchRow[winner];
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
    if (!netplay::mod_settings::AreOnlineCustomColorsEnabled())
    {
        return false;   // master flag off: do not touch the game
    }
    if (netplay::mod_settings::IsModInteropLoopbackEnabled())
    {
        g_transport = Transport::Loopback;
        g_localSide = 0;
    }
    else
    {
        g_transport = Transport::Piggyback;
        g_localSide = 0;   // provisional; DeriveSideSource() runs at char-select
        if (!ipc::Attach(/*asHelper=*/false))
        {
            mod::Log("PaletteCharSelect: overlay IPC attach failed - "
                     "install skipped");
            return false;
        }
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

    g_installed = true;
    mod::Log("PaletteCharSelect: hooks installed transport=%s side=%d "
             "reload=%p init=%p result=%p", TransportName(g_transport),
             g_localSide, g_reloadTarget, g_initTarget, g_resultTarget);
    return true;
}

void TickGameThread()
{
    if (!g_installed)
    {
        return;
    }
    const int screen = ReadCurrentScreen();
    if (screen == kScreenBattle)
    {
        return;   // never any work during a battle (rollback parity island)
    }

    if (g_transport == Transport::Piggyback)
    {
        static unsigned s_hdiag = 0;
        if ((++s_hdiag % 125u) == 1u)
        {
            if (ipc::OverlayIpcBlock* b = ipc::Block())
            {
                mod::Log("OverlayHelperDiag: installed=%u reason=%u iatMask=0x%X "
                         "sendToSeen=%u recvCompletions=%u rxObserved=%u "
                         "txFlushed=%u peerValid=%u",
                         b->helperInstalled, b->helperInstallReason,
                         b->helperIatMask, b->sendToSeen, b->recvCompletions,
                         b->rxObserved, b->txFlushed, b->socketValid);
            }
        }
    }

    const std::uint32_t csObj = g_csObj;
    if (csObj == 0)
    {
        return;
    }
    if (screen != kScreenCharSelect)
    {
        g_csObj = 0;
        g_lastCustom[0] = false;
        g_lastCustom[1] = false;
        return;
    }
    OverlayChannel& ch = OverlayChannel::Instance();
    if (!ch.IsActive())
    {
        return;
    }
    if (g_transport == Transport::Socket)
    {
        socket_transport::Poll(ch);
    }
    else if (g_transport == Transport::Piggyback)
    {
        PiggybackPoll(ch);
    }
    ch.Tick(static_cast<std::uint32_t>(GetTickCount()));

    static unsigned s_diag = 0;
    if ((++s_diag % 30u) == 1u)
    {
        const CsDiag d = ReadCsDiag(csObj);
        P::PaletteBlobBody pr0{}, pr1{};
        const bool has0 = ch.GetPeerPaletteRow(0, &pr0);
        const bool has1 = ch.GetPeerPaletteRow(1, &pr1);
        mod::Log("PaletteDiag: state=[%u,%u] exit=%u stageGate=%d leaveGate=%d "
                 "localSide=%d src=%s peer0(has=%d slot=%u) peer1(has=%d slot=%u)",
                 d.s0, d.s1, d.exitFlag,
                 InStageFlow(csObj) ? 1 : 0,
                 ScreenLeavingCharSelect(csObj) ? 1 : 0, g_localSide,
                 SideSourceName(g_sideSource),
                 has0 ? 1 : 0, pr0.slotByte, has1 ? 1 : 0, pr1.slotByte);
    }

    ProcessSide(csObj, 0, false);
    ProcessSide(csObj, 1, false);
}
} // namespace netplay::interop::charselect
