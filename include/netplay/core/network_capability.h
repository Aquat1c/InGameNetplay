#pragma once

#include "netplay/core/network_endpoint.h"

#include <cstdint>

namespace netplay::network
{
// Strongest local address class observed on a family; diagnostic only, never advertised as a public Host address.
enum class LocalAddressScope : uint8_t
{
    None = 0,
    Loopback,
    LinkLocal,
    PrivateOrUniqueLocal,
    GlobalUnicast,
};

// Unknown is conservative: callers must not reject a family on an inconclusive probe.
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

// Caller-thread only; call from a background worker. Binds no game port, writes no INI values, never discovers a public address.
LocalNetworkCapabilitySnapshot ScanLocalNetworkCapabilities();

const LocalFamilyCapability& GetFamilyCapability(
    const LocalNetworkCapabilitySnapshot& snapshot,
    NetworkFamily family);

// LocalOnly and HardUnavailable skip public discovery; Unknown stays eligible.
bool CanAttemptGlobalPublicDiscovery(
    const LocalFamilyCapability& capability);

const char* LocalAddressScopeName(LocalAddressScope scope);
const char* LocalFamilyCapabilityStateName(
    LocalFamilyCapabilityState state);
} // namespace netplay::network
