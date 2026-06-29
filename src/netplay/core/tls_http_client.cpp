#include "netplay/core/tls_http_client.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "logger.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>

#if defined(EFZ_EMBEDDED_TLS)
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

namespace netplay::tls
{
namespace
{
#if defined(EFZ_EMBEDDED_TLS)
int ConnectTcpWithTimeout(
    mbedtls_net_context* context,
    const char* host,
    const char* port,
    uint32_t timeoutMs,
    std::string* outError)
{
    if (context == nullptr || host == nullptr || port == nullptr)
    {
        return MBEDTLS_ERR_NET_BAD_INPUT_DATA;
    }

    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 0), &wsaData) != 0)
    {
        if (outError != nullptr)
        {
            *outError = "WSAStartup failed";
        }
        return MBEDTLS_ERR_NET_SOCKET_FAILED;
    }

    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(host, port, &hints, &addresses) != 0)
    {
        WSACleanup();
        if (outError != nullptr)
        {
            *outError = "getaddrinfo failed";
        }
        return MBEDTLS_ERR_NET_UNKNOWN_HOST;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs != 0 ? timeoutMs : 1u);
    int result = MBEDTLS_ERR_NET_CONNECT_FAILED;
    int lastSocketError = 0;

    for (const addrinfo* address = addresses;
         address != nullptr;
         address = address->ai_next)
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            lastSocketError = WSAETIMEDOUT;
            break;
        }

        SOCKET socketHandle = socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol);
        if (socketHandle == INVALID_SOCKET)
        {
            lastSocketError = WSAGetLastError();
            result = MBEDTLS_ERR_NET_SOCKET_FAILED;
            continue;
        }

        u_long nonBlocking = 1;
        if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) != 0)
        {
            lastSocketError = WSAGetLastError();
            closesocket(socketHandle);
            result = MBEDTLS_ERR_NET_SOCKET_FAILED;
            continue;
        }

        int connectResult = connect(
            socketHandle,
            address->ai_addr,
            static_cast<int>(address->ai_addrlen));
        if (connectResult == SOCKET_ERROR)
        {
            lastSocketError = WSAGetLastError();
            if (lastSocketError != WSAEWOULDBLOCK
                && lastSocketError != WSAEINPROGRESS
                && lastSocketError != WSAEALREADY
                && lastSocketError != WSAEINVAL)
            {
                closesocket(socketHandle);
                continue;
            }

            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
            {
                lastSocketError = WSAETIMEDOUT;
                closesocket(socketHandle);
                break;
            }

            fd_set writeSet;
            fd_set errorSet;
            FD_ZERO(&writeSet);
            FD_ZERO(&errorSet);
            FD_SET(socketHandle, &writeSet);
            FD_SET(socketHandle, &errorSet);
            timeval timeout = {};
            timeout.tv_sec = static_cast<long>(remaining.count() / 1000);
            timeout.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
            const int selected = select(0, nullptr, &writeSet, &errorSet, &timeout);
            if (selected <= 0)
            {
                lastSocketError = selected == 0 ? WSAETIMEDOUT : WSAGetLastError();
                closesocket(socketHandle);
                if (selected == 0)
                {
                    break;
                }
                continue;
            }

            int socketError = 0;
            int socketErrorSize = sizeof(socketError);
            if (getsockopt(
                    socketHandle,
                    SOL_SOCKET,
                    SO_ERROR,
                    reinterpret_cast<char*>(&socketError),
                    &socketErrorSize) != 0
                || socketError != 0)
            {
                lastSocketError = socketError != 0 ? socketError : WSAGetLastError();
                closesocket(socketHandle);
                continue;
            }
        }

        u_long blocking = 0;
        if (ioctlsocket(socketHandle, FIONBIO, &blocking) != 0)
        {
            lastSocketError = WSAGetLastError();
            closesocket(socketHandle);
            result = MBEDTLS_ERR_NET_SOCKET_FAILED;
            continue;
        }

        context->fd = static_cast<int>(socketHandle);
        result = 0;
        break;
    }

    freeaddrinfo(addresses);
    if (result != 0)
    {
        WSACleanup();
        if (outError != nullptr)
        {
            char errorText[96] = {};
            std::snprintf(
                errorText,
                sizeof(errorText),
                "TCP connect failed/timeout (WSA=%d timeoutMs=%lu)",
                lastSocketError,
                static_cast<unsigned long>(timeoutMs));
            *outError = errorText;
        }
    }
    // On success the matching WSACleanup occurs after mbedtls_net_free().
    return result;
}
#endif

constexpr const char kIsrgRootX1Pem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "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\n"
    "-----END CERTIFICATE-----\n";

struct ParsedUrl
{
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
};

