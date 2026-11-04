#include "netplay/bridge/delay_selection.h"
#include <cstdio>
#include <climits>

int main() {
    using namespace netplay::bridge;
    int failures = 0;
    const auto expect = [&](bool ok, const char* message) {
        if (!ok) { std::fprintf(stderr, "%s\n", message); ++failures; }
    };
    DelayPromptMetrics metrics{};
    metrics.serial = 1;
    expect(!delay_selection::HasRange(metrics, 1), "missing native limits cannot become a selectable 0..20 range");
    metrics.minDelay = 4;
    metrics.maxDelay = 20;
    metrics.recommendedDelay = 6;
    expect(delay_selection::HasRange(metrics, 1), "complete current prompt has a range");
    expect(!delay_selection::HasRange(metrics, 2), "old prompt limits cannot validate the next prompt");
    expect(!delay_selection::Accepts(metrics, 1, 3), "below native minimum is rejected before submission");
    expect(delay_selection::Accepts(metrics, 1, 4), "native minimum is inclusive");
    expect(delay_selection::Accepts(metrics, 1, 20), "native maximum is inclusive");
    expect(!delay_selection::Accepts(metrics, 1, 21), "above native maximum is rejected before submission");
    expect(delay_selection::Clamp(metrics, INT_MIN) == 4, "selection clamps to native minimum");
    expect(delay_selection::Clamp(metrics, INT_MAX) == 20, "selection clamps to native maximum");
    expect(delay_selection::Clamp(metrics, 9) == 9, "valid user selection is preserved");
    expect(!delay_selection::IsRetry(1, 1, 1), "accepted prompt stays in sync wait");
    expect(!delay_selection::IsRetry(1, 0, 1), "undelivered input is not a retry");
    expect(delay_selection::IsRetry(2, 1, 1), "new unserved native prompt reopens delay selection");
    expect(!delay_selection::IsRetry(2, 2, 1), "already served next prompt is not a retry");
    expect(delay_selection::ShouldNotifyPeerOnCancel(false, true, 1), "delay cancel notifies peer before local online initialization");
    expect(delay_selection::ShouldNotifyPeerOnCancel(true, true, 1), "online cancel still notifies peer");
    expect(!delay_selection::ShouldNotifyPeerOnCancel(false, true, 0), "idle listener has no delay peer to notify");
    expect(!delay_selection::ShouldNotifyPeerOnCancel(false, false, 1), "non-player role does not broadcast player quit");
    metrics.minDelay = 21;
    expect(!delay_selection::HasRange(metrics, 1), "inverted native limits are unavailable");
    metrics.minDelay = -1;
    expect(!delay_selection::HasRange(metrics, 1), "unknown minimum is unavailable");
    return failures ? 1 : 0;
}
