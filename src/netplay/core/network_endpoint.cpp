#include "netplay/core/network_endpoint.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace netplay::network
{
namespace
{
std::string TrimAscii(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size()
           && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
    {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin
           && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }
    return value.substr(begin, end - begin);
}

class WinsockLease
{
public:
    WinsockLease()
    {
        WSADATA data = {};
        error_ = WSAStartup(MAKEWORD(2, 0), &data);
        active_ = error_ == 0;
    }

    ~WinsockLease()
    {
        if (active_)
        {
            WSACleanup();
        }
    }

    WinsockLease(const WinsockLease&) = delete;
    WinsockLease& operator=(const WinsockLease&) = delete;

    bool IsActive() const
    {
        return active_;
    }

    int Error() const
    {
        return error_;
    }

private:
    bool active_ = false;
    int error_ = 0;
};

int ToNativeFamily(NetworkFamily family)
{
    return family == NetworkFamily::IPv6 ? AF_INET6 : AF_INET;
}

bool IsKnownFamily(NetworkFamily family)
{
    return family == NetworkFamily::IPv4
        || family == NetworkFamily::IPv6;
}

bool IsDefinitiveFamilySocketError(int error)
{
    return error == WSAEAFNOSUPPORT
        || error == WSAEPROTONOSUPPORT;
}

NetworkFamilyProbeResult MakeProbeFailure(
    int nativeError,
    NetworkProbeStage stage,
    bool unavailable)
{
    NetworkFamilyProbeResult result;
    result.unavailable = unavailable;
    result.nativeError = nativeError;
    result.stage = stage;
    return result;
}

bool BuildNativeEndpoint(
    const NetworkEndpoint& endpoint,
    SOCKADDR_STORAGE* outStorage,
    int* outStorageLength)
{
    if (outStorage == nullptr
        || outStorageLength == nullptr
        || !IsKnownFamily(endpoint.family)
        || endpoint.port == 0
        || endpoint.host.empty()
        || endpoint.host.size() >= 128)
    {
        return false;
    }

    std::array<wchar_t, 128> addressText = {};
    for (size_t index = 0; index < endpoint.host.size(); ++index)
    {
        addressText[index] =
            static_cast<unsigned char>(endpoint.host[index]);
    }

    SOCKADDR_STORAGE storage = {};
    int storageLength = sizeof(storage);
    if (WSAStringToAddressW(
            addressText.data(),
            ToNativeFamily(endpoint.family),
            nullptr,
            reinterpret_cast<LPSOCKADDR>(&storage),
            &storageLength)
        != 0)
    {
        return false;
    }

    if (endpoint.family == NetworkFamily::IPv6)
    {
        reinterpret_cast<sockaddr_in6*>(&storage)->sin6_port =
            htons(endpoint.port);
    }
    else
    {
        reinterpret_cast<sockaddr_in*>(&storage)->sin_port =
            htons(endpoint.port);
    }

    *outStorage = storage;
    *outStorageLength = storageLength;
    return true;
}

bool ParseDecimalPort(const std::string& text, uint16_t* outPort)
{
    if (outPort == nullptr || text.empty() || text.size() > 5)
    {
        return false;
    }

    uint32_t value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
        value = value * 10u + static_cast<uint32_t>(c - '0');
    }

    if (value == 0 || value > std::numeric_limits<uint16_t>::max())
    {
        return false;
    }

    *outPort = static_cast<uint16_t>(value);
    return true;
}

bool NormalizeStrictIpv4Syntax(
    const std::string& text,
    std::string* outDecimalAddress)
{
    if (outDecimalAddress == nullptr || text.empty())
    {
        return false;
    }

    std::array<uint32_t, 4> octets = {};
    size_t octetIndex = 0;
    size_t digitsInOctet = 0;

    for (const char c : text)
    {
        if (c == '.')
        {
            if (digitsInOctet == 0 || octetIndex >= octets.size() - 1)
            {
                return false;
            }
            ++octetIndex;
            digitsInOctet = 0;
            continue;
        }

        if (c < '0' || c > '9' || octetIndex >= octets.size())
        {
            return false;
        }

        ++digitsInOctet;
        if (digitsInOctet > 3)
        {
            return false;
        }

        octets[octetIndex] =
            octets[octetIndex] * 10u + static_cast<uint32_t>(c - '0');
        if (octets[octetIndex] > 255u)
        {
            return false;
        }
    }

    if (octetIndex != octets.size() - 1 || digitsInOctet == 0)
    {
        return false;
    }

    *outDecimalAddress =
        std::to_string(octets[0]) + "."
        + std::to_string(octets[1]) + "."
        + std::to_string(octets[2]) + "."
        + std::to_string(octets[3]);
    return true;
}

bool HasValidIpv6Syntax(const std::string& text)
{
    if (text.empty() || text.find(':') == std::string::npos)
    {
        return false;
    }

    const size_t scopeSeparator = text.find('%');
    if (scopeSeparator != std::string::npos)
    {
        if (scopeSeparator == 0
            || scopeSeparator + 1 >= text.size()
            || text.find('%', scopeSeparator + 1) != std::string::npos)
        {
            return false;
        }

        uint64_t scopeId = 0;
        for (size_t i = scopeSeparator + 1; i < text.size(); ++i)
        {
            const char c = text[i];
            if (c < '0' || c > '9')
            {
                return false;
            }
            scopeId = scopeId * 10u + static_cast<uint64_t>(c - '0');
            if (scopeId > std::numeric_limits<uint32_t>::max())
            {
                return false;
            }
        }
    }

    const size_t addressEnd =
        scopeSeparator == std::string::npos ? text.size() : scopeSeparator;
    for (size_t i = 0; i < addressEnd; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isxdigit(c) == 0 && c != ':' && c != '.')
        {
            return false;
        }
    }
    return true;
}