std::string ToLower(std::string value)
{
    for (char& c : value)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool ParseUrl(const std::string& url, ParsedUrl* out)
{
    if (out == nullptr)
    {
        return false;
    }

    const size_t schemeSep = url.find("://");
    if (schemeSep == std::string::npos)
    {
        return false;
    }

    out->scheme = ToLower(url.substr(0, schemeSep));
    if (out->scheme != "https")
    {
        return false;
    }

    const size_t hostStart = schemeSep + 3;
    size_t pathStart = url.find('/', hostStart);
    const std::string hostPort = pathStart == std::string::npos
        ? url.substr(hostStart)
        : url.substr(hostStart, pathStart - hostStart);

    if (hostPort.empty())
    {
        return false;
    }

    size_t colon = std::string::npos;
    if (!hostPort.empty() && hostPort.front() == '[')
    {
        const size_t close = hostPort.find(']');
        if (close == std::string::npos)
        {
            return false;
        }
        out->host = hostPort.substr(1, close - 1);
        if (close + 1 < hostPort.size() && hostPort[close + 1] == ':')
        {
            out->port = hostPort.substr(close + 2);
        }
    }
    else
    {
        colon = hostPort.find(':');
        if (colon != std::string::npos)
        {
            out->host = hostPort.substr(0, colon);
            out->port = hostPort.substr(colon + 1);
        }
        else
        {
            out->host = hostPort;
        }
    }

    if (out->host.empty())
    {
        return false;
    }

    if (out->port.empty())
    {
        out->port = "443";
    }

    out->path = (pathStart == std::string::npos) ? "/" : url.substr(pathStart);
    return !out->path.empty();
}

std::string MbedErrorToString(int code)
{
#if defined(EFZ_EMBEDDED_TLS)
    char buffer[256] = {};
    mbedtls_strerror(code, buffer, sizeof(buffer));
    return std::string(buffer);
#else
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "mbedtls_err_%d", code);
    return std::string(buffer);
#endif
}

bool StartsWithCaseInsensitive(const std::string& value, const char* prefix)
{
    const size_t n = std::strlen(prefix);
    if (value.size() < n)
    {
        return false;
    }
    for (size_t i = 0; i < n; ++i)
    {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b)
        {
            return false;
        }
    }
    return true;
}

bool DecodeChunkedBody(const std::string& input, std::string* output)
{
    if (output == nullptr)
    {
        return false;
    }
    output->clear();

    size_t cursor = 0;
    while (cursor < input.size())
    {
        const size_t lineEnd = input.find("\r\n", cursor);
        if (lineEnd == std::string::npos)
        {
            return false;
        }

        std::string sizeText = input.substr(cursor, lineEnd - cursor);
        const size_t extPos = sizeText.find(';');
        if (extPos != std::string::npos)
        {
            sizeText.resize(extPos);
        }

        char* endPtr = nullptr;
        const unsigned long chunkSize = std::strtoul(sizeText.c_str(), &endPtr, 16);
        if (endPtr == sizeText.c_str())
        {
            return false;
        }

        cursor = lineEnd + 2;
        if (chunkSize == 0)
        {
            return true;
        }

        if (cursor + chunkSize + 2 > input.size())
        {
            return false;
        }

        output->append(input, cursor, chunkSize);
        cursor += chunkSize;
        if (input.compare(cursor, 2, "\r\n") != 0)
        {
            return false;
        }
        cursor += 2;
    }

    return true;
}

bool ParseHttpResponse(const std::string& response, std::string* outBody, std::string* outError)
{
    if (outBody == nullptr || outError == nullptr)
    {
        return false;
    }

    const size_t headerEnd = response.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
        *outError = "invalid HTTP response";
        return false;
    }

    const std::string headers = response.substr(0, headerEnd);
    std::string body = response.substr(headerEnd + 4);

    const size_t lineEnd = headers.find("\r\n");
    const std::string statusLine = (lineEnd == std::string::npos) ? headers : headers.substr(0, lineEnd);
    int statusCode = 0;
    if (sscanf_s(statusLine.c_str(), "HTTP/%*d.%*d %d", &statusCode) != 1)
    {
        *outError = "failed to parse HTTP status";
        return false;
    }

    std::string lowerHeaders = ToLower(headers);
    if (lowerHeaders.find("transfer-encoding: chunked") != std::string::npos)
    {
        std::string decoded;
        if (!DecodeChunkedBody(body, &decoded))
        {
            *outError = "failed to decode chunked response";
            return false;
        }
        body.swap(decoded);
    }

    if (statusCode < 200 || statusCode >= 300)
    {
        char text[128] = {};
        std::snprintf(text, sizeof(text), "HTTP status %d body='%s'", statusCode, body.c_str());
        *outError = text;
        return false;
    }

    *outBody = body;
    return true;
}
} // namespace

bool IsAvailable()
{
#if defined(EFZ_EMBEDDED_TLS)
    return true;
#else
    return false;
#endif
}

