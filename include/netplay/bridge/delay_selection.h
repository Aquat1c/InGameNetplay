#pragma once
#include "netplay/bridge/session_bridge.h"
#include <algorithm>

namespace netplay::bridge::delay_selection {
inline bool HasRange(const DelayPromptMetrics& metrics, int promptSerial) {
    return promptSerial > 0 && metrics.serial == promptSerial
        && metrics.minDelay >= 0 && metrics.minDelay <= metrics.maxDelay
        && metrics.maxDelay <= 20;
}
inline bool Accepts(const DelayPromptMetrics& metrics, int promptSerial, int value) {
    return HasRange(metrics, promptSerial)
        && value >= metrics.minDelay && value <= metrics.maxDelay;
}
inline int Clamp(const DelayPromptMetrics& metrics, int value) {
    return (std::max)(metrics.minDelay, (std::min)(metrics.maxDelay, value));
}
inline bool ShouldNotifyPeerOnCancel(bool online, bool intendedOnline, int promptSerial) {
    return online || (intendedOnline && promptSerial > 0);
}
inline bool IsRetry(int promptSerial, int servedSerial, int submittedSerial) {
    return submittedSerial > 0 && promptSerial > submittedSerial && servedSerial < promptSerial;
}
}
