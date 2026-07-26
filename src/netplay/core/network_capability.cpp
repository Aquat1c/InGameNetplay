#include "netplay/core/network_capability.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>

#include <cstddef>
#include <cstring>
#include <vector>

#include <windows.h>

namespace netplay::network
{
namespace
{
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

    bool IsActive() const { return active_; }
    int Error() const { return error_; }

private:
    bool active_ = false;
    int error_ = 0;
};

int NativeFamily(NetworkFamily family)
{
    return family == NetworkFamily::IPv6 ? AF_INET6 : AF_INET;
}

constexpr DWORD kMaximumAddressListBytes = 64u * 1024u;

bool IsDefinitiveSocketFailure(int error)
{
    return error == WSAEAFNOSUPPORT
        || error == WSAEPROTONOSUPPORT
        || error == WSAEADDRNOTAVAIL;
}

int ScopeRank(LocalAddressScope scope)
{
    return static_cast<int>(scope);
}

void RecordScope(LocalFamilyCapability* capability, LocalAddressScope scope)
{
    if (capability != nullptr
        && ScopeRank(scope) > ScopeRank(capability->strongestAddressScope))
    {
        capability->strongestAddressScope = scope;
    }
}

bool IsIpv4PrivateOrCarrierGradeNat(uint32_t address)
{
    const uint8_t first = static_cast<uint8_t>(address >> 24u);
    const uint8_t second = static_cast<uint8_t>(address >> 16u);
    return first == 10u
        || (first == 100u && second >= 64u && second <= 127u)
        || (first == 172u && second >= 16u && second <= 31u)
        || (first == 192u && second == 168u);
}

LocalAddressScope ClassifySockaddr(const sockaddr* address, int length)
{
    if (address == nullptr || length < static_cast<int>(sizeof(short)))
    {
        return LocalAddressScope::None;
    }

    if (address->sa_family == AF_INET
        && length >= static_cast<int>(sizeof(sockaddr_in)))
    {
        const uint32_t value = ntohl(
            reinterpret_cast<const sockaddr_in*>(address)->sin_addr.S_un.S_addr);
        const uint8_t first = static_cast<uint8_t>(value >> 24u);
        const uint8_t second = static_cast<uint8_t>(value >> 16u);
        if (value == 0u)
        {
            return LocalAddressScope::None;
        }
        if (first == 127u)
        {
            return LocalAddressScope::Loopback;
        }
        if (first == 169u && second == 254u)
        {
            return LocalAddressScope::LinkLocal;
        }
        if (IsIpv4PrivateOrCarrierGradeNat(value))
        {
            return LocalAddressScope::PrivateOrUniqueLocal;
        }
        // A locally selected non-special unicast IPv4 source can still be
        // NATed, but is routable enough to attempt public discovery.
        return (first > 0u && first < 224u)
            ? LocalAddressScope::GlobalUnicast
            : LocalAddressScope::None;
    }

    if (address->sa_family == AF_INET6
        && length >= static_cast<int>(sizeof(sockaddr_in6)))
    {
        const auto* bytes = reinterpret_cast<const uint8_t*>(
            &reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr);
        bool allZero = true;
        bool loopback = true;
        for (size_t index = 0; index < 16; ++index)
        {
            allZero = allZero && bytes[index] == 0u;
            loopback = loopback
                && (index == 15u ? bytes[index] == 1u : bytes[index] == 0u);
        }
        if (allZero)
        {
            return LocalAddressScope::None;
        }
        if (loopback)
        {
            return LocalAddressScope::Loopback;
        }
        if (bytes[0] == 0xfeu && (bytes[1] & 0xc0u) == 0x80u)
        {
            return LocalAddressScope::LinkLocal;
        }
        if ((bytes[0] & 0xfeu) == 0xfcu)
        {
            return LocalAddressScope::PrivateOrUniqueLocal;
        }
        return (bytes[0] & 0xe0u) == 0x20u
            ? LocalAddressScope::GlobalUnicast
            : LocalAddressScope::None;
    }
    return LocalAddressScope::None;
}

void QueryBindableAddresses(
    SOCKET socketHandle,
    LocalFamilyCapability* capability)
{
    if (socketHandle == INVALID_SOCKET || capability == nullptr)
    {
        return;
    }

    DWORD bytesRequired = 0;
    const int firstResult = WSAIoctl(
        socketHandle, SIO_ADDRESS_LIST_QUERY, nullptr, 0, nullptr, 0,
        &bytesRequired, nullptr, nullptr);
    const int firstError = firstResult == SOCKET_ERROR ? WSAGetLastError() : 0;
    const size_t headerSize = offsetof(SOCKET_ADDRESS_LIST, Address);
    if (firstResult != SOCKET_ERROR || firstError != WSAEFAULT
        || bytesRequired < headerSize
        || bytesRequired > kMaximumAddressListBytes)
    {
        capability->addressListNativeError =
            bytesRequired > kMaximumAddressListBytes ? WSAEMSGSIZE : firstError;
        return;
    }

    std::vector<uint8_t> bytes(bytesRequired);
    DWORD bytesReturned = 0;
    if (WSAIoctl(
            socketHandle, SIO_ADDRESS_LIST_QUERY, nullptr, 0, bytes.data(),
            static_cast<DWORD>(bytes.size()), &bytesReturned, nullptr, nullptr)
        == SOCKET_ERROR)
    {
        capability->addressListNativeError = WSAGetLastError();
        return;
    }
    if (bytesReturned < headerSize)
    {
        capability->addressListNativeError = WSAEFAULT;
        return;
    }

    const auto* list =
        reinterpret_cast<const SOCKET_ADDRESS_LIST*>(bytes.data());
    const size_t maximumCount =
        (bytesReturned - headerSize) / sizeof(SOCKET_ADDRESS);
    if (list->iAddressCount < 0
        || static_cast<size_t>(list->iAddressCount) > maximumCount)
    {
        capability->addressListNativeError = WSAEFAULT;
        return;
    }

    capability->addressListQueried = true;
    for (int index = 0; index < list->iAddressCount; ++index)
    {
        const SOCKET_ADDRESS& entry = list->Address[index];
        if (entry.lpSockaddr == nullptr
            || entry.iSockaddrLength < static_cast<int>(sizeof(short))
            || entry.lpSockaddr->sa_family != NativeFamily(capability->family))
        {
            continue;
        }
        const LocalAddressScope scope = ClassifySockaddr(
            entry.lpSockaddr, entry.iSockaddrLength);
        if (scope != LocalAddressScope::None)
        {
            ++capability->bindableAddressCount;
            RecordScope(capability, scope);
        }
    }
}

bool BuildRouteWitness(
    NetworkFamily family,
    SOCKADDR_STORAGE* outStorage,
    int* outLength)
{
    if (outStorage == nullptr || outLength == nullptr)
    {
        return false;
    }

    SOCKADDR_STORAGE storage = {};
    if (family == NetworkFamily::IPv4)
    {
        sockaddr_in destination = {};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(9u);
        // UDP connect only selects a local route/source and sends no payload.
        // Use a real global unicast destination: documentation prefixes are
        // often intentionally black-holed and can falsely mark a usable
        // family as unreachable before a Host attempt.
        destination.sin_addr.s_addr = htonl(0x01010101u);
        std::memcpy(&storage, &destination, sizeof(destination));
        *outLength = sizeof(destination);
    }
    else if (family == NetworkFamily::IPv6)
    {
        sockaddr_in6 destination = {};
        destination.sin6_family = AF_INET6;
        destination.sin6_port = htons(9u);
        const uint8_t witness[16] = {
            0x26u, 0x06u, 0x47u, 0x00u, 0x47u, 0x00u, 0u, 0u,
            0u, 0u, 0u, 0u, 0u, 0u, 0x11u, 0x11u,
        };
        std::memcpy(&destination.sin6_addr, witness, sizeof(witness));
        std::memcpy(&storage, &destination, sizeof(destination));
        *outLength = sizeof(destination);
    }
    else
    {
        return false;
    }

    *outStorage = storage;
    return true;
}

void ProbeRoute(NetworkFamily family, LocalFamilyCapability* capability)
{
    if (capability == nullptr)
    {
        return;
    }

    const SOCKET socketHandle = socket(
        NativeFamily(family), SOCK_DGRAM, IPPROTO_UDP);
    capability->routeProbeAttempted = true;
    if (socketHandle == INVALID_SOCKET)
    {
        capability->routeNativeError = WSAGetLastError();
        capability->routeDefinitelyUnavailable =
            IsDefinitiveSocketFailure(capability->routeNativeError);
        return;
    }

    SOCKADDR_STORAGE destination = {};
    int destinationLength = 0;
    if (!BuildRouteWitness(family, &destination, &destinationLength))
    {
        capability->routeNativeError = WSAEINVAL;
        closesocket(socketHandle);
        return;
    }
    if (connect(
            socketHandle,
            reinterpret_cast<const sockaddr*>(&destination),
            destinationLength)
        == SOCKET_ERROR)
    {
        capability->routeNativeError = WSAGetLastError();
        capability->routeDefinitelyUnavailable =
            IsDefinitiveRemoteRouteFailure(capability->routeNativeError);
        closesocket(socketHandle);
        return;
    }

    SOCKADDR_STORAGE source = {};
    int sourceLength = sizeof(source);
    if (getsockname(
            socketHandle,
            reinterpret_cast<sockaddr*>(&source),
            &sourceLength)
        == SOCKET_ERROR)
    {
        capability->routeNativeError = WSAGetLastError();
        closesocket(socketHandle);
        return;
    }
    closesocket(socketHandle);

    capability->routeAvailable = true;
    capability->routeSourceScope = ClassifySockaddr(
        reinterpret_cast<const sockaddr*>(&source), sourceLength);
}

void SetListenerFailure(
    LocalFamilyCapability* capability,
    int error,
    NetworkProbeStage stage,
    bool unavailable)
{
    capability->listenerProbe.unavailable = unavailable;
    capability->listenerProbe.nativeError = error;
    capability->listenerProbe.stage = stage;
    capability->state = unavailable
        ? LocalFamilyCapabilityState::HardUnavailable
        : LocalFamilyCapabilityState::Unknown;
}

LocalFamilyCapability ScanFamily(
    NetworkFamily family,
    const WinsockLease& winsock)
{
    LocalFamilyCapability capability;
    capability.family = family;
    if (!winsock.IsActive())
    {
        SetListenerFailure(
            &capability, winsock.Error(), NetworkProbeStage::WinsockStartup,
            // A transient process-wide Winsock initialization failure must
            // not suppress public discovery or force a family switch. The
            // real Revival listener remains the final authority.
            false);
        return capability;
    }

    const SOCKET listenerSocket = socket(
        NativeFamily(family), SOCK_DGRAM, IPPROTO_UDP);
    if (listenerSocket == INVALID_SOCKET)
    {
        const int error = WSAGetLastError();
        SetListenerFailure(
            &capability, error, NetworkProbeStage::SocketCreate,
            IsDefinitiveSocketFailure(error));
        return capability;
    }

    int bindResult = SOCKET_ERROR;
    if (family == NetworkFamily::IPv4)
    {
        sockaddr_in any = {};
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = htonl(INADDR_ANY);
        bindResult = bind(
            listenerSocket, reinterpret_cast<const sockaddr*>(&any),
            sizeof(any));
    }
    else
    {
        sockaddr_in6 any = {};
        any.sin6_family = AF_INET6;
        bindResult = bind(
            listenerSocket, reinterpret_cast<const sockaddr*>(&any),
            sizeof(any));
    }
    if (bindResult == SOCKET_ERROR)
    {
        const int error = WSAGetLastError();
        closesocket(listenerSocket);
        SetListenerFailure(
            &capability, error, NetworkProbeStage::BindWildcard,
            IsDefinitiveSocketFailure(error));
        return capability;
    }

    QueryBindableAddresses(listenerSocket, &capability);
    closesocket(listenerSocket);
    ProbeRoute(family, &capability);

    if (capability.routeAvailable)
    {
        if (capability.routeSourceScope == LocalAddressScope::None)
        {
            // A successful UDP connect with an undecodable/unspecified local
            // source is inconclusive. Do not turn a malformed observation
            // into a false public-family rejection.
            capability.state = LocalFamilyCapabilityState::Unknown;
            return capability;
        }
        const bool usableSource = family == NetworkFamily::IPv4
            ? capability.routeSourceScope == LocalAddressScope::GlobalUnicast
                || capability.routeSourceScope
                    == LocalAddressScope::PrivateOrUniqueLocal
            : capability.routeSourceScope == LocalAddressScope::GlobalUnicast;
        capability.state = usableSource
            ? LocalFamilyCapabilityState::Routable
            : LocalFamilyCapabilityState::LocalOnly;
    }
    else if (capability.routeDefinitelyUnavailable
             || capability.strongestAddressScope == LocalAddressScope::Loopback
             || capability.strongestAddressScope == LocalAddressScope::LinkLocal)
    {
        capability.state = LocalFamilyCapabilityState::LocalOnly;
    }
    else
    {
        capability.state = LocalFamilyCapabilityState::Unknown;
    }
    return capability;
}
} // namespace