bool NormalizeWithWinsock(
    const std::string& text,
    int addressFamily,
    std::string* outNormalized)
{
    if (outNormalized == nullptr || text.empty() || text.size() >= 128)
    {
        return false;
    }

    WinsockLease winsock;
    if (!winsock.IsActive())
    {
        return false;
    }

    std::array<wchar_t, 128> mutableAddress = {};
    for (size_t i = 0; i < text.size(); ++i)
    {
        // Numeric address syntax is ASCII-only by construction.
        mutableAddress[i] = static_cast<unsigned char>(text[i]);
    }

    SOCKADDR_STORAGE storage = {};
    int storageLength = sizeof(storage);
    if (WSAStringToAddressW(
            mutableAddress.data(),
            addressFamily,
            nullptr,
            reinterpret_cast<LPSOCKADDR>(&storage),
            &storageLength)
        != 0)
    {
        return false;
    }

    std::array<wchar_t, 128> normalizedAddress = {};
    DWORD normalizedLength = static_cast<DWORD>(normalizedAddress.size());
    if (WSAAddressToStringW(
            reinterpret_cast<LPSOCKADDR>(&storage),
            static_cast<DWORD>(storageLength),
            nullptr,
            normalizedAddress.data(),
            &normalizedLength)
        != 0)
    {
        return false;
    }

    std::string normalized;
    for (const wchar_t c : normalizedAddress)
    {
        if (c == L'\0')
        {
            break;
        }
        if (static_cast<unsigned int>(c) > 0x7Fu)
        {
            return false;
        }
        normalized.push_back(static_cast<char>(c));
    }
    if (addressFamily == AF_INET6
        && normalized.size() >= 2
        && normalized.front() == '['
        && normalized.back() == ']')
    {
        normalized = normalized.substr(1, normalized.size() - 2);
    }

    if (normalized.empty()
        || normalized.front() == '['
        || normalized.back() == ']')
    {
        return false;
    }

    *outNormalized = normalized;
    return true;
}

