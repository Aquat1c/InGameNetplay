#pragma once

#include "netplay/core/network_endpoint.h"

#include <cstdint>

namespace netplay::network
{
// Diagnostic only, never advertised as a public Host address.
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
    uint32_t completedTick = 0;
    LocalFamilyCapability ipv4 = {};
    LocalFamilyCapability ipv6 = {
        NetworkFamily::IPv6,
    };
};

// Background worker only.
LocalNetworkCapabilitySnapshot ScanLocalNetworkCapabilities();

const LocalFamilyCapability& GetFamilyCapability(
    const LocalNetworkCapabilitySnapshot& snapshot,
    NetworkFamily family);

bool CanAttemptGlobalPublicDiscovery(
    const LocalFamilyCapability& capability);

const char* LocalAddressScopeName(LocalAddressScope scope);
const char* LocalFamilyCapabilityStateName(
    LocalFamilyCapabilityState state);
}