LocalNetworkCapabilitySnapshot ScanLocalNetworkCapabilities()
{
    WinsockLease winsock;
    LocalNetworkCapabilitySnapshot snapshot;
    snapshot.ipv4 = ScanFamily(NetworkFamily::IPv4, winsock);
    snapshot.ipv6 = ScanFamily(NetworkFamily::IPv6, winsock);
    snapshot.completedTick = GetTickCount();
    return snapshot;
}

const LocalFamilyCapability& GetFamilyCapability(
    const LocalNetworkCapabilitySnapshot& snapshot,
    NetworkFamily family)
{
    return family == NetworkFamily::IPv6 ? snapshot.ipv6 : snapshot.ipv4;
}

bool CanAttemptGlobalPublicDiscovery(
    const LocalFamilyCapability& capability)
{
    return capability.state == LocalFamilyCapabilityState::Routable
        || capability.state == LocalFamilyCapabilityState::Unknown;
}

const char* LocalAddressScopeName(LocalAddressScope scope)
{
    switch (scope)
    {
    case LocalAddressScope::None: return "none";
    case LocalAddressScope::Loopback: return "loopback";
    case LocalAddressScope::LinkLocal: return "link_local";
    case LocalAddressScope::PrivateOrUniqueLocal: return "private_or_ula";
    case LocalAddressScope::GlobalUnicast: return "global_unicast";
    }
    return "unknown";
}

const char* LocalFamilyCapabilityStateName(
    LocalFamilyCapabilityState state)
{
    switch (state)
    {
    case LocalFamilyCapabilityState::Unknown: return "unknown";
    case LocalFamilyCapabilityState::HardUnavailable:
        return "hard_unavailable";
    case LocalFamilyCapabilityState::LocalOnly: return "local_only";
    case LocalFamilyCapabilityState::Routable: return "routable";
    }
    return "unknown";
}
} // namespace netplay::network