bool ParseUnbracketedHost(const std::string& text, NetworkHost* outHost)
{
    if (outHost == nullptr || text.empty())
    {
        return false;
    }

    std::string strictIpv4;
    std::string normalized;
    if (NormalizeStrictIpv4Syntax(text, &strictIpv4)
        && NormalizeWithWinsock(strictIpv4, AF_INET, &normalized))
    {
        NetworkHost parsed;
        parsed.family = NetworkFamily::IPv4;
        parsed.host = std::move(normalized);
        *outHost = std::move(parsed);
        return true;
    }

    if (!HasValidIpv6Syntax(text)
        || !NormalizeWithWinsock(text, AF_INET6, &normalized))
    {
        return false;
    }

    NetworkHost parsed;
    parsed.family = NetworkFamily::IPv6;
    parsed.host = std::move(normalized);
    *outHost = std::move(parsed);
    return true;
}

bool DecodeNormalizedHost(
    const NetworkHost& host,
    SOCKADDR_STORAGE* outStorage)
{
    if (outStorage == nullptr
        || !IsKnownFamily(host.family)
        || host.host.empty()
        || host.host.size() >= 128)
    {
        return false;
    }

    NetworkHost normalized;
    if (!ParseBareHost(host.host, &normalized)
        || normalized.family != host.family)
    {
        return false;
    }

    // A scope ID identifies a local interface, so it cannot describe a
    // globally routable address even if the IPv6 bits would otherwise look
    // public.
    if (normalized.family == NetworkFamily::IPv6
        && normalized.host.find('%') != std::string::npos)
    {
        return false;
    }

    WinsockLease winsock;
    if (!winsock.IsActive())
    {
        return false;
    }

    std::array<wchar_t, 128> addressText = {};
    for (size_t index = 0; index < normalized.host.size(); ++index)
    {
        addressText[index] =
            static_cast<unsigned char>(normalized.host[index]);
    }

    SOCKADDR_STORAGE storage = {};
    int storageLength = sizeof(storage);
    if (WSAStringToAddressW(
            addressText.data(),
            ToNativeFamily(normalized.family),
            nullptr,
            reinterpret_cast<LPSOCKADDR>(&storage),
            &storageLength)
        != 0)
    {
        return false;
    }

    *outStorage = storage;
    return true;
}

bool IsGloballyRoutableIpv4(const IN_ADDR& address)
{
    const uint32_t value = ntohl(address.S_un.S_addr);
    const uint8_t first = static_cast<uint8_t>(value >> 24u);
    const uint8_t second = static_cast<uint8_t>(value >> 16u);
    const uint8_t third = static_cast<uint8_t>(value >> 8u);
    const uint8_t fourth = static_cast<uint8_t>(value);

    // IANA special-purpose ranges that must never be published as a public
    // listener address.  The two PCP anycast addresses in 192.0.0.0/24 are
    // the only globally reachable exceptions in that otherwise reserved /24.
    if (first == 0u
        || first == 10u
        || first == 127u
        || first >= 224u
        || (first == 100u && second >= 64u && second <= 127u)
        || (first == 169u && second == 254u)
        || (first == 172u && second >= 16u && second <= 31u)
        || (first == 192u && second == 168u)
        || (first == 192u && second == 0u && third == 0u
            && fourth != 9u && fourth != 10u)
        || (first == 192u && second == 0u && third == 2u)
        || (first == 192u && second == 88u && third == 99u)
        || (first == 198u && (second == 18u || second == 19u))
        || (first == 198u && second == 51u && third == 100u)
        || (first == 203u && second == 0u && third == 113u))
    {
        return false;
    }

    return true;
}

template <size_t PrefixLength>
bool HasIpv6Prefix(
    const IN6_ADDR& address,
    const std::array<uint8_t, PrefixLength>& prefix)
{
    const auto* bytes =
        reinterpret_cast<const uint8_t*>(&address);
    for (size_t index = 0; index < PrefixLength; ++index)
    {
        if (bytes[index] != prefix[index])
        {
            return false;
        }
    }
    return true;
}

