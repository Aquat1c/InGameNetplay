#include "netplay/bridge/netplay_state_snapshot.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

using netplay::bridge::state_export::StateSnapshot;

int main()
{
    int failures = 0;
    const auto expect = [&](bool condition, const char* message) {
        if (!condition) { std::fprintf(stderr, "%s\n", message); ++failures; }
    };
    StateSnapshot publication;
    EFZNetplayState output;
    std::memset(&output, 0xA5, sizeof(output));
    const EFZNetplayState untouched = output;
    expect(!publication.Copy(&output, sizeof(output), 4), "uninitialized copy rejected");
    expect(std::memcmp(&output, &untouched, sizeof(output)) == 0, "failure leaves caller buffer intact");

    EFZNetplayState offline{};
    offline.magic = EFZ_NETPLAY_STATE_MAGIC;
    offline.version = EFZ_NETPLAY_STATE_VERSION;
    offline.structSize = sizeof(offline);
    offline.capabilityFlags = EFZ_CAP_SESSION | EFZ_CAP_ACTIVITY;
    offline.sessionMode = EFZ_SESSION_HOSTING;
    offline.sessionPhase = EFZ_PHASE_CONNECTING;
    offline.activityPhase = EFZ_ACTIVITY_HOST_IDLE;
    offline.stateSeq = 1;
    EFZNetplayState online = offline;
    online.sessionPhase = EFZ_PHASE_CONNECTED;
    online.activityPhase = EFZ_ACTIVITY_MATCH;
    online.inNetplayMatch = 1;
    online.stateSeq = 2;
    publication.Publish(offline);
    expect(!publication.Copy(nullptr, sizeof(output), 4), "null buffer rejected");
    expect(!publication.Copy(&output, 11, 4), "short header rejected");
    expect(!publication.Copy(&output, sizeof(output), 8), "unsupported minimum version rejected");
    expect(std::memcmp(&output, &untouched, sizeof(output)) == 0, "invalid requests leave caller buffer intact");
    expect(publication.Copy(&output, 12, 4), "header-sized buffer accepted");
    expect(output.magic == EFZ_NETPLAY_STATE_MAGIC && output.version == 7 && output.structSize == sizeof(offline), "header copied");
    expect(std::memcmp(reinterpret_cast<const char*>(&output) + 12,
                       reinterpret_cast<const char*>(&untouched) + 12,
                       sizeof(output) - 12) == 0, "copy honors caller capacity");
    struct Extended { EFZNetplayState state; unsigned char tail[16]; } extended;
    std::memset(&extended, 0xA5, sizeof(extended));
    expect(publication.Copy(&extended.state, sizeof(extended), 4), "larger caller accepted");
    for (unsigned char byte : extended.tail) expect(byte == 0xA5, "copy honors producer size");

    netplay::bridge::NetbridgeStatus handoff;
    handoff.phase = static_cast<int>(netplay::bridge::NetbridgePhase::Connecting);
    handoff.role = static_cast<int>(netplay::bridge::NetbridgeRole::Join);
    publication.PublishHandoff(handoff, 1234, 9);
    expect(publication.Copy(&output, sizeof(output), 4), "handoff snapshot copy succeeds");
    expect(output.sessionPhase == EFZ_PHASE_CONNECTING && output.sessionMode == EFZ_SESSION_JOINING
        && output.activityPhase == EFZ_ACTIVITY_CONNECTING && !output.inNetplayMatch
        && !output.inNetplayCharacterSelect && !output.inNetplayMenu,
        "online handoff clears stale background-host and game-flow state");
    expect(output.stateSeq == 9 && output.lastUpdateTick == 1234, "handoff updates publication sequence and tick");
    handoff.phase = static_cast<int>(netplay::bridge::NetbridgePhase::Failed);
    publication.PublishHandoff(handoff, 1235, 10);
    expect(publication.Copy(&output, sizeof(output), 4)
        && output.sessionMode == EFZ_SESSION_NONE && output.sessionPhase == EFZ_PHASE_FAILED
        && output.activityPhase == EFZ_ACTIVITY_IDLE,
        "failed handoff publishes a terminal offline snapshot");
    publication.Publish(offline);

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 200000; ++i) publication.Publish((i & 1) ? offline : online);
        done.store(true, std::memory_order_release);
    });
    start.store(true, std::memory_order_release);
    unsigned reads = 0;
    do {
        expect(publication.Copy(&output, sizeof(output), 4), "concurrent copy succeeds");
        if (std::memcmp(&output, &offline, sizeof(output)) != 0
            && std::memcmp(&output, &online, sizeof(output)) != 0) {
            expect(false, "concurrent copy must contain exactly one complete publication");
            break;
        }
        ++reads;
    } while (!done.load(std::memory_order_acquire));
    writer.join();
    expect(reads != 0, "concurrent reader ran");
    publication.Publish({});
    expect(!publication.Copy(&output, sizeof(output), 4), "shutdown invalidation rejected");
    return failures == 0 ? 0 : 1;
}
