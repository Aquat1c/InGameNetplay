#pragma once

#include "netplay/core/network_endpoint.h"

#include <cstdint>

namespace netplay::network
{
// The strongest local address class observed on a family. This is a local
// diagnostic only: it is never advertised as a public Host address.
enum class LocalAddressScope : uint8_t
{
    None = 0,
    Loopback,
    LinkLocal,
    PrivateOrUniqueLocal,
    GlobalUnicast,
};

// Unknown is conservative: callers must not reject a family merely because a
// temporary local probe was inconclusive.
enum class LocalFamilyCapabilityState : uint8_t
{
    Unknown = 0,
    HardUnavailable,
    LocalOnly,
    Routable,
};

struct LocalFamilyCapability
{
    NetworkFamily family = NetworkFamily::IPv4;
    LocalFamilyCapabilityState state =
        LocalFamilyCapabilityState::Unknown;
    NetworkFamilyProbeResult listenerProbe = {};

    bool addressListQueried = false;
    int addressListNativeError = 0;
    uint32_t bindableAddressCount = 0;
    LocalAddressScope strongestAddressScope = LocalAddressScope::None;

    bool routeProbeAttempted = false;
    bool routeAvailable = false;
    bool routeDefinitelyUnavailable = false;
    int routeNativeError = 0;
    LocalAddressScope routeSourceScope = LocalAddressScope::None;
};

struct LocalNetworkCapabilitySnapshot
{
    // GetTickCount value recorded by the worker when the immutable scan ends.
    uint32_t completedTick = 0;
    LocalFamilyCapability ipv4 = {};
    LocalFamilyCapability ipv6 = {
        NetworkFamily::IPv6,
    };
};

// Runs entirely on its caller's thread. Call it only from a background worker:
// it opens no Revival session, binds no requested game port, writes no INI
// values, and never discovers or returns a public address.
LocalNetworkCapabilitySnapshot ScanLocalNetworkCapabilities();

const LocalFamilyCapability& GetFamilyCapability(
    const LocalNetworkCapabilitySnapshot& snapshot,
    NetworkFamily family);

// LocalOnly and HardUnavailable are skipped for external public-address
// discovery. Unknown remains eligible so a transient probe cannot falsely
// disable a usable family.
bool CanAttemptGlobalPublicDiscovery(
    const LocalFamilyCapability& capability);

const char* LocalAddressScopeName(LocalAddressScope scope);
const char* LocalFamilyCapabilityStateName(
    LocalFamilyCapabilityState state);
} // namespace netplay::network