bool IsGloballyRoutableIpv6(const IN6_ADDR& address)
{
    const auto* bytes =
        reinterpret_cast<const uint8_t*>(&address);

    // Public native IPv6 source addresses live in global-unicast 2000::/3.
    // This excludes unspecified, loopback, IPv4-mapped, ULA, link-local,
    // site-local, multicast, and translation-only/local-use prefixes.
    if (bytes[0] < 0x20u || bytes[0] > 0x3Fu)
    {
        return false;
    }

    // Reject globally non-routable special-purpose subranges inside 2000::/3
    // as well.  This keeps documentation and benchmark addresses from ever
    // becoming a lobby/direct-host endpoint.
    if (HasIpv6Prefix(
            address,
            std::array<uint8_t, 4>{0x20u, 0x01u, 0x0Du, 0xB8u})
        || HasIpv6Prefix(
            address,
            std::array<uint8_t, 6>{0x20u, 0x01u, 0x00u, 0x02u, 0x00u, 0x00u})
        || HasIpv6Prefix(
            address,
            std::array<uint8_t, 3>{0x3Fu, 0xFFu, 0x00u}))
    {
        return false;
    }

    // ORCHID and ORCHIDv2: 2001:10::/28 and 2001:20::/28.
    if (bytes[0] == 0x20u
        && bytes[1] == 0x01u
        && bytes[2] == 0x00u
        && ((bytes[3] & 0xF0u) == 0x10u
            || (bytes[3] & 0xF0u) == 0x20u))
    {
        return false;
    }

    return true;
}

bool IsValidRemoteHostname(const std::string& text)
{
    // NetbridgeStatus::address and the original Join editor both have a
    // 63-byte payload limit. Keep the accepted hostname contract aligned so
    // a value which validates can never be silently truncated elsewhere.
    if (text.empty() || text.size() > 63)
    {
        return false;
    }

    const bool hasTrailingRootDot = text.back() == '.';
    const size_t nameEnd =
        hasTrailingRootDot ? text.size() - 1 : text.size();
    if (nameEnd == 0)
    {
        return false;
    }

    bool hasNonNumericNameCharacter = false;
    size_t labelStart = 0;
    for (size_t index = 0; index < nameEnd; ++index)
    {
        const unsigned char c =
            static_cast<unsigned char>(text[index]);
        if (c == '.')
        {
            const size_t labelLength = index - labelStart;
            if (labelLength == 0
                || labelLength > 63
                || text[labelStart] == '-'
                || text[index - 1] == '-')
            {
                return false;
            }
            labelStart = index + 1;
            continue;
        }

        if (std::isalnum(c) == 0 && c != '-' && c != '_')
        {
            return false;
        }
        if (std::isalpha(c) != 0 || c == '-' || c == '_')
        {
            hasNonNumericNameCharacter = true;
        }
    }

    const size_t finalLabelLength = nameEnd - labelStart;
    if (finalLabelLength == 0
        || finalLabelLength > 63
        || text[labelStart] == '-'
        || text[nameEnd - 1] == '-')
    {
        return false;
    }

    // Do not let malformed or shorthand numeric IPv4 text (127.1,
    // 256.0.0.1, integer-form addresses, and similar) bypass the strict
    // numeric parser by falling through to getaddrinfo.
    return hasNonNumericNameCharacter;
}

bool TryConvertResolvedAddress(
    const addrinfo* addressInfo,
    uint16_t port,
    NetworkEndpoint* outEndpoint)
{
    if (addressInfo == nullptr
        || addressInfo->ai_addr == nullptr
        || outEndpoint == nullptr
        || port == 0
        || (addressInfo->ai_family != AF_INET
            && addressInfo->ai_family != AF_INET6))
    {
        return false;
    }

    std::array<char, 128> numericHost = {};
    if (getnameinfo(
            addressInfo->ai_addr,
            static_cast<int>(addressInfo->ai_addrlen),
            numericHost.data(),
            static_cast<DWORD>(numericHost.size()),
            nullptr,
            0,
            NI_NUMERICHOST)
        != 0)
    {
        return false;
    }

    NetworkHost parsedHost;
    if (!ParseUnbracketedHost(numericHost.data(), &parsedHost))
    {
        return false;
    }

    const NetworkFamily expectedFamily =
        addressInfo->ai_family == AF_INET6
        ? NetworkFamily::IPv6
        : NetworkFamily::IPv4;
    if (parsedHost.family != expectedFamily)
    {
        return false;
    }

    NetworkEndpoint endpoint;
    endpoint.family = parsedHost.family;
    endpoint.host = std::move(parsedHost.host);
    endpoint.port = port;
    *outEndpoint = std::move(endpoint);
    return true;
}

