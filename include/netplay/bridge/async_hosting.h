#pragma once

#include "netplay/bridge/session_bridge.h"

#include <cstdint>

namespace netplay::bridge::async_host
{
enum class State : uint8_t
{
    Idle = 0,
    Hosting = 1,
    PeerFoundHeld = 2,
    Accepted = 3,
    TimedOut = 4,
};

void OnHostStarted(uint16_t port, const char* nickname);
void OnHostStarted(
    uint16_t port,
    const char* nickname,
    const HostSessionNetworkConfig& networkConfig);

void Tick();

bool IsActive();

bool IsMinimized();
void SetMinimized(bool minimized);

bool IsPeerFoundHeld();

bool IsTimedOut();

bool IsHostListenerReady();
bool HasHostListenerStartupFailed();
bool NotifyHostListenerReady(
    const HostListenerObservation& observation);

const char* ReturnKeyDisplay();

bool OnNetplayMenuRestored();

bool ConsumeReturnKeyArrival();

bool ShouldSuppressDelayOverlay();

void RequestAccept();

void RequestCancel(const char* reason);

void Reset();

State GetState();

uint16_t HostPort();
const char* HostNickname();
bool GetHostNetworkConfig(HostSessionNetworkConfig* outConfig);
}