bool HttpGet(
    const std::string& url,
    bool verifyPeer,
    uint32_t connectTimeoutMs,
    uint32_t receiveTimeoutMs,
    std::string* outBody,
    std::string* outError)
{
    if (outBody == nullptr || outError == nullptr)
    {
        return false;
    }
    outBody->clear();
    outError->clear();

#if !defined(EFZ_EMBEDDED_TLS)
    *outError = "embedded TLS backend not compiled";
    return false;
#else
    ParsedUrl parsed;
    if (!ParseUrl(url, &parsed))
    {
        *outError = "unsupported URL";
        return false;
    }

    mbedtls_net_context serverFd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctrDrbg;
    mbedtls_entropy_context entropy;
    mbedtls_x509_crt cacert;

    mbedtls_net_init(&serverFd);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctrDrbg);
    mbedtls_entropy_init(&entropy);
    mbedtls_x509_crt_init(&cacert);

    bool ok = false;
    bool winsockConnected = false;
    std::string request;
    size_t writeOffset = 0;
    std::string rawResponse;
    unsigned char buffer[2048] = {};
    const char* pers = "efz_netplay_mod_tls";
    int ret = mbedtls_ctr_drbg_seed(
        &ctrDrbg,
        mbedtls_entropy_func,
        &entropy,
        reinterpret_cast<const unsigned char*>(pers),
        std::strlen(pers));
    if (ret != 0)
    {
        *outError = "mbedtls_ctr_drbg_seed: " + MbedErrorToString(ret);
        goto cleanup;
    }

    ret = ConnectTcpWithTimeout(
        &serverFd,
        parsed.host.c_str(),
        parsed.port.c_str(),
        connectTimeoutMs,
        outError);
    if (ret != 0)
    {
        if (outError->empty())
        {
            *outError = "TCP connect: " + MbedErrorToString(ret);
        }
        goto cleanup;
    }
    winsockConnected = true;

    ret = mbedtls_ssl_config_defaults(
        &conf,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
    {
        *outError = "mbedtls_ssl_config_defaults: " + MbedErrorToString(ret);
        goto cleanup;
    }

    if (verifyPeer)
    {
        ret = mbedtls_x509_crt_parse(
            &cacert,
            reinterpret_cast<const unsigned char*>(kIsrgRootX1Pem),
            sizeof(kIsrgRootX1Pem));
        if (ret < 0)
        {
            *outError = "mbedtls_x509_crt_parse: " + MbedErrorToString(ret);
            goto cleanup;
        }

        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&conf, &cacert, nullptr);
    }
    else
    {
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctrDrbg);
    mbedtls_ssl_conf_read_timeout(&conf, receiveTimeoutMs);
    mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3); // TLS 1.2

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0)
    {
        *outError = "mbedtls_ssl_setup: " + MbedErrorToString(ret);
        goto cleanup;
    }

    ret = mbedtls_ssl_set_hostname(&ssl, parsed.host.c_str());
    if (ret != 0)
    {
        *outError = "mbedtls_ssl_set_hostname: " + MbedErrorToString(ret);
        goto cleanup;
    }

    mbedtls_ssl_set_bio(&ssl, &serverFd, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            *outError = "mbedtls_ssl_handshake: " + MbedErrorToString(ret);
            goto cleanup;
        }
    }

    if (verifyPeer)
    {
        const uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
        if (flags != 0)
        {
            char verifyBuf[512] = {};
            mbedtls_x509_crt_verify_info(verifyBuf, sizeof(verifyBuf), "", flags);
            *outError = std::string("certificate verification failed: ") + verifyBuf;
            goto cleanup;
        }
    }

    request.clear();
    request.reserve(parsed.path.size() + parsed.host.size() + 128);
    request += "GET ";
    request += parsed.path;
    request += " HTTP/1.1\r\nHost: ";
    request += parsed.host;
    request += "\r\nUser-Agent: EFZNetplayMod/1.0\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n";

    writeOffset = 0;
    while (writeOffset < request.size())
    {
        ret = mbedtls_ssl_write(
            &ssl,
            reinterpret_cast<const unsigned char*>(request.data() + writeOffset),
            request.size() - writeOffset);
        if (ret > 0)
        {
            writeOffset += static_cast<size_t>(ret);
            continue;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            *outError = "mbedtls_ssl_write: " + MbedErrorToString(ret);
            goto cleanup;
        }
    }

    rawResponse.clear();
    rawResponse.reserve(4096);
    for (;;)
    {
        ret = mbedtls_ssl_read(&ssl, buffer, sizeof(buffer));
        if (ret > 0)
        {
            rawResponse.append(reinterpret_cast<const char*>(buffer), reinterpret_cast<const char*>(buffer) + ret);
            continue;
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
            break;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_TIMEOUT)
        {
            *outError = "mbedtls_ssl_read: timeout";
            goto cleanup;
        }

        *outError = "mbedtls_ssl_read: " + MbedErrorToString(ret);
        goto cleanup;
    }

    if (!ParseHttpResponse(rawResponse, outBody, outError))
    {
        goto cleanup;
    }

    ok = true;

cleanup:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_net_free(&serverFd);
    if (winsockConnected)
    {
        WSACleanup();
    }
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctrDrbg);
    mbedtls_entropy_free(&entropy);
    return ok;
#endif
}
} // namespace netplay::tls