void AppendUniqueCandidate(
    const NetworkEndpoint& endpoint,
    std::vector<NetworkEndpoint>* candidates)
{
    if (candidates == nullptr)
    {
        return;
    }

    const auto existing = std::find_if(
        candidates->begin(),
        candidates->end(),
        [&endpoint](const NetworkEndpoint& candidate) {
            return candidate.family == endpoint.family
                && candidate.host == endpoint.host
                && candidate.port == endpoint.port;
        });
    if (existing == candidates->end())
    {
        candidates->push_back(endpoint);
    }
}
} // namespace

bool ParseBareHost(const std::string& text, NetworkHost* outHost)
{
    if (outHost == nullptr)
    {
        return false;
    }

    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty())
    {
        return false;
    }

    if (trimmed.front() == '[' || trimmed.back() == ']')
    {
        if (trimmed.size() < 3
            || trimmed.front() != '['
            || trimmed.back() != ']')
        {
            return false;
        }

        NetworkHost parsed;
        if (!ParseUnbracketedHost(
                trimmed.substr(1, trimmed.size() - 2),
                &parsed)
            || parsed.family != NetworkFamily::IPv6)
        {
            return false;
        }

        *outHost = std::move(parsed);
        return true;
    }

    return ParseUnbracketedHost(trimmed, outHost);
}

bool IsGloballyRoutableHost(const NetworkHost& host)
{
    SOCKADDR_STORAGE storage = {};
    if (!DecodeNormalizedHost(host, &storage))
    {
        return false;
    }

    if (host.family == NetworkFamily::IPv4
        && storage.ss_family == AF_INET)
    {
        return IsGloballyRoutableIpv4(
            reinterpret_cast<const sockaddr_in*>(&storage)->sin_addr);
    }
    if (host.family == NetworkFamily::IPv6
        && storage.ss_family == AF_INET6)
    {
        return IsGloballyRoutableIpv6(
            reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_addr);
    }
    return false;
}

bool ParseRemoteHostInput(
    const std::string& text,
    RemoteHostInput* outInput)
{
    if (outInput == nullptr)
    {
        return false;
    }

    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty())
    {
        return false;
    }

    NetworkHost numericHost;
    if (ParseBareHost(trimmed, &numericHost))
    {
        RemoteHostInput parsed;
        parsed.numeric = true;
        parsed.family = numericHost.family;
        parsed.host = std::move(numericHost.host);
        *outInput = std::move(parsed);
        return true;
    }

    if (!IsValidRemoteHostname(trimmed))
    {
        return false;
    }

    RemoteHostInput parsed;
    parsed.numeric = false;
    parsed.host = trimmed;
    *outInput = std::move(parsed);
    return true;
}

