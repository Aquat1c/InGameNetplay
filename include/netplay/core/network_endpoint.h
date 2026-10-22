#pragma once

#include <cstdint>
#include <string>

namespace netplay::network
{
enum class NetworkFamily : uint8_t
{
    IPv4 = 4,
    IPv6 = 6,
};

struct NetworkHost
{
    NetworkFamily family = NetworkFamily::IPv4;
    std::string host;
};

struct NetworkEndpoint
{
    NetworkFamily family = NetworkFamily::IPv4;
    std::string host;
    uint16_t port = 0;
};

struct RemoteHostInput
{
    bool numeric = false;
    NetworkFamily family = NetworkFamily::IPv4;
    std::string host;
};

enum class NetworkProbeStage : uint8_t
{
    None = 0,
    WinsockStartup,
    SocketCreate,
    BindWildcard,
    NumericAddress,
    UdpRoute,
};

struct NetworkFamilyProbeResult
{
    bool unavailable = false;
    int nativeError = 0;
    NetworkProbeStage stage = NetworkProbeStage::None;
};

enum class RemoteEndpointResolveFailure : uint8_t
{
    None = 0,
    InvalidInput,
    WinsockStartup,
    NameLookup,
    NoUsableFamily,
};

struct RemoteEndpointResolution
{
    RemoteEndpointResolveFailure failure =
        RemoteEndpointResolveFailure::None;
    bool inputWasHostname = false;
    bool ipv4CandidateSeen = false;
    bool ipv6CandidateSeen = false;
    bool ipv4Unavailable = false;
    bool ipv6Unavailable = false;
    int nativeError = 0;
    NetworkFamilyProbeResult selectedProbe = {};
};

bool ParseBareHost(const std::string& text, NetworkHost* outHost);

bool IsGloballyRoutableHost(const NetworkHost& host);

bool ParseRemoteHostInput(
    const std::string& text,
    RemoteHostInput* outInput);

bool ParseEndpoint(const std::string& text, NetworkEndpoint* outEndpoint);

bool FormatEndpoint(const NetworkEndpoint& endpoint, std::string* outText);

bool ResolveRemoteEndpoint(
    const std::string& hostInput,
    uint16_t port,
    NetworkEndpoint* outEndpoint,
    RemoteEndpointResolution* outResolution = nullptr);

bool TryParseFamilyName(const std::string& text, NetworkFamily* outFamily);

NetworkFamilyProbeResult ProbeLocalHostFamily(NetworkFamily family);
NetworkFamilyProbeResult ProbeRemoteEndpoint(
    const NetworkEndpoint& endpoint);
bool IsDefinitiveRemoteRouteFailure(int nativeError);
const char* ProbeStageName(NetworkProbeStage stage);

const char* FamilyName(NetworkFamily family);
} // namespace netplay::network
