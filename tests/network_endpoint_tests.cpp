#include "netplay/core/network_endpoint.h"
#include "netplay/core/network_capability.h"

#include <winsock2.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace
{
using netplay::network::FormatEndpoint;
using netplay::network::CanAttemptGlobalPublicDiscovery;
using netplay::network::GetFamilyCapability;
using netplay::network::IsGloballyRoutableHost;
using netplay::network::IsDefinitiveRemoteRouteFailure;
using netplay::network::NetworkEndpoint;
using netplay::network::NetworkFamily;
using netplay::network::NetworkHost;
using netplay::network::LocalFamilyCapability;
using netplay::network::LocalFamilyCapabilityState;
using netplay::network::LocalNetworkCapabilitySnapshot;
using netplay::network::ParseBareHost;
using netplay::network::ParseEndpoint;
using netplay::network::ParseRemoteHostInput;
using netplay::network::RemoteEndpointResolveFailure;
using netplay::network::RemoteEndpointResolution;
using netplay::network::RemoteHostInput;
using netplay::network::ResolveRemoteEndpoint;
using netplay::network::ScanLocalNetworkCapabilities;

int gFailures = 0;

void Check(bool condition, const char* expression, int line)
{
    if (condition)
    {
        return;
    }

    std::cerr << "FAIL line " << line << ": " << expression << '\n';
    ++gFailures;
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void ExpectBareHost(
    const std::string& text,
    NetworkFamily family,
    const std::string& normalized)
{
    NetworkHost parsed;
    CHECK(ParseBareHost(text, &parsed));
    CHECK(parsed.family == family);
    CHECK(parsed.host == normalized);
}

void ExpectInvalidBareHost(const std::string& text)
{
    NetworkHost parsed;
    parsed.family = NetworkFamily::IPv6;
    parsed.host = "unchanged";
    CHECK(!ParseBareHost(text, &parsed));
    CHECK(parsed.family == NetworkFamily::IPv6);
    CHECK(parsed.host == "unchanged");
}

void ExpectRemoteHostInput(
    const std::string& text,
    bool numeric,
    NetworkFamily family,
    const std::string& normalized)
{
    RemoteHostInput parsed;
    CHECK(ParseRemoteHostInput(text, &parsed));
    CHECK(parsed.numeric == numeric);
    CHECK(parsed.family == family);
    CHECK(parsed.host == normalized);
}

void ExpectInvalidRemoteHostInput(const std::string& text)
{
    RemoteHostInput parsed;
    parsed.numeric = true;
    parsed.family = NetworkFamily::IPv6;
    parsed.host = "unchanged";
    CHECK(!ParseRemoteHostInput(text, &parsed));
    CHECK(parsed.numeric);
    CHECK(parsed.family == NetworkFamily::IPv6);
    CHECK(parsed.host == "unchanged");
}

void ExpectEndpoint(
    const std::string& text,
    NetworkFamily family,
    const std::string& host,
    uint16_t port)
{
    NetworkEndpoint parsed;
    CHECK(ParseEndpoint(text, &parsed));
    CHECK(parsed.family == family);
    CHECK(parsed.host == host);
    CHECK(parsed.port == port);
}

void ExpectInvalidEndpoint(const std::string& text)
{
    NetworkEndpoint parsed;
    parsed.family = NetworkFamily::IPv6;
    parsed.host = "unchanged";
    parsed.port = 123;
    CHECK(!ParseEndpoint(text, &parsed));
    CHECK(parsed.family == NetworkFamily::IPv6);
    CHECK(parsed.host == "unchanged");
    CHECK(parsed.port == 123);
}

void TestBareHosts()
{
    ExpectBareHost("203.0.113.7", NetworkFamily::IPv4, "203.0.113.7");
    ExpectBareHost("  001.002.003.004\t", NetworkFamily::IPv4, "1.2.3.4");
    ExpectBareHost("2001:0db8:0:0:0:0:0:7", NetworkFamily::IPv6, "2001:db8::7");
    ExpectBareHost("[2001:db8::7]", NetworkFamily::IPv6, "2001:db8::7");
    ExpectBareHost("fe80::7%12", NetworkFamily::IPv6, "fe80::7%12");
    ExpectBareHost("[::ffff:192.0.2.1]", NetworkFamily::IPv6, "::ffff:192.0.2.1");

    ExpectInvalidBareHost("");
    ExpectInvalidBareHost("localhost");
    ExpectInvalidBareHost("203.0.113.7:10800");
    ExpectInvalidBareHost("[203.0.113.7]");
    ExpectInvalidBareHost("[2001:db8::7");
    ExpectInvalidBareHost("2001:db8::7]");
    ExpectInvalidBareHost("fe80::7%Ethernet");
    ExpectInvalidBareHost("fe80::7%");
    ExpectInvalidBareHost("fe80::7%4294967296");
    ExpectInvalidBareHost("256.0.0.1");
    ExpectInvalidBareHost("127.1");
}

void TestEndpoints()
{
    ExpectEndpoint(
        "203.0.113.7:10800",
        NetworkFamily::IPv4,
        "203.0.113.7",
        10800);
    ExpectEndpoint(
        "  001.002.003.004:00001 ",
        NetworkFamily::IPv4,
        "1.2.3.4",
        1);
    ExpectEndpoint(
        "[2001:0db8:0:0:0:0:0:7]:10800",
        NetworkFamily::IPv6,
        "2001:db8::7",
        10800);
    ExpectEndpoint(
        "[fe80::7%12]:65535",
        NetworkFamily::IPv6,
        "fe80::7%12",
        65535);

    ExpectInvalidEndpoint("");
    ExpectInvalidEndpoint("203.0.113.7");
    ExpectInvalidEndpoint("203.0.113.7:0");
    ExpectInvalidEndpoint("203.0.113.7:65536");
    ExpectInvalidEndpoint("203.0.113.7:80x");
    ExpectInvalidEndpoint("203.0.113.7: 80");
    ExpectInvalidEndpoint("localhost:10800");
    ExpectInvalidEndpoint("[203.0.113.7]:10800");
    ExpectInvalidEndpoint("[2001:db8::7]");
    ExpectInvalidEndpoint("[2001:db8::7]:");
    ExpectInvalidEndpoint("[2001:db8::7]:10800x");
    ExpectInvalidEndpoint("[2001:db8::7]extra:10800");
    ExpectInvalidEndpoint("[2001:db8::7]]:10800");
    ExpectInvalidEndpoint("[fe80::7%Ethernet]:10800");
    ExpectInvalidEndpoint("2001:db8::7:10800");
    ExpectInvalidEndpoint("::1:10800");
}

void TestRemoteHostInputs()
{
    ExpectRemoteHostInput(
        "  001.002.003.004 ",
        true,
        NetworkFamily::IPv4,
        "1.2.3.4");
    ExpectRemoteHostInput(
        "[2001:0db8::7]",
        true,
        NetworkFamily::IPv6,
        "2001:db8::7");
    ExpectRemoteHostInput(
        "localhost",
        false,
        NetworkFamily::IPv4,
        "localhost");
    ExpectRemoteHostInput(
        "  Player.Example-DDNS.Net\t",
        false,
        NetworkFamily::IPv4,
        "Player.Example-DDNS.Net");
    ExpectRemoteHostInput(
        "_efz._udp.example.net",
        false,
        NetworkFamily::IPv4,
        "_efz._udp.example.net");
    ExpectRemoteHostInput(
        "host.example.net.",
        false,
        NetworkFamily::IPv4,
        "host.example.net.");

    ExpectInvalidRemoteHostInput("");
    ExpectInvalidRemoteHostInput("256.0.0.1");
    ExpectInvalidRemoteHostInput("127.1");
    ExpectInvalidRemoteHostInput("12345");
    ExpectInvalidRemoteHostInput("host..example.net");
    ExpectInvalidRemoteHostInput("-host.example.net");
    ExpectInvalidRemoteHostInput("host-.example.net");
    ExpectInvalidRemoteHostInput("[localhost]");
    ExpectInvalidRemoteHostInput("localhost:10800");
    ExpectInvalidRemoteHostInput("host name.example.net");
    CHECK(!ParseRemoteHostInput("localhost", nullptr));
}

void ExpectPublicHost(const std::string& text, bool expected)
{
    NetworkHost parsed;
    CHECK(ParseBareHost(text, &parsed));
    CHECK(IsGloballyRoutableHost(parsed) == expected);
}

void TestPublicHostClassification()
{
    // Ordinary assigned Internet endpoints stay acceptable.
    ExpectPublicHost("8.8.8.8", true);
    ExpectPublicHost("1.1.1.1", true);
    ExpectPublicHost("192.0.0.9", true); // PCP anycast exception.
    ExpectPublicHost("2606:4700:4700::1111", true);
    ExpectPublicHost("2001:4860:4860::8888", true);

    // IPv4 special-purpose ranges must never be emitted as a public host.
    ExpectPublicHost("0.0.0.0", false);
    ExpectPublicHost("10.0.0.1", false);
    ExpectPublicHost("100.64.0.1", false);
    ExpectPublicHost("127.0.0.1", false);
    ExpectPublicHost("169.254.1.1", false);
    ExpectPublicHost("172.16.0.1", false);
    ExpectPublicHost("192.0.0.8", false);
    ExpectPublicHost("192.0.2.1", false);
    ExpectPublicHost("192.88.99.1", false);
    ExpectPublicHost("192.168.0.1", false);
    ExpectPublicHost("198.18.0.1", false);
    ExpectPublicHost("198.51.100.1", false);
    ExpectPublicHost("203.0.113.1", false);
    ExpectPublicHost("224.0.0.1", false);

    // IPv6 must be global unicast rather than local, mapped, multicast, or
    // documentation/special-purpose space.
    ExpectPublicHost("::", false);
    ExpectPublicHost("::1", false);
    ExpectPublicHost("::ffff:192.0.2.1", false);
    ExpectPublicHost("fc00::1", false);
    ExpectPublicHost("fd12:3456::1", false);
    ExpectPublicHost("fe80::1%1", false);
    ExpectPublicHost("fec0::1", false);
    ExpectPublicHost("ff02::1", false);
    ExpectPublicHost("2001:db8::1", false);
    ExpectPublicHost("2001:2::1", false);
    ExpectPublicHost("2001:10::1", false);
    ExpectPublicHost("3fff::1", false);

    NetworkHost mismatched;
    mismatched.family = NetworkFamily::IPv6;
    mismatched.host = "8.8.8.8";
    CHECK(!IsGloballyRoutableHost(mismatched));
}

void TestRemoteResolution()
{
    NetworkEndpoint endpoint;
    endpoint.family = NetworkFamily::IPv6;
    endpoint.host = "unchanged";
    endpoint.port = 123;
    RemoteEndpointResolution resolution;

    CHECK(ResolveRemoteEndpoint(
        "001.002.003.004",
        10800,
        &endpoint,
        &resolution));
    CHECK(!resolution.inputWasHostname);
    CHECK(resolution.failure == RemoteEndpointResolveFailure::None);
    CHECK(endpoint.family == NetworkFamily::IPv4);
    CHECK(endpoint.host == "1.2.3.4");
    CHECK(endpoint.port == 10800);

    CHECK(ResolveRemoteEndpoint(
        "::1",
        10800,
        &endpoint,
        &resolution));
    CHECK(!resolution.inputWasHostname);
    CHECK(endpoint.family == NetworkFamily::IPv6);
    CHECK(endpoint.host == "::1");
    CHECK(endpoint.port == 10800);

    endpoint.family = NetworkFamily::IPv6;
    endpoint.host = "unchanged";
    endpoint.port = 123;
    CHECK(!ResolveRemoteEndpoint(
        "127.1",
        10800,
        &endpoint,
        &resolution));
    CHECK(resolution.failure
          == RemoteEndpointResolveFailure::InvalidInput);
    CHECK(endpoint.family == NetworkFamily::IPv6);
    CHECK(endpoint.host == "unchanged");
    CHECK(endpoint.port == 123);

    CHECK(ResolveRemoteEndpoint(
        "localhost",
        10800,
        &endpoint,
        &resolution));
    CHECK(resolution.inputWasHostname);
    CHECK(resolution.failure == RemoteEndpointResolveFailure::None);
    CHECK(resolution.ipv4CandidateSeen
          || resolution.ipv6CandidateSeen);
    CHECK(endpoint.port == 10800);

    NetworkHost selectedHost;
    CHECK(ParseBareHost(endpoint.host, &selectedHost));
    CHECK(selectedHost.family == endpoint.family);
    if (resolution.ipv4CandidateSeen
        && !resolution.ipv4Unavailable)
    {
        CHECK(endpoint.family == NetworkFamily::IPv4);
    }
    else
    {
        CHECK(endpoint.family == NetworkFamily::IPv6);
    }
}

void TestFormatting()
{
    std::string formatted = "unchanged";

    NetworkEndpoint ipv4;
    ipv4.family = NetworkFamily::IPv4;
    ipv4.host = "001.002.003.004";
    ipv4.port = 10800;
    CHECK(FormatEndpoint(ipv4, &formatted));
    CHECK(formatted == "1.2.3.4:10800");

    NetworkEndpoint ipv6;
    ipv6.family = NetworkFamily::IPv6;
    ipv6.host = "[2001:0db8:0:0:0:0:0:7]";
    ipv6.port = 10800;
    CHECK(FormatEndpoint(ipv6, &formatted));
    CHECK(formatted == "[2001:db8::7]:10800");

    NetworkEndpoint scopedIpv6;
    scopedIpv6.family = NetworkFamily::IPv6;
    scopedIpv6.host = "fe80::7%12";
    scopedIpv6.port = 65535;
    CHECK(FormatEndpoint(scopedIpv6, &formatted));
    CHECK(formatted == "[fe80::7%12]:65535");

    const std::string prior = formatted;
    ipv4.port = 0;
    CHECK(!FormatEndpoint(ipv4, &formatted));
    CHECK(formatted == prior);

    ipv4.port = 10800;
    ipv4.family = NetworkFamily::IPv6;
    CHECK(!FormatEndpoint(ipv4, &formatted));
    CHECK(formatted == prior);

    ipv4.family = static_cast<NetworkFamily>(0);
    CHECK(!FormatEndpoint(ipv4, &formatted));
    CHECK(formatted == prior);

    CHECK(!FormatEndpoint(scopedIpv6, nullptr));
}

void TestFamilyNames()
{
    CHECK(static_cast<uint8_t>(NetworkFamily::IPv4) == 4);
    CHECK(static_cast<uint8_t>(NetworkFamily::IPv6) == 6);
    CHECK(std::string(netplay::network::FamilyName(NetworkFamily::IPv4)) == "IPv4");
    CHECK(std::string(netplay::network::FamilyName(NetworkFamily::IPv6)) == "IPv6");
    CHECK(std::string(netplay::network::FamilyName(
              static_cast<NetworkFamily>(0)))
          == "Unknown");

    NetworkFamily parsed = NetworkFamily::IPv6;
    CHECK(netplay::network::TryParseFamilyName("IPv4", &parsed));
    CHECK(parsed == NetworkFamily::IPv4);
    CHECK(netplay::network::TryParseFamilyName("ipv6", &parsed));
    CHECK(parsed == NetworkFamily::IPv6);
    CHECK(netplay::network::TryParseFamilyName("  IpV4\t", &parsed));
    CHECK(parsed == NetworkFamily::IPv4);

    parsed = NetworkFamily::IPv6;
    CHECK(!netplay::network::TryParseFamilyName("IPv5", &parsed));
    CHECK(parsed == NetworkFamily::IPv6);
    CHECK(!netplay::network::TryParseFamilyName("4", &parsed));
    CHECK(parsed == NetworkFamily::IPv6);
    CHECK(!netplay::network::TryParseFamilyName("", &parsed));
    CHECK(parsed == NetworkFamily::IPv6);
    CHECK(!netplay::network::TryParseFamilyName("IPv4", nullptr));
}

void TestProbeStageNames()
{
    using netplay::network::NetworkProbeStage;
    using netplay::network::ProbeStageName;

    CHECK(std::string(ProbeStageName(NetworkProbeStage::None)) == "none");
    CHECK(std::string(ProbeStageName(NetworkProbeStage::WinsockStartup))
          == "WSAStartup");
    CHECK(std::string(ProbeStageName(NetworkProbeStage::SocketCreate))
          == "socket");
    CHECK(std::string(ProbeStageName(NetworkProbeStage::BindWildcard))
          == "bind_wildcard");
    CHECK(std::string(ProbeStageName(NetworkProbeStage::NumericAddress))
          == "numeric_address");
    CHECK(std::string(ProbeStageName(NetworkProbeStage::UdpRoute))
          == "udp_route");
}

void TestRemoteRouteFailureClassification()
{
    CHECK(IsDefinitiveRemoteRouteFailure(WSAEAFNOSUPPORT));
    CHECK(IsDefinitiveRemoteRouteFailure(WSAEPROTONOSUPPORT));
    CHECK(IsDefinitiveRemoteRouteFailure(WSAEADDRNOTAVAIL));
    CHECK(IsDefinitiveRemoteRouteFailure(WSAENETUNREACH));
    CHECK(IsDefinitiveRemoteRouteFailure(WSAEHOSTUNREACH));

    // Transient/global policy errors remain ambiguous and are left to
    // Revival's native connection path instead of forcing a family switch.
    CHECK(!IsDefinitiveRemoteRouteFailure(WSAEACCES));
    CHECK(!IsDefinitiveRemoteRouteFailure(WSAETIMEDOUT));
    CHECK(!IsDefinitiveRemoteRouteFailure(WSAECONNRESET));
}

void TestLocalFamilyCapabilityPolicy()
{
    LocalFamilyCapability capability;
    capability.state = LocalFamilyCapabilityState::HardUnavailable;
    CHECK(!CanAttemptGlobalPublicDiscovery(capability));
    capability.state = LocalFamilyCapabilityState::LocalOnly;
    CHECK(!CanAttemptGlobalPublicDiscovery(capability));
    capability.state = LocalFamilyCapabilityState::Unknown;
    CHECK(CanAttemptGlobalPublicDiscovery(capability));
    capability.state = LocalFamilyCapabilityState::Routable;
    CHECK(CanAttemptGlobalPublicDiscovery(capability));

    // The actual probe is intentionally environment-dependent, but it must be
    // safe to run on a worker and always return a coherent two-family result.
    const LocalNetworkCapabilitySnapshot snapshot =
        ScanLocalNetworkCapabilities();
    CHECK(GetFamilyCapability(snapshot, NetworkFamily::IPv4).family
          == NetworkFamily::IPv4);
    CHECK(GetFamilyCapability(snapshot, NetworkFamily::IPv6).family
          == NetworkFamily::IPv6);
}
} // namespace

int main()
{
    TestBareHosts();
    TestEndpoints();
    TestRemoteHostInputs();
    TestPublicHostClassification();
    TestRemoteResolution();
    TestFormatting();
    TestFamilyNames();
    TestProbeStageNames();
    TestRemoteRouteFailureClassification();
    TestLocalFamilyCapabilityPolicy();

    if (gFailures != 0)
    {
        std::cerr << gFailures << " network endpoint test(s) failed\n";
        return 1;
    }

    std::cout << "All network endpoint tests passed\n";
    return 0;
}