bool ParseEndpoint(const std::string& text, NetworkEndpoint* outEndpoint)
{
    if (outEndpoint == nullptr)
    {
        return false;
    }

    const std::string trimmed = TrimAscii(text);
    if (trimmed.empty())
    {
        return false;
    }

    std::string hostText;
    std::string portText;
    NetworkFamily requiredFamily = NetworkFamily::IPv4;

    if (trimmed.front() == '[')
    {
        const size_t closingBracket = trimmed.find(']');
        if (closingBracket == std::string::npos
            || closingBracket == 1
            || closingBracket + 2 > trimmed.size()
            || trimmed[closingBracket + 1] != ':'
            || trimmed.find('[', 1) != std::string::npos
            || trimmed.find(']', closingBracket + 1) != std::string::npos)
        {
            return false;
        }

        hostText = trimmed.substr(1, closingBracket - 1);
        portText = trimmed.substr(closingBracket + 2);
        requiredFamily = NetworkFamily::IPv6;
    }
    else
    {
        const size_t separator = trimmed.find(':');
        if (separator == std::string::npos
            || separator == 0
            || separator + 1 >= trimmed.size()
            || trimmed.find(':', separator + 1) != std::string::npos)
        {
            // More than one colon is an ambiguous unbracketed IPv6 endpoint.
            return false;
        }

        hostText = trimmed.substr(0, separator);
        portText = trimmed.substr(separator + 1);
    }

    uint16_t port = 0;
    NetworkHost parsedHost;
    if (!ParseDecimalPort(portText, &port)
        || !ParseUnbracketedHost(hostText, &parsedHost)
        || parsedHost.family != requiredFamily)
    {
        return false;
    }

    NetworkEndpoint parsed;
    parsed.family = parsedHost.family;
    parsed.host = std::move(parsedHost.host);
    parsed.port = port;
    *outEndpoint = std::move(parsed);
    return true;
}

bool FormatEndpoint(const NetworkEndpoint& endpoint, std::string* outText)
{
    if (outText == nullptr || endpoint.port == 0)
    {
        return false;
    }

    NetworkHost parsedHost;
    if (!ParseBareHost(endpoint.host, &parsedHost)
        || parsedHost.family != endpoint.family)
    {
        return false;
    }

    std::string formatted;
    if (endpoint.family == NetworkFamily::IPv6)
    {
        formatted = "[" + parsedHost.host + "]:";
    }
    else if (endpoint.family == NetworkFamily::IPv4)
    {
        formatted = parsedHost.host + ":";
    }
    else
    {
        return false;
    }

    formatted += std::to_string(endpoint.port);
    *outText = std::move(formatted);
    return true;
}

