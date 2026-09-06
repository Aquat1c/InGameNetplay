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
    // Trimmed, normalized numeric literal. IPv6 hosts never include [].
    std::string host;
};

struct NetworkEndpoint
{
    NetworkFamily family = NetworkFamily::IPv4;
    // Trimmed, normalized numeric literal. IPv6 hosts never include [].
    std::string host;
    // A remote/effective endpoint always has a non-zero port.
    uint16_t port = 0;
};

struct RemoteHostInput
{
    // True for a strict IPv4/IPv6 literal, false for a DNS hostname.
    bool numeric = false;
    // Meaningful only when numeric is true. Hostname family is selected from
    // the resolved A/AAAA candidates for each individual session.
    NetworkFamily family = NetworkFamily::IPv4;
    // Normalized numeric literal, or a validated hostname kept verbatim
    // (apart from surrounding ASCII whitespace).
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
    // True only for a definitive local capability/route failure. Callers
    // should leave ambiguous/transient failures to Revival's native path.
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

// Parses a numeric address without a port. Bare IPv6 and [bracketed IPv6]
// are accepted; the returned host never includes brackets. DNS names and
// named IPv6 scope IDs are rejected. The output is unchanged on failure.
bool ParseBareHost(const std::string& text, NetworkHost* outHost);

// Returns true only for a numeric address suitable for advertising as a
// public Internet endpoint. Private, loopback, link-local, multicast,
// documentation, and other special-purpose ranges are rejected. This is
// intentionally stricter than ParseBareHost: LAN/manual Join addresses remain
// valid and must not be filtered through this helper.
bool IsGloballyRoutableHost(const NetworkHost& host);

// Parses the address accepted by the Join/Spectate UI. Strict numeric
// literals retain ParseBareHost's family inference and normalization. A
// syntactically valid DNS hostname (including localhost/DDNS names) is kept
// unresolved so it can be saved and resolved afresh for each session.
// Numeric-looking text which is not a strict literal is rejected rather than
// being reinterpreted through legacy IPv4 shorthand rules.
bool ParseRemoteHostInput(
    const std::string& text,
    RemoteHostInput* outInput);

// Parses a complete remote endpoint. Accepted forms are strictly
// IPv4:port and [IPv6%numeric-scope]:port. Unbracketed IPv6 endpoints are
// rejected as ambiguous. The port must be fully decimal and in [1, 65535].
// The output is unchanged on failure.
bool ParseEndpoint(const std::string& text, NetworkEndpoint* outEndpoint);

// Formats a validated endpoint as IPv4:port or [IPv6]:port. Returns false
// for a zero port, invalid numeric host, or a host/family mismatch. The
// output is unchanged on failure.
bool FormatEndpoint(const NetworkEndpoint& endpoint, std::string* outText);

// Resolves a validated Join/Spectate address to the immutable numeric
// endpoint used by one session. Numeric input never changes family. Hostnames
// prefer a locally usable IPv4 candidate, then safely fall back to IPv6 when
// IPv4 is unavailable. The endpoint output is unchanged on failure.
bool ResolveRemoteEndpoint(
    const std::string& hostInput,
    uint16_t port,
    NetworkEndpoint* outEndpoint,
    RemoteEndpointResolution* outResolution = nullptr);

// Parses the canonical EfzRevival Protocol values (IPv4 or IPv6)
// case-insensitively. The output is unchanged on failure.
bool TryParseFamilyName(const std::string& text, NetworkFamily* outFamily);

// Non-sending local capability checks used before destructive session setup.
// Host probes socket creation plus an ephemeral wildcard bind. Remote probes
// socket creation plus UDP route selection; UDP connect sends no packet.
NetworkFamilyProbeResult ProbeLocalHostFamily(NetworkFamily family);
NetworkFamilyProbeResult ProbeRemoteEndpoint(
    const NetworkEndpoint& endpoint);
// Deterministic WinSock error classification used by the remote UDP route
// probe. These errors prove that the candidate cannot use its local family or
// route, so hostname resolution may continue to another candidate/family.
bool IsDefinitiveRemoteRouteFailure(int nativeError);
const char* ProbeStageName(NetworkProbeStage stage);

const char* FamilyName(NetworkFamily family);
} // namespace netplay::network
