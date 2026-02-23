#pragma once

#include <cstddef>
#include <cstdint>

namespace netplay::bridge
{
enum class NetbridgeRole : int
{
    Host = 0,
    Join = 1,
    Spectate = 2,
};

enum class NetbridgePhase : int
{
    Idle = 0,
    Connecting = 1,
    Connected = 2,
    Failed = 3,
    SessionEnded = 4,
};

struct NetbridgeStatus
{
    int phase = static_cast<int>(NetbridgePhase::Idle);
    int role = -1;
    int roleFlag = -1;
    int pingMs = -1;
    int rollbackFrames = -1;
    uint16_t port = 0;
    char address[64] = {};
    char nickname[32] = {};
    char p1Name[64] = {};
    char p2Name[64] = {};
    char errorMsg[128] = {};
    uint32_t phaseTick = 0;
    uint32_t processId = 0;
};

bool IsCurrentProcessRevival();
void Initialize();
void Shutdown();
void InitializeInjectedProcess();
void ShutdownInjectedProcess();
void Tick();
bool StartSession(NetbridgeRole role, uint16_t port, const char* address, const char* nickname);
void CancelSession(const char* reason);
void OnTitleSelectionConfirmed(int selection);
NetbridgeStatus GetStatus();
const char* PhaseToString(NetbridgePhase phase);
void BuildStatusLine(const NetbridgeStatus& status, char* buffer, size_t bufferSize);
}