bool ResolveRemoteEndpoint(
    const std::string& hostInput,
    uint16_t port,
    NetworkEndpoint* outEndpoint,
    RemoteEndpointResolution* outResolution)
{
    RemoteEndpointResolution resolution;
    auto publishResolution = [&]() {
        if (outResolution != nullptr)
        {
            *outResolution = resolution;
        }
    };

    RemoteHostInput parsedInput;
    if (outEndpoint == nullptr
        || port == 0
        || !ParseRemoteHostInput(hostInput, &parsedInput))
    {
        resolution.failure =
            RemoteEndpointResolveFailure::InvalidInput;
        publishResolution();
        return false;
    }

    resolution.inputWasHostname = !parsedInput.numeric;
    if (parsedInput.numeric)
    {
        NetworkEndpoint endpoint;
        endpoint.family = parsedInput.family;
        endpoint.host = parsedInput.host;
        endpoint.port = port;
        resolution.ipv4CandidateSeen =
            endpoint.family == NetworkFamily::IPv4;
        resolution.ipv6CandidateSeen =
            endpoint.family == NetworkFamily::IPv6;
        resolution.selectedProbe = ProbeRemoteEndpoint(endpoint);
        *outEndpoint = std::move(endpoint);
        publishResolution();
        return true;
    }

    WinsockLease winsock;
    if (!winsock.IsActive())
    {
        resolution.failure =
            RemoteEndpointResolveFailure::WinsockStartup;
        resolution.nativeError = winsock.Error();
        publishResolution();
        return false;
    }

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* addressList = nullptr;
    const int lookupResult = getaddrinfo(
        parsedInput.host.c_str(),
        nullptr,
        &hints,
        &addressList);
    if (lookupResult != 0)
    {
        resolution.failure =
            RemoteEndpointResolveFailure::NameLookup;
        resolution.nativeError = lookupResult;
        publishResolution();
        return false;
    }

    std::vector<NetworkEndpoint> candidates;
    for (const addrinfo* candidate = addressList;
         candidate != nullptr;
         candidate = candidate->ai_next)
    {
        NetworkEndpoint endpoint;
        if (TryConvertResolvedAddress(candidate, port, &endpoint))
        {
            AppendUniqueCandidate(endpoint, &candidates);
        }
    }
    freeaddrinfo(addressList);

    for (const NetworkEndpoint& candidate : candidates)
    {
        resolution.ipv4CandidateSeen =
            resolution.ipv4CandidateSeen
            || candidate.family == NetworkFamily::IPv4;
        resolution.ipv6CandidateSeen =
            resolution.ipv6CandidateSeen
            || candidate.family == NetworkFamily::IPv6;
    }

    if (candidates.empty())
    {
        resolution.failure =
            RemoteEndpointResolveFailure::NameLookup;
        resolution.nativeError = WSAHOST_NOT_FOUND;
        publishResolution();
        return false;
    }

    const std::array<NetworkFamily, 2> familyPriority = {
        NetworkFamily::IPv4,
        NetworkFamily::IPv6,
    };
    for (const NetworkFamily family : familyPriority)
    {
        bool familyCandidateSeen = false;
        bool everyCandidateUnavailable = true;
        for (const NetworkEndpoint& candidate : candidates)
        {
            if (candidate.family != family)
            {
                continue;
            }

            familyCandidateSeen = true;
            const NetworkFamilyProbeResult probe =
                ProbeRemoteEndpoint(candidate);
            resolution.selectedProbe = probe;
            if (probe.unavailable)
            {
                resolution.nativeError = probe.nativeError;
                continue;
            }

            everyCandidateUnavailable = false;
            *outEndpoint = candidate;
            resolution.failure =
                RemoteEndpointResolveFailure::None;
            publishResolution();
            return true;
        }

        if (familyCandidateSeen && everyCandidateUnavailable)
        {
            if (family == NetworkFamily::IPv4)
            {
                resolution.ipv4Unavailable = true;
            }
            else
            {
                resolution.ipv6Unavailable = true;
            }
        }
    }

    resolution.failure =
        resolution.selectedProbe.stage
                == NetworkProbeStage::WinsockStartup
            ? RemoteEndpointResolveFailure::WinsockStartup
            : RemoteEndpointResolveFailure::NoUsableFamily;
    publishResolution();
    return false;
}

bool TryParseFamilyName(const std::string& text, NetworkFamily* outFamily)
{
    if (outFamily == nullptr)
    {
        return false;
    }

    const std::string trimmed = TrimAscii(text);
    if (trimmed.size() != 4)
    {
        return false;
    }

    const bool prefixMatches =
        (trimmed[0] == 'I' || trimmed[0] == 'i')
        && (trimmed[1] == 'P' || trimmed[1] == 'p')
        && (trimmed[2] == 'V' || trimmed[2] == 'v');
    if (!prefixMatches)
    {
        return false;
    }

    if (trimmed[3] == '4')
    {
        *outFamily = NetworkFamily::IPv4;
        return true;
    }
    if (trimmed[3] == '6')
    {
        *outFamily = NetworkFamily::IPv6;
        return true;
    }
    return false;
}

NetworkFamilyProbeResult ProbeLocalHostFamily(NetworkFamily family)
{
    if (!IsKnownFamily(family))
    {
        return MakeProbeFailure(
            WSAEAFNOSUPPORT,
            NetworkProbeStage::SocketCreate,
            true);
    }

    WinsockLease winsock;
    if (!winsock.IsActive())
    {
        return MakeProbeFailure(
            winsock.Error(),
            NetworkProbeStage::WinsockStartup,
            true);
    }

    const int nativeFamily = ToNativeFamily(family);
    const SOCKET socketHandle =
        socket(nativeFamily, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET)
    {
        const int error = WSAGetLastError();
        return MakeProbeFailure(
            error,
            NetworkProbeStage::SocketCreate,
            IsDefinitiveFamilySocketError(error));
    }

    int bindResult = SOCKET_ERROR;
    if (family == NetworkFamily::IPv6)
    {
        sockaddr_in6 any = {};
        any.sin6_family = AF_INET6;
        bindResult = bind(
            socketHandle,
            reinterpret_cast<const sockaddr*>(&any),
            sizeof(any));
    }
    else
    {
        sockaddr_in any = {};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        bindResult = bind(
            socketHandle,
            reinterpret_cast<const sockaddr*>(&any),
            sizeof(any));
    }

    NetworkFamilyProbeResult result;
    if (bindResult == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        result = MakeProbeFailure(
            error,
            NetworkProbeStage::BindWildcard,
            error == WSAEADDRNOTAVAIL
                || IsDefinitiveFamilySocketError(error));
    }
    closesocket(socketHandle);
    return result;
}

NetworkFamilyProbeResult ProbeRemoteEndpoint(
    const NetworkEndpoint& endpoint)
{
    NetworkHost parsedHost;
    if (!ParseBareHost(endpoint.host, &parsedHost)
        || parsedHost.family != endpoint.family
        || endpoint.port == 0)
    {
        return MakeProbeFailure(
            WSAEINVAL,
            NetworkProbeStage::NumericAddress,
            true);
    }

    WinsockLease winsock;
    if (!winsock.IsActive())
    {
        return MakeProbeFailure(
            winsock.Error(),
            NetworkProbeStage::WinsockStartup,
            true);
    }

    const SOCKET socketHandle = socket(
        ToNativeFamily(endpoint.family),
        SOCK_DGRAM,
        IPPROTO_UDP);
    if (socketHandle == INVALID_SOCKET)
    {
        const int error = WSAGetLastError();
        return MakeProbeFailure(
            error,
            NetworkProbeStage::SocketCreate,
            IsDefinitiveFamilySocketError(error));
    }

    NetworkEndpoint normalizedEndpoint = endpoint;
    normalizedEndpoint.host = parsedHost.host;
    SOCKADDR_STORAGE storage = {};
    int storageLength = 0;
    if (!BuildNativeEndpoint(
            normalizedEndpoint,
            &storage,
            &storageLength))
    {
        const int error = WSAGetLastError();
        closesocket(socketHandle);
        return MakeProbeFailure(
            error != 0 ? error : WSAEINVAL,
            NetworkProbeStage::NumericAddress,
            true);
    }

    NetworkFamilyProbeResult result;
    // UDP connect performs local address-family and route selection and sends
    // no packet. Only definitive local errors reject the session.
    if (connect(
            socketHandle,
            reinterpret_cast<const sockaddr*>(&storage),
            storageLength)
        == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        result = MakeProbeFailure(
            error,
            NetworkProbeStage::UdpRoute,
            IsDefinitiveRemoteRouteFailure(error));
    }
    closesocket(socketHandle);
    return result;
}

bool IsDefinitiveRemoteRouteFailure(int nativeError)
{
    return IsDefinitiveFamilySocketError(nativeError)
        || nativeError == WSAEADDRNOTAVAIL
        || nativeError == WSAENETUNREACH
        || nativeError == WSAEHOSTUNREACH;
}

const char* ProbeStageName(NetworkProbeStage stage)
{
    switch (stage)
    {
    case NetworkProbeStage::None:
        return "none";
    case NetworkProbeStage::WinsockStartup:
        return "WSAStartup";
    case NetworkProbeStage::SocketCreate:
        return "socket";
    case NetworkProbeStage::BindWildcard:
        return "bind_wildcard";
    case NetworkProbeStage::NumericAddress:
        return "numeric_address";
    case NetworkProbeStage::UdpRoute:
        return "udp_route";
    }
    return "unknown";
}

const char* FamilyName(NetworkFamily family)
{
    switch (family)
    {
    case NetworkFamily::IPv4:
        return "IPv4";
    case NetworkFamily::IPv6:
        return "IPv6";
    default:
        return "Unknown";
    }
}
} // namespace netplay::network
