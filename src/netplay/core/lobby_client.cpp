#include "netplay/core/lobby_client.h"
#include "netplay/core/tls_http_client.h"

#include "logger.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#ifdef EFZ_XP_COMPAT
// The Windows 7.1A SDK (v141_xp) has type-name conflicts when both
// winhttp.h and wininet.h are included in the same translation unit.
// Since all WinINet APIs are resolved at runtime via GetProcAddress we
// only need the function declarations and a handful of constants.

extern "C" {
    HINTERNET WINAPI InternetOpenW(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
    BOOL      WINAPI InternetSetOptionW(HINTERNET, DWORD, LPVOID, DWORD);
    HINTERNET WINAPI InternetOpenUrlW(HINTERNET, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD_PTR);
    BOOL      WINAPI InternetReadFile(HINTERNET, LPVOID, DWORD, LPDWORD);
    BOOL      WINAPI InternetCloseHandle(HINTERNET);
    BOOL      WINAPI InternetGetLastResponseInfoA(LPDWORD, LPSTR, LPDWORD);
}

#ifndef INTERNET_OPEN_TYPE_PRECONFIG
#define INTERNET_OPEN_TYPE_PRECONFIG        0
#endif
#ifndef INTERNET_OPTION_CONNECT_TIMEOUT
#define INTERNET_OPTION_CONNECT_TIMEOUT     2
#endif
#ifndef INTERNET_OPTION_SEND_TIMEOUT
#define INTERNET_OPTION_SEND_TIMEOUT        5
#endif
#ifndef INTERNET_OPTION_RECEIVE_TIMEOUT
#define INTERNET_OPTION_RECEIVE_TIMEOUT     6
#endif
#ifndef INTERNET_FLAG_RELOAD
#define INTERNET_FLAG_RELOAD                0x80000000
#endif
#ifndef INTERNET_FLAG_NO_CACHE_WRITE
#define INTERNET_FLAG_NO_CACHE_WRITE        0x04000000
#endif
#ifndef INTERNET_FLAG_PRAGMA_NOCACHE
#define INTERNET_FLAG_PRAGMA_NOCACHE        0x00000010
#endif
#ifndef INTERNET_FLAG_SECURE
#define INTERNET_FLAG_SECURE                0x00800000
#endif

#ifndef ERROR_INTERNET_TIMEOUT
#define ERROR_INTERNET_TIMEOUT              12002
#define ERROR_INTERNET_NAME_NOT_RESOLVED    12007
#define ERROR_INTERNET_DECODING_FAILED      12019
#define ERROR_INTERNET_CANNOT_CONNECT       12029
#define ERROR_INTERNET_CONNECTION_ABORTED   12030
#define ERROR_INTERNET_CONNECTION_RESET     12031
#define ERROR_INTERNET_SEC_CERT_DATE_INVALID 12037
#define ERROR_INTERNET_SEC_CERT_CN_INVALID  12038
#define ERROR_INTERNET_HTTP_TO_HTTPS_ON_REDIR 12039
#define ERROR_INTERNET_HTTPS_TO_HTTP_ON_REDIR 12040
#define ERROR_INTERNET_CLIENT_AUTH_CERT_NEEDED 12044
#define ERROR_INTERNET_INVALID_CA           12045
#define ERROR_INTERNET_SEC_CERT_ERRORS      12055
#endif

#else // !EFZ_XP_COMPAT
#include <wininet.h>
#endif

namespace netplay::lobby
{
namespace
{
constexpr const char* kConcertoHostUtf8 = "concerto-mbaacc.shib.live";
constexpr const wchar_t* kConcertoHost = L"concerto-mbaacc.shib.live";
constexpr INTERNET_PORT kConcertoPort = INTERNET_DEFAULT_HTTPS_PORT;
constexpr DWORD kConnectTimeoutMs = 8000;
constexpr DWORD kReceiveTimeoutMs = 12000;
constexpr DWORD kPollIntervalMs = 1000;
std::atomic<int> g_lobbyHttpBackend{ -1 }; // -1 unknown, 0 none, 1 WinHTTP, 2 WinINet, 3 EmbeddedTLS

struct LobbyEndpointConfig
{
    bool forceWinInet = false;
    bool forceEmbeddedTls = false;
    bool tlsVerify = false;
    bool hasBaseUrlOverride = false;
    std::string baseUrl;
    bool hasProxyBaseUrl = false;
    std::string proxyBaseUrl;
};

std::string TrimAscii(std::string value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0)
    {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }

    return value.substr(begin, end - begin);
}

std::string ResolveRevivalIniPath()
{
    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, static_cast<DWORD>(std::size(exePath))) == 0)
    {
        return "EfzRevival.ini";
    }

    std::string iniPath = exePath;
    const size_t sep = iniPath.find_last_of("\\/");
    if (sep == std::string::npos)
    {
        return "EfzRevival.ini";
    }

    iniPath.resize(sep + 1);
    iniPath += "EfzRevival.ini";
    return iniPath;
}

bool ParseBoolValue(const std::string& text, bool* out)
{
    if (out == nullptr)
    {
        return false;
    }

    std::string normalized = TrimAscii(text);
    for (char& c : normalized)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on")
    {
        *out = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off")
    {
        *out = false;
        return true;
    }
    return false;
}

std::string ReadEnvironmentString(const char* name)
{
    if (name == nullptr || name[0] == '\0')
    {
        return std::string();
    }

    char buffer[1024] = {};
    const DWORD size = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (size == 0 || size >= static_cast<DWORD>(std::size(buffer)))
    {
        return std::string();
    }
    return std::string(buffer, buffer + size);
}

bool HasHttpScheme(const std::string& value)
{
    if (value.size() < 8)
    {
        return false;
    }

    auto startsWithIgnoreCase = [&](const char* prefix) -> bool
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
    };

    return startsWithIgnoreCase("http://") || startsWithIgnoreCase("https://");
}

bool HasHttpsScheme(const std::string& value)
{
    if (value.size() < 8)
    {
        return false;
    }
    const char* prefix = "https://";
    for (size_t i = 0; i < 8; ++i)
    {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
        if (a != prefix[i])
        {
            return false;
        }
    }
    return true;
}

std::string NormalizeBaseUrl(std::string value)
{
    value = TrimAscii(std::move(value));
    while (!value.empty() && value.back() == '/')
    {
        value.pop_back();
    }
    return value;
}

struct WindowsVersionInfo
{
    DWORD major = 0;
    DWORD minor = 0;
    DWORD build = 0;
    bool valid = false;
};

WindowsVersionInfo QueryWindowsVersion()
{
    WindowsVersionInfo info = {};

    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll != nullptr)
    {
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtlGetVersion != nullptr)
        {
            OSVERSIONINFOEXW version = {};
            version.dwOSVersionInfoSize = sizeof(version);
            if (rtlGetVersion(&version) == 0)
            {
                info.major = version.dwMajorVersion;
                info.minor = version.dwMinorVersion;
                info.build = version.dwBuildNumber;
                info.valid = true;
                return info;
            }
        }
    }
    return info;
}

bool IsWindowsXpFamily(const WindowsVersionInfo& info)
{
    if (!info.valid)
    {
        return false;
    }

    // Windows 2000/XP/Server 2003 all report major version 5.
    return info.major <= 5;
}

const LobbyEndpointConfig& GetLobbyEndpointConfig()
{
    static LobbyEndpointConfig config;
    static bool initialized = false;
    if (initialized)
    {
        return config;
    }
    initialized = true;

    std::string iniBaseUrl;
    std::string iniProxyBaseUrl;
    bool iniForceWinInet = false;
    bool hasIniForceWinInet = false;
    bool iniForceEmbeddedTls = false;
    bool hasIniForceEmbeddedTls = false;
    bool iniTlsVerify = false;
    bool hasIniTlsVerify = false;

    const std::string iniPath = ResolveRevivalIniPath();
    if (GetFileAttributesA(iniPath.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        char baseUrlBuf[512] = {};
        (void)GetPrivateProfileStringA(
            "Lobby",
            "BaseUrl",
            "",
            baseUrlBuf,
            static_cast<DWORD>(std::size(baseUrlBuf)),
            iniPath.c_str());
        iniBaseUrl = TrimAscii(baseUrlBuf);

        char proxyBaseUrlBuf[512] = {};
        (void)GetPrivateProfileStringA(
            "Lobby",
            "ProxyBaseUrl",
            "",
            proxyBaseUrlBuf,
            static_cast<DWORD>(std::size(proxyBaseUrlBuf)),
            iniPath.c_str());
        iniProxyBaseUrl = TrimAscii(proxyBaseUrlBuf);

        char forceBuf[32] = {};
        (void)GetPrivateProfileStringA(
            "Lobby",
            "ForceWinInet",
            "",
            forceBuf,
            static_cast<DWORD>(std::size(forceBuf)),
            iniPath.c_str());
        if (forceBuf[0] != '\0')
        {
            hasIniForceWinInet = ParseBoolValue(forceBuf, &iniForceWinInet);
            if (!hasIniForceWinInet)
            {
                mod::Log(
                    "LobbySession: invalid Lobby.ForceWinInet='%s' in '%s' (expected 0/1/true/false)",
                    forceBuf,
                    iniPath.c_str());
            }
        }

        char forceEmbeddedBuf[32] = {};
        (void)GetPrivateProfileStringA(
            "Lobby",
            "ForceEmbeddedTls",
            "",
            forceEmbeddedBuf,
            static_cast<DWORD>(std::size(forceEmbeddedBuf)),
            iniPath.c_str());
        if (forceEmbeddedBuf[0] != '\0')
        {
            hasIniForceEmbeddedTls = ParseBoolValue(forceEmbeddedBuf, &iniForceEmbeddedTls);
            if (!hasIniForceEmbeddedTls)
            {
                mod::Log(
                    "LobbySession: invalid Lobby.ForceEmbeddedTls='%s' in '%s' (expected 0/1/true/false)",
                    forceEmbeddedBuf,
                    iniPath.c_str());
            }
        }

        char tlsVerifyBuf[32] = {};
        (void)GetPrivateProfileStringA(
            "Lobby",
            "TlsVerify",
            "",
            tlsVerifyBuf,
            static_cast<DWORD>(std::size(tlsVerifyBuf)),
            iniPath.c_str());
        if (tlsVerifyBuf[0] != '\0')
        {
            hasIniTlsVerify = ParseBoolValue(tlsVerifyBuf, &iniTlsVerify);
            if (!hasIniTlsVerify)
            {
                mod::Log(
                    "LobbySession: invalid Lobby.TlsVerify='%s' in '%s' (expected 0/1/true/false)",
                    tlsVerifyBuf,
                    iniPath.c_str());
            }
        }
    }

    const std::string envBaseUrl = TrimAscii(ReadEnvironmentString("EFZ_LOBBY_BASE_URL"));
    const std::string envProxyBaseUrl = TrimAscii(ReadEnvironmentString("EFZ_LOBBY_PROXY_BASE_URL"));
    const std::string envForceWinInetText = TrimAscii(ReadEnvironmentString("EFZ_LOBBY_FORCE_WININET"));
    const std::string envForceEmbeddedTlsText = TrimAscii(ReadEnvironmentString("EFZ_LOBBY_FORCE_EMBEDDED_TLS"));
    const std::string envTlsVerifyText = TrimAscii(ReadEnvironmentString("EFZ_LOBBY_TLS_VERIFY"));
    bool envForceWinInet = false;
    bool hasEnvForceWinInet = false;
    bool envForceEmbeddedTls = false;
    bool hasEnvForceEmbeddedTls = false;
    bool envTlsVerify = false;
    bool hasEnvTlsVerify = false;
    if (!envForceWinInetText.empty())
    {
        hasEnvForceWinInet = ParseBoolValue(envForceWinInetText, &envForceWinInet);
        if (!hasEnvForceWinInet)
        {
            mod::Log(
                "LobbySession: invalid EFZ_LOBBY_FORCE_WININET='%s' (expected 0/1/true/false)",
                envForceWinInetText.c_str());
        }
    }
    if (!envForceEmbeddedTlsText.empty())
    {
        hasEnvForceEmbeddedTls = ParseBoolValue(envForceEmbeddedTlsText, &envForceEmbeddedTls);
        if (!hasEnvForceEmbeddedTls)
        {
            mod::Log(
                "LobbySession: invalid EFZ_LOBBY_FORCE_EMBEDDED_TLS='%s' (expected 0/1/true/false)",
                envForceEmbeddedTlsText.c_str());
        }
    }
    if (!envTlsVerifyText.empty())
    {
        hasEnvTlsVerify = ParseBoolValue(envTlsVerifyText, &envTlsVerify);
        if (!hasEnvTlsVerify)
        {
            mod::Log(
                "LobbySession: invalid EFZ_LOBBY_TLS_VERIFY='%s' (expected 0/1/true/false)",
                envTlsVerifyText.c_str());
        }
    }

    const std::string selectedBaseUrl = !envBaseUrl.empty() ? envBaseUrl : iniBaseUrl;
    if (!selectedBaseUrl.empty())
    {
        config.baseUrl = NormalizeBaseUrl(selectedBaseUrl);
        if (HasHttpScheme(config.baseUrl))
        {
            config.hasBaseUrlOverride = true;
        }
        else
        {
            mod::Log(
                "LobbySession: ignoring lobby base URL without http/https scheme: '%s'",
                config.baseUrl.c_str());
            config.baseUrl.clear();
        }
    }

    const std::string selectedProxyBaseUrl = !envProxyBaseUrl.empty() ? envProxyBaseUrl : iniProxyBaseUrl;
    if (!selectedProxyBaseUrl.empty())
    {
        config.proxyBaseUrl = NormalizeBaseUrl(selectedProxyBaseUrl);
        if (HasHttpScheme(config.proxyBaseUrl))
        {
            config.hasProxyBaseUrl = true;
        }
        else
        {
            mod::Log(
                "LobbySession: ignoring proxy base URL without http/https scheme: '%s'",
                config.proxyBaseUrl.c_str());
            config.proxyBaseUrl.clear();
        }
    }

    if (hasEnvForceWinInet)
    {
        config.forceWinInet = envForceWinInet;
    }
    else if (hasIniForceWinInet)
    {
        config.forceWinInet = iniForceWinInet;
    }

    if (hasEnvForceEmbeddedTls)
    {
        config.forceEmbeddedTls = envForceEmbeddedTls;
    }
    else if (hasIniForceEmbeddedTls)
    {
        config.forceEmbeddedTls = iniForceEmbeddedTls;
    }

    if (hasEnvTlsVerify)
    {
        config.tlsVerify = envTlsVerify;
    }
    else if (hasIniTlsVerify)
    {
        config.tlsVerify = iniTlsVerify;
    }

    const bool hasExplicitBackendOverride =
        hasEnvForceWinInet || hasIniForceWinInet || hasEnvForceEmbeddedTls || hasIniForceEmbeddedTls;
    if (!hasExplicitBackendOverride)
    {
        const WindowsVersionInfo windowsVersion = QueryWindowsVersion();
        if (IsWindowsXpFamily(windowsVersion))
        {
            config.forceEmbeddedTls = true;
            mod::Log(
                "LobbySession: detected legacy Windows %lu.%lu build=%lu; auto enabling ForceEmbeddedTls=1",
                static_cast<unsigned long>(windowsVersion.major),
                static_cast<unsigned long>(windowsVersion.minor),
                static_cast<unsigned long>(windowsVersion.build));
        }
    }

    if (config.forceEmbeddedTls && config.forceWinInet)
    {
        mod::Log("LobbySession: both force flags set; ForceEmbeddedTls takes priority for HTTPS URLs");
    }

    mod::Log(
        "LobbySession: endpoint config override=%d baseUrl='%s' proxyOverride=%d proxyBaseUrl='%s' forceWinInet=%d forceEmbeddedTls=%d tlsVerify=%d embeddedAvailable=%d",
        config.hasBaseUrlOverride ? 1 : 0,
        config.hasBaseUrlOverride ? config.baseUrl.c_str() : "",
        config.hasProxyBaseUrl ? 1 : 0,
        config.hasProxyBaseUrl ? config.proxyBaseUrl.c_str() : "",
        config.forceWinInet ? 1 : 0,
        config.forceEmbeddedTls ? 1 : 0,
        config.tlsVerify ? 1 : 0,
        netplay::tls::IsAvailable() ? 1 : 0);

    return config;
}

std::string BuildLobbyRequestUrl(const std::string& path)
{
    const LobbyEndpointConfig& config = GetLobbyEndpointConfig();
    if (config.hasBaseUrlOverride)
    {
        return config.baseUrl + path;
    }
    return std::string("https://") + kConcertoHostUtf8 + path;
}

std::string BuildLobbyProxyRequestUrl(const std::string& path)
{
    const LobbyEndpointConfig& config = GetLobbyEndpointConfig();
    if (!config.hasProxyBaseUrl)
    {
        return std::string();
    }
    return config.proxyBaseUrl + path;
}

struct WinHttpApi
{
    HMODULE module = nullptr;
    bool initialized = false;
    bool available = false;

    decltype(&WinHttpOpen) Open = nullptr;
    decltype(&WinHttpSetTimeouts) SetTimeouts = nullptr;
    decltype(&WinHttpConnect) Connect = nullptr;
    decltype(&WinHttpOpenRequest) OpenRequest = nullptr;
    decltype(&WinHttpSendRequest) SendRequest = nullptr;
    decltype(&WinHttpReceiveResponse) ReceiveResponse = nullptr;
    decltype(&WinHttpQueryDataAvailable) QueryDataAvailable = nullptr;
    decltype(&WinHttpReadData) ReadData = nullptr;
    decltype(&WinHttpCloseHandle) CloseHandle = nullptr;
};

WinHttpApi& GetWinHttpApi()
{
    static WinHttpApi api;
    if (api.initialized)
    {
        return api;
    }
    api.initialized = true;

    api.module = LoadLibraryA("winhttp.dll");
    if (api.module == nullptr)
    {
        mod::Log("LobbySession: winhttp.dll unavailable (%lu); will try WinINet fallback", GetLastError());
        return api;
    }

    auto resolve = [&](auto* fn, const char* name) -> bool
    {
        *fn = reinterpret_cast<std::remove_reference_t<decltype(*fn)>>(GetProcAddress(api.module, name));
        if (*fn == nullptr)
        {
            mod::Log("LobbySession: missing WinHTTP symbol '%s' (%lu)", name, GetLastError());
            return false;
        }
        return true;
    };

    if (!resolve(&api.Open, "WinHttpOpen")
        || !resolve(&api.SetTimeouts, "WinHttpSetTimeouts")
        || !resolve(&api.Connect, "WinHttpConnect")
        || !resolve(&api.OpenRequest, "WinHttpOpenRequest")
        || !resolve(&api.SendRequest, "WinHttpSendRequest")
        || !resolve(&api.ReceiveResponse, "WinHttpReceiveResponse")
        || !resolve(&api.QueryDataAvailable, "WinHttpQueryDataAvailable")
        || !resolve(&api.ReadData, "WinHttpReadData")
        || !resolve(&api.CloseHandle, "WinHttpCloseHandle"))
    {
        FreeLibrary(api.module);
        api.module = nullptr;
        return api;
    }

    api.available = true;
    mod::Log("LobbySession: WinHTTP runtime API loaded");
    return api;
}

struct WinInetApi
{
    HMODULE module = nullptr;
    bool initialized = false;
    bool available = false;

    decltype(&InternetOpenW) Open = nullptr;
    decltype(&InternetSetOptionW) SetOption = nullptr;
    decltype(&InternetOpenUrlW) OpenUrl = nullptr;
    decltype(&InternetReadFile) ReadFile = nullptr;
    decltype(&InternetCloseHandle) CloseHandle = nullptr;
    decltype(&InternetGetLastResponseInfoA) GetLastResponseInfo = nullptr;
};

WinInetApi& GetWinInetApi()
{
    static WinInetApi api;
    if (api.initialized)
    {
        return api;
    }
    api.initialized = true;

    api.module = LoadLibraryA("wininet.dll");
    if (api.module == nullptr)
    {
        mod::Log("LobbySession: wininet.dll unavailable (%lu); fallback disabled", GetLastError());
        return api;
    }

    auto resolve = [&](auto* fn, const char* name) -> bool
    {
        *fn = reinterpret_cast<std::remove_reference_t<decltype(*fn)>>(GetProcAddress(api.module, name));
        if (*fn == nullptr)
        {
            mod::Log("LobbySession: missing WinINet symbol '%s' (%lu)", name, GetLastError());
            return false;
        }
        return true;
    };

    if (!resolve(&api.Open, "InternetOpenW")
        || !resolve(&api.SetOption, "InternetSetOptionW")
        || !resolve(&api.OpenUrl, "InternetOpenUrlW")
        || !resolve(&api.ReadFile, "InternetReadFile")
        || !resolve(&api.CloseHandle, "InternetCloseHandle"))
    {
        FreeLibrary(api.module);
        api.module = nullptr;
        return api;
    }

    api.GetLastResponseInfo = reinterpret_cast<decltype(api.GetLastResponseInfo)>(
        GetProcAddress(api.module, "InternetGetLastResponseInfoA"));

    api.available = true;
    mod::Log("LobbySession: WinINet runtime API loaded");
    return api;
}

const char* WinInetErrorName(DWORD code)
{
    switch (code)
    {
    case ERROR_INTERNET_TIMEOUT:
        return "ERROR_INTERNET_TIMEOUT";
    case ERROR_INTERNET_NAME_NOT_RESOLVED:
        return "ERROR_INTERNET_NAME_NOT_RESOLVED";
    case ERROR_INTERNET_CANNOT_CONNECT:
        return "ERROR_INTERNET_CANNOT_CONNECT";
    case ERROR_INTERNET_CONNECTION_ABORTED:
        return "ERROR_INTERNET_CONNECTION_ABORTED";
    case ERROR_INTERNET_CONNECTION_RESET:
        return "ERROR_INTERNET_CONNECTION_RESET";
    case ERROR_INTERNET_INVALID_CA:
        return "ERROR_INTERNET_INVALID_CA";
    case ERROR_INTERNET_SEC_CERT_CN_INVALID:
        return "ERROR_INTERNET_SEC_CERT_CN_INVALID";
    case ERROR_INTERNET_SEC_CERT_DATE_INVALID:
        return "ERROR_INTERNET_SEC_CERT_DATE_INVALID";
    case ERROR_INTERNET_SEC_CERT_ERRORS:
        return "ERROR_INTERNET_SEC_CERT_ERRORS";
    case ERROR_INTERNET_CLIENT_AUTH_CERT_NEEDED:
        return "ERROR_INTERNET_CLIENT_AUTH_CERT_NEEDED";
    case ERROR_INTERNET_HTTP_TO_HTTPS_ON_REDIR:
        return "ERROR_INTERNET_HTTP_TO_HTTPS_ON_REDIR";
    case ERROR_INTERNET_HTTPS_TO_HTTP_ON_REDIR:
        return "ERROR_INTERNET_HTTPS_TO_HTTP_ON_REDIR";
    case ERROR_INTERNET_DECODING_FAILED:
        return "ERROR_INTERNET_DECODING_FAILED";
    default:
        return "ERROR_INTERNET_UNKNOWN";
    }
}

std::string ReadWinInetResponseInfo(WinInetApi& api)
{
    if (api.GetLastResponseInfo == nullptr)
    {
        return std::string();
    }

    char buffer[512] = {};
    DWORD responseError = 0;
    DWORD size = static_cast<DWORD>(std::size(buffer) - 1);
    if (api.GetLastResponseInfo(&responseError, buffer, &size) == FALSE || size == 0)
    {
        return std::string();
    }

    buffer[size] = '\0';
    std::string info = TrimAscii(buffer);
    if (info.empty())
    {
        return std::string();
    }

    char prefixed[640] = {};
    std::snprintf(prefixed, sizeof(prefixed), "respErr=%lu msg=%s", static_cast<unsigned long>(responseError), info.c_str());
    return std::string(prefixed);
}

void LogWinInetFailure(const char* stage, WinInetApi& api)
{
    const DWORD error = GetLastError();
    const std::string responseInfo = ReadWinInetResponseInfo(api);
    if (!responseInfo.empty())
    {
        mod::Log(
            "LobbySession::DoHttpGet: %s failed (%lu %s) %s",
            stage,
            static_cast<unsigned long>(error),
            WinInetErrorName(error),
            responseInfo.c_str());
    }
    else
    {
        mod::Log(
            "LobbySession::DoHttpGet: %s failed (%lu %s)",
            stage,
            static_cast<unsigned long>(error),
            WinInetErrorName(error));
    }
}

std::wstring Utf8ToWideNullTerminated(const std::string& utf8)
{
    const int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wLen <= 1)
    {
        return std::wstring();
    }

    std::wstring wide(static_cast<size_t>(wLen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], wLen);
    return wide;
}

std::string DoHttpGetViaWinHttp(const std::string& path)
{
    std::string result;
    WinHttpApi& api = GetWinHttpApi();
    if (!api.available)
    {
        return result;
    }

    HINTERNET hSession = api.Open(
        L"EFZNetplayMod/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (hSession == nullptr)
    {
        mod::Log("LobbySession::DoHttpGet: WinHttpOpen failed (%lu)", GetLastError());
        return result;
    }

    api.SetTimeouts(hSession,
        static_cast<int>(kConnectTimeoutMs),
        static_cast<int>(kConnectTimeoutMs),
        static_cast<int>(kReceiveTimeoutMs),
        static_cast<int>(kReceiveTimeoutMs));

    HINTERNET hConnect = api.Connect(hSession, kConcertoHost, kConcertoPort, 0);
    if (hConnect == nullptr)
    {
        mod::Log("LobbySession::DoHttpGet: WinHttpConnect failed (%lu)", GetLastError());
        api.CloseHandle(hSession);
        return result;
    }

    const std::wstring wPath = Utf8ToWideNullTerminated(path);
    if (wPath.empty())
    {
        api.CloseHandle(hConnect);
        api.CloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = api.OpenRequest(
        hConnect,
        L"GET",
        wPath.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (hRequest == nullptr)
    {
        mod::Log("LobbySession::DoHttpGet: WinHttpOpenRequest failed (%lu)", GetLastError());
        api.CloseHandle(hConnect);
        api.CloseHandle(hSession);
        return result;
    }

    const BOOL sent = api.SendRequest(
        hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0);
    if (!sent || !api.ReceiveResponse(hRequest, nullptr))
    {
        mod::Log("LobbySession::DoHttpGet: WinHttp send/receive failed (%lu)", GetLastError());
        api.CloseHandle(hRequest);
        api.CloseHandle(hConnect);
        api.CloseHandle(hSession);
        return result;
    }

    DWORD available = 0;
    while (api.QueryDataAvailable(hRequest, &available) && available > 0)
    {
        const size_t oldSize = result.size();
        result.resize(oldSize + available);
        DWORD bytesRead = 0;
        if (!api.ReadData(hRequest, &result[oldSize], available, &bytesRead))
        {
            break;
        }
        result.resize(oldSize + bytesRead);
    }

    api.CloseHandle(hRequest);
    api.CloseHandle(hConnect);
    api.CloseHandle(hSession);
    return result;
}

std::string DoHttpGetViaWinInet(const std::string& fullUrl)
{
    std::string result;
    WinInetApi& api = GetWinInetApi();
    if (!api.available)
    {
        return result;
    }

    HINTERNET hInternet = api.Open(
        L"EFZNetplayMod/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG,
        nullptr,
        nullptr,
        0);
    if (hInternet == nullptr)
    {
        LogWinInetFailure("InternetOpenW", api);
        return result;
    }

    const DWORD connectTimeout = kConnectTimeoutMs;
    const DWORD receiveTimeout = kReceiveTimeoutMs;
    if (api.SetOption(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, (LPVOID)&connectTimeout, sizeof(connectTimeout)) == FALSE)
    {
        LogWinInetFailure("InternetSetOption(CONNECT_TIMEOUT)", api);
    }
    if (api.SetOption(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, (LPVOID)&receiveTimeout, sizeof(receiveTimeout)) == FALSE)
    {
        LogWinInetFailure("InternetSetOption(RECEIVE_TIMEOUT)", api);
    }
    if (api.SetOption(hInternet, INTERNET_OPTION_SEND_TIMEOUT, (LPVOID)&receiveTimeout, sizeof(receiveTimeout)) == FALSE)
    {
        LogWinInetFailure("InternetSetOption(SEND_TIMEOUT)", api);
    }

    const std::wstring wUrl = Utf8ToWideNullTerminated(fullUrl);
    if (wUrl.empty())
    {
        api.CloseHandle(hInternet);
        return result;
    }

    DWORD flags = INTERNET_FLAG_RELOAD
        | INTERNET_FLAG_NO_CACHE_WRITE
        | INTERNET_FLAG_PRAGMA_NOCACHE;
    if (HasHttpsScheme(fullUrl))
    {
        flags |= INTERNET_FLAG_SECURE;
    }

    HINTERNET hUrl = api.OpenUrl(hInternet, wUrl.c_str(), nullptr, 0, flags, 0);
    if (hUrl == nullptr)
    {
        LogWinInetFailure("InternetOpenUrlW", api);
        api.CloseHandle(hInternet);
        return result;
    }

    char buffer[4096] = {};
    for (;;)
    {
        DWORD bytesRead = 0;
        if (!api.ReadFile(hUrl, buffer, sizeof(buffer), &bytesRead))
        {
            LogWinInetFailure("InternetReadFile", api);
            break;
        }
        if (bytesRead == 0)
        {
            break;
        }
        result.append(buffer, buffer + bytesRead);
    }

    api.CloseHandle(hUrl);
    api.CloseHandle(hInternet);
    return result;
}

std::string TryHttpGetForEndpoint(
    const LobbyEndpointConfig& endpointConfig,
    const std::string& endpointTag,
    const std::string& requestUrl,
    const std::string& defaultPathForWinHttp,
    bool allowWinHttpForThisUrl,
    int* outBackend)
{
    if (outBackend != nullptr)
    {
        *outBackend = 0;
    }

    const bool isHttps = HasHttpsScheme(requestUrl);
    const bool tryEmbeddedTls = isHttps && (!endpointConfig.forceWinInet || endpointConfig.forceEmbeddedTls);

    if (tryEmbeddedTls)
    {
        std::string body;
        std::string error;
        if (netplay::tls::HttpGet(requestUrl, endpointConfig.tlsVerify, kReceiveTimeoutMs, &body, &error))
        {
            if (outBackend != nullptr)
            {
                *outBackend = 3;
            }
            return body;
        }

        mod::Log(
            "LobbySession::DoHttpGet: %s endpoint EmbeddedTLS failed url='%s' error='%s'",
            endpointTag.c_str(),
            requestUrl.c_str(),
            error.c_str());

        // Explicit force means skip non-embedded backends for this endpoint only.
        if (endpointConfig.forceEmbeddedTls)
        {
            return std::string();
        }
    }

    if (allowWinHttpForThisUrl && !endpointConfig.forceWinInet && !endpointConfig.forceEmbeddedTls)
    {
        const std::string result = DoHttpGetViaWinHttp(defaultPathForWinHttp);
        if (!result.empty())
        {
            if (outBackend != nullptr)
            {
                *outBackend = 1;
            }
            return result;
        }
    }

    const std::string winInetResult = DoHttpGetViaWinInet(requestUrl);
    if (!winInetResult.empty())
    {
        if (outBackend != nullptr)
        {
            *outBackend = 2;
        }
        return winInetResult;
    }

    return std::string();
}

void LogBackendTransition(const LobbyEndpointConfig& endpointConfig, int backend, const char* endpointTag)
{
    const int previous = g_lobbyHttpBackend.exchange(backend);
    if (previous == backend)
    {
        return;
    }

    switch (backend)
    {
    case 1:
        mod::Log("LobbySession::DoHttpGet: backend=WinHTTP endpoint=%s", endpointTag);
        break;
    case 2:
        mod::Log("LobbySession::DoHttpGet: backend=WinINet endpoint=%s", endpointTag);
        break;
    case 3:
        mod::Log(
            "LobbySession::DoHttpGet: backend=EmbeddedTLS endpoint=%s verify=%d",
            endpointTag,
            endpointConfig.tlsVerify ? 1 : 0);
        break;
    default:
        break;
    }
}

// Minimal percent-encoding for a query-string value.
std::string UrlEncode(const std::string& s)
{
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s)
    {
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '~')
        {
            out += static_cast<char>(c);
        }
        else if (c == ' ')
        {
            out += '+';
        }
        else
        {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned>(c));
            out += buf;
        }
    }
    return out;
}
} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

LobbySession::LobbySession(std::string nickname, uint16_t hostPort)
    : m_nickname(std::move(nickname))
    , m_hostPort(hostPort)
{
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_wakeEvent == nullptr)
    {
        mod::Log("LobbySession: CreateEvent failed (%lu)", GetLastError());
    }

    m_status.pollState = PollState::NotJoined;
    m_pollThread = std::thread(&LobbySession::PollThreadEntry, this);
    mod::Log("LobbySession: started for nickname='%s'", m_nickname.c_str());
}

LobbySession::~LobbySession()
{
    mod::Log("LobbySession: shutting down");
    m_shouldStop.store(true);
    if (m_wakeEvent != nullptr)
    {
        SetEvent(m_wakeEvent);
    }
    if (m_pollThread.joinable())
    {
        m_pollThread.join();
    }
    if (m_wakeEvent != nullptr)
    {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
    mod::Log("LobbySession: shut down complete");
}

const std::string& LobbySession::GetNickname() const
{
    return m_nickname;
}

LobbyStatus LobbySession::GetStatus() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

int LobbySession::GetPlayerId() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_playerId;
}

void LobbySession::RequestRefresh()
{
    m_refreshRequested.store(true);
    if (m_wakeEvent != nullptr)
    {
        SetEvent(m_wakeEvent);
    }
}

void LobbySession::SendChallenge(int targetPlayerId, const std::string& ipPort)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        PendingAction action;
        action.type = PendingAction::Challenge;
        action.targetPlayerId = targetPlayerId;
        action.ipPort = ipPort;
        m_pendingActions.push_back(std::move(action));
    }
    // Wake the poll thread to process immediately.
    if (m_wakeEvent != nullptr)
    {
        SetEvent(m_wakeEvent);
    }
}

void LobbySession::AcceptChallenge(int challengerPlayerId)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        PendingAction action;
        action.type = PendingAction::PreAccept;
        action.targetPlayerId = challengerPlayerId;
        m_pendingActions.push_back(std::move(action));
    }
    if (m_wakeEvent != nullptr)
    {
        SetEvent(m_wakeEvent);
    }
}

void LobbySession::NotifyMatchConnected()
{
    // Called when the P2P connection is established (delay setup overlay shown).
    // Queue the deferred 'accept' so the lobby shows the pair as "playing".
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        PendingAction action;
        action.type = PendingAction::ConfirmAccept;
        // targetPlayerId is not needed for accept — the server tracks the
        // pending challenge state.  We send 0 and DoAccept will use
        // the last pre_accept target if needed.
        action.targetPlayerId = 0;
        m_pendingActions.push_back(std::move(action));
    }
    if (m_wakeEvent != nullptr)
    {
        SetEvent(m_wakeEvent);
    }
}

void LobbySession::NotifyEndMatch()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        PendingAction action;
        action.type = PendingAction::End;
        m_pendingActions.push_back(std::move(action));
    }
    if (m_wakeEvent != nullptr)
    {
        SetEvent(m_wakeEvent);
    }
}

// ---------------------------------------------------------------------------
// Background thread
// ---------------------------------------------------------------------------

void LobbySession::PollThreadEntry()
{
    mod::Log("LobbySession::PollThread: entering");

    // Update state to Joining before the HTTP call.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Joining;
        m_status.statusMessage = "Connecting to lobby...";
    }

    if (m_shouldStop.load())
    {
        mod::Log("LobbySession::PollThread: stop requested before join");
        return;
    }

    if (!DoJoin())
    {
        mod::Log("LobbySession::PollThread: join failed, exiting");
        return;
    }

    mod::Log("LobbySession::PollThread: joined lobby id=%d playerId=%d", m_lobbyNumericId, m_playerId);

    // Discover public IP in the background after joining.
    DiscoverPublicIp();

    // Poll loop.
    while (!m_shouldStop.load())
    {
        m_refreshRequested.store(false);

        // Process any queued challenge/accept actions before polling.
        ProcessPendingActions();

        DoPollStatus();

        // Wait for kPollIntervalMs or until woken early.
        if (m_wakeEvent != nullptr)
        {
            WaitForSingleObject(m_wakeEvent, kPollIntervalMs);
        }
        else
        {
            Sleep(kPollIntervalMs);
        }
    }

    // Leave the lobby on the way out.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Leaving;
        m_status.statusMessage = "Leaving lobby...";
    }
    DoLeave();
    mod::Log("LobbySession::PollThread: exiting");
}

// ---------------------------------------------------------------------------
// Individual lobby HTTP operations
// ---------------------------------------------------------------------------

bool LobbySession::DoJoin()
{
    char path[512];
    if (m_hostPort != 0)
    {
        std::snprintf(
            path,
            sizeof(path),
            "/l?action=join&id=EFZ&game=efz&name=%s&port=%u",
            UrlEncode(m_nickname).c_str(),
            static_cast<unsigned>(m_hostPort));
    }
    else
    {
        std::snprintf(
            path,
            sizeof(path),
            "/l?action=join&id=EFZ&game=efz&name=%s",
            UrlEncode(m_nickname).c_str());
    }

    const std::string body = DoHttpGet(path);
    if (body.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Error;
        m_status.statusMessage = "HTTP request failed";
        return false;
    }

    mod::Log("LobbySession::DoJoin: response='%s'", body.c_str());

    // Expect {"status":"OK","id":N,"msg":N,"secret":N}
    if (body.find("\"OK\"") == std::string::npos)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Error;
        m_status.statusMessage = "Join rejected by server";
        return false;
    }

    int numericId = 0;
    int playerId = 0;
    int secret = 0;
    if (!ExtractJsonInt(body, "id", &numericId)
        || !ExtractJsonInt(body, "msg", &playerId)
        || !ExtractJsonInt(body, "secret", &secret))
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Error;
        m_status.statusMessage = "Failed to parse join response";
        return false;
    }

    m_lobbyNumericId = numericId;
    m_playerId = playerId;
    m_secret = secret;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.pollState = PollState::Polling;
    m_status.statusMessage.clear();
    return true;
}

bool LobbySession::DoPollStatus()
{
    char path[512];
    std::snprintf(
        path,
        sizeof(path),
        "/l?action=status&id=%d&p=%d&secret=%d",
        m_lobbyNumericId,
        m_playerId,
        m_secret);

    const std::string body = DoHttpGet(path);
    if (body.empty())
    {
        mod::Log("LobbySession::DoPollStatus: empty response");
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.pollState = PollState::Error;
        m_status.statusMessage = "Poll request failed";
        return false;
    }

    mod::Log("LobbySession::DoPollStatus: response='%s'", body.c_str());

    std::vector<LobbyPlayer> idlePlayers;
    ParseIdlePlayers(body, &idlePlayers);

    std::vector<LobbyChallenge> challenges;
    ParseChallenges(body, &challenges);

    std::vector<LobbyPlayingPair> playingPairs;
    ParsePlayingPairs(body, &playingPairs);

    std::vector<LobbyDisplayEntry> displayEntries;
    BuildDisplayEntries(challenges, idlePlayers, playingPairs, m_playerId, &displayEntries);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.pollState = PollState::Polling;
    m_status.idlePlayers = std::move(idlePlayers);
    m_status.challenges = std::move(challenges);
    m_status.displayEntries = std::move(displayEntries);
    m_status.playing = std::move(playingPairs);
    m_status.publicIp = m_publicIp;
    m_status.statusMessage.clear();
    m_status.lastPollTick = GetTickCount();
    return true;
}

void LobbySession::DoLeave()
{
    if (m_lobbyNumericId == 0)
    {
        return;
    }

    char path[512];
    std::snprintf(
        path,
        sizeof(path),
        "/l?action=leave&id=%d&p=%d&secret=%d",
        m_lobbyNumericId,
        m_playerId,
        m_secret);

    const std::string body = DoHttpGet(path);
    mod::Log("LobbySession::DoLeave: response='%s'", body.c_str());
}

// ---------------------------------------------------------------------------
// HTTP backend helper
// ---------------------------------------------------------------------------

std::string LobbySession::DoHttpGet(const std::string& path)
{
    const LobbyEndpointConfig& endpointConfig = GetLobbyEndpointConfig();
    const std::string primaryUrl = BuildLobbyRequestUrl(path);
    std::string proxyUrl = BuildLobbyProxyRequestUrl(path);
    if (!proxyUrl.empty() && proxyUrl == primaryUrl)
    {
        proxyUrl.clear();
    }

    int backend = 0;
    const bool allowPrimaryWinHttp = !endpointConfig.hasBaseUrlOverride;
    std::string body = TryHttpGetForEndpoint(
        endpointConfig,
        "primary",
        primaryUrl,
        path,
        allowPrimaryWinHttp,
        &backend);
    if (!body.empty())
    {
        LogBackendTransition(endpointConfig, backend, "primary");
        return body;
    }

    if (!proxyUrl.empty())
    {
        mod::Log(
            "LobbySession::DoHttpGet: primary endpoint failed; retrying proxy endpoint url='%s'",
            proxyUrl.c_str());

        body = TryHttpGetForEndpoint(
            endpointConfig,
            "proxy",
            proxyUrl,
            path,
            false,
            &backend);
        if (!body.empty())
        {
            LogBackendTransition(endpointConfig, backend, "proxy");
            return body;
        }
    }

    const int previous = g_lobbyHttpBackend.exchange(0);
    if (previous != 0)
    {
        mod::Log("LobbySession::DoHttpGet: no available HTTP backend");
    }

    return std::string();
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers
// ---------------------------------------------------------------------------

bool LobbySession::ExtractJsonInt(const std::string& json, const char* key, int* out)
{
    // Search for "key": and parse the integer that follows.
    const std::string search = std::string("\"") + key + "\":";
    const size_t pos = json.find(search);
    if (pos == std::string::npos)
    {
        return false;
    }

    size_t valuePos = pos + search.size();
    while (valuePos < json.size() && json[valuePos] == ' ')
    {
        ++valuePos;
    }

    if (valuePos >= json.size() || !std::isdigit(static_cast<unsigned char>(json[valuePos])))
    {
        return false;
    }

    char* endPtr = nullptr;
    *out = static_cast<int>(std::strtol(json.c_str() + valuePos, &endPtr, 10));
    return (endPtr != json.c_str() + valuePos);
}

void LobbySession::ParseIdlePlayers(const std::string& json, std::vector<LobbyPlayer>* out)
{
    out->clear();

    // Find the idle array: "idle":[
    const char* idleTag = "\"idle\":[";
    const size_t idlePos = json.find(idleTag);
    if (idlePos == std::string::npos)
    {
        return;
    }

    size_t cursor = idlePos + std::strlen(idleTag);

    // Each element is either [] (empty) or ["name",playerId]
    while (cursor < json.size() && out->size() < static_cast<size_t>(kMaxDisplayPlayers))
    {
        // Skip whitespace and commas between elements.
        while (cursor < json.size() && (json[cursor] == ',' || json[cursor] == ' ' || json[cursor] == '\n' || json[cursor] == '\r'))
        {
            ++cursor;
        }

        if (cursor >= json.size() || json[cursor] == ']')
        {
            break; // End of idle array.
        }

        if (json[cursor] != '[')
        {
            break; // Unexpected character.
        }
        ++cursor; // Consume '['.

        // Skip whitespace.
        while (cursor < json.size() && json[cursor] == ' ')
        {
            ++cursor;
        }

        if (cursor >= json.size() || json[cursor] != '"')
        {
            break; // Expected quoted name.
        }
        ++cursor; // Consume opening '"'.

        // Read the player name until closing '"'.
        std::string playerName;
        while (cursor < json.size() && json[cursor] != '"')
        {
            // Basic escape: skip backslash and next char.
            if (json[cursor] == '\\' && cursor + 1 < json.size())
            {
                ++cursor;
            }
            playerName += json[cursor];
            ++cursor;
        }
        if (cursor < json.size())
        {
            ++cursor; // Consume closing '"'.
        }

        // Skip comma between name and id.
        while (cursor < json.size() && (json[cursor] == ',' || json[cursor] == ' '))
        {
            ++cursor;
        }

        // Read player id.
        if (cursor >= json.size() || !std::isdigit(static_cast<unsigned char>(json[cursor])))
        {
            break;
        }
        char* endPtr = nullptr;
        const int playerId = static_cast<int>(std::strtol(json.c_str() + cursor, &endPtr, 10));
        cursor = static_cast<size_t>(endPtr - json.c_str());

        // Advance past the closing ']' of this element.
        while (cursor < json.size() && json[cursor] != ']')
        {
            ++cursor;
        }
        if (cursor < json.size())
        {
            ++cursor;
        }

        LobbyPlayer player;
        player.name = std::move(playerName);
        player.playerId = playerId;
        out->push_back(std::move(player));
    }
}

void LobbySession::ParsePlayingPairs(const std::string& json, std::vector<LobbyPlayingPair>* out)
{
    out->clear();

    // Find the playing array: "playing":[
    // Each element is ["p1name","p2name",p1id,p2id,"host_ip"]
    const char* playingTag = "\"playing\":";
    const size_t playingPos = json.find(playingTag);
    if (playingPos == std::string::npos)
    {
        return;
    }

    // Find the opening '[' of the playing array.
    size_t arrayStart = json.find('[', playingPos + std::strlen(playingTag));
    if (arrayStart == std::string::npos)
    {
        return;
    }
    size_t cursor = arrayStart + 1;

    auto skipWhitespace = [&]()
    {
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\n' || json[cursor] == '\r'))
        {
            ++cursor;
        }
    };

    auto readQuotedString = [&](std::string* dst) -> bool
    {
        skipWhitespace();
        if (cursor >= json.size() || json[cursor] != '"')
        {
            return false;
        }
        ++cursor; // consume opening '"'
        dst->clear();
        while (cursor < json.size() && json[cursor] != '"')
        {
            if (json[cursor] == '\\' && cursor + 1 < json.size())
            {
                ++cursor;
            }
            *dst += json[cursor];
            ++cursor;
        }
        if (cursor < json.size())
        {
            ++cursor; // consume closing '"'
        }
        return true;
    };

    auto readInt = [&](int* dst) -> bool
    {
        skipWhitespace();
        if (cursor >= json.size() || !std::isdigit(static_cast<unsigned char>(json[cursor])))
        {
            return false;
        }
        char* endPtr = nullptr;
        *dst = static_cast<int>(std::strtol(json.c_str() + cursor, &endPtr, 10));
        cursor = static_cast<size_t>(endPtr - json.c_str());
        return true;
    };

    while (cursor < json.size() && out->size() < static_cast<size_t>(kMaxPlayingPairs))
    {
        // Skip commas and whitespace between elements.
        while (cursor < json.size() && (json[cursor] == ',' || json[cursor] == ' ' || json[cursor] == '\n'))
        {
            ++cursor;
        }
        if (cursor >= json.size() || json[cursor] == ']')
        {
            break;
        }
        if (json[cursor] != '[')
        {
            break;
        }
        ++cursor; // consume '['

        LobbyPlayingPair pair;
        if (!readQuotedString(&pair.p1Name))
        {
            break;
        }
        skipWhitespace();
        if (cursor < json.size() && json[cursor] == ',')
        {
            ++cursor;
        }
        if (!readQuotedString(&pair.p2Name))
        {
            break;
        }
        skipWhitespace();
        if (cursor < json.size() && json[cursor] == ',')
        {
            ++cursor;
        }
        readInt(&pair.p1Id);
        skipWhitespace();
        if (cursor < json.size() && json[cursor] == ',')
        {
            ++cursor;
        }
        readInt(&pair.p2Id);
        skipWhitespace();
        if (cursor < json.size() && json[cursor] == ',')
        {
            ++cursor;
        }
        readQuotedString(&pair.hostIp); // optional, may not be present

        // Advance past the closing ']' of this element.
        while (cursor < json.size() && json[cursor] != ']')
        {
            ++cursor;
        }
        if (cursor < json.size())
        {
            ++cursor;
        }

        out->push_back(std::move(pair));
    }
}

// ---------------------------------------------------------------------------
// Challenge JSON parsing
// ---------------------------------------------------------------------------

void LobbySession::ParseChallenges(const std::string& json, std::vector<LobbyChallenge>* out)
{
    out->clear();

    // Find the challenges array: "challenges":[
    // Each element is ["name", playerId, "ip:port"]
    const char* tag = "\"challenges\":[";
    const size_t tagPos = json.find(tag);
    if (tagPos == std::string::npos)
    {
        return;
    }

    size_t cursor = tagPos + std::strlen(tag);

    while (cursor < json.size() && out->size() < 16u)
    {
        // Skip whitespace and commas.
        while (cursor < json.size() && (json[cursor] == ',' || json[cursor] == ' ' || json[cursor] == '\n' || json[cursor] == '\r'))
        {
            ++cursor;
        }

        if (cursor >= json.size() || json[cursor] == ']')
        {
            break;
        }

        if (json[cursor] != '[')
        {
            break;
        }
        ++cursor; // consume '['

        // Read quoted name.
        while (cursor < json.size() && json[cursor] == ' ')
        {
            ++cursor;
        }
        if (cursor >= json.size() || json[cursor] != '"')
        {
            break;
        }
        ++cursor; // consume opening '"'

        std::string name;
        while (cursor < json.size() && json[cursor] != '"')
        {
            if (json[cursor] == '\\' && cursor + 1 < json.size())
            {
                ++cursor;
            }
            name += json[cursor];
            ++cursor;
        }
        if (cursor < json.size())
        {
            ++cursor; // consume closing '"'
        }

        // Skip comma.
        while (cursor < json.size() && (json[cursor] == ',' || json[cursor] == ' '))
        {
            ++cursor;
        }

        // Read player id.
        if (cursor >= json.size() || !std::isdigit(static_cast<unsigned char>(json[cursor])))
        {
            break;
        }
        char* endPtr = nullptr;
        const int playerId = static_cast<int>(std::strtol(json.c_str() + cursor, &endPtr, 10));
        cursor = static_cast<size_t>(endPtr - json.c_str());

        // Skip comma.
        while (cursor < json.size() && (json[cursor] == ',' || json[cursor] == ' '))
        {
            ++cursor;
        }

        // Read ip:port string.
        std::string ipPort;
        if (cursor < json.size() && json[cursor] == '"')
        {
            ++cursor; // consume opening '"'
            while (cursor < json.size() && json[cursor] != '"')
            {
                ipPort += json[cursor];
                ++cursor;
            }
            if (cursor < json.size())
            {
                ++cursor; // consume closing '"'
            }
        }

        // Advance past closing ']'.
        while (cursor < json.size() && json[cursor] != ']')
        {
            ++cursor;
        }
        if (cursor < json.size())
        {
            ++cursor;
        }

        LobbyChallenge ch;
        ch.name = std::move(name);
        ch.playerId = playerId;
        ch.ipPort = std::move(ipPort);
        out->push_back(std::move(ch));
    }
}

// ---------------------------------------------------------------------------
// Build merged display list
// ---------------------------------------------------------------------------

void LobbySession::BuildDisplayEntries(
    const std::vector<LobbyChallenge>& challenges,
    const std::vector<LobbyPlayer>& idlePlayers,
    const std::vector<LobbyPlayingPair>& playing,
    int selfPlayerId,
    std::vector<LobbyDisplayEntry>* out)
{
    out->clear();
    out->reserve(challenges.size() + idlePlayers.size());

    // Build a set of player IDs that appear in challenges, so we can
    // deduplicate them from the idle list (a challenger also shows in idle).
    std::vector<int> challengerIds;
    challengerIds.reserve(challenges.size());

    // Build a set of player IDs that are currently playing.
    std::vector<int> playingIds;
    playingIds.reserve(playing.size() * 2);
    for (const auto& pp : playing)
    {
        playingIds.push_back(pp.p1Id);
        playingIds.push_back(pp.p2Id);
    }

    auto isPlayerPlaying = [&](int id) -> bool {
        for (int pid : playingIds)
        {
            if (pid == id) return true;
        }
        return false;
    };

    auto findSpectateIp = [&](int id) -> std::string {
        for (const auto& pp : playing)
        {
            if (pp.p1Id == id || pp.p2Id == id)
            {
                return pp.hostIp;
            }
        }
        return std::string();
    };

    // Challenges first — they are actionable and time-sensitive.
    for (const auto& ch : challenges)
    {
        challengerIds.push_back(ch.playerId);

        LobbyDisplayEntry entry;
        entry.name = ch.name;
        entry.playerId = ch.playerId;
        entry.isChallenge = true;
        entry.isSelf = (ch.playerId == selfPlayerId);
        entry.isPlaying = isPlayerPlaying(ch.playerId);
        entry.ipPort = ch.ipPort;
        entry.spectateIp = findSpectateIp(ch.playerId);
        out->push_back(std::move(entry));
    }

    // Then idle players, skipping any that already appear as challengers.
    for (const auto& p : idlePlayers)
    {
        bool isDuplicate = false;
        for (int cid : challengerIds)
        {
            if (cid == p.playerId)
            {
                isDuplicate = true;
                break;
            }
        }
        if (isDuplicate)
        {
            continue;
        }

        LobbyDisplayEntry entry;
        entry.name = p.name;
        entry.playerId = p.playerId;
        entry.isChallenge = false;
        entry.isSelf = (p.playerId == selfPlayerId);
        entry.isPlaying = isPlayerPlaying(p.playerId);
        entry.spectateIp = findSpectateIp(p.playerId);
        out->push_back(std::move(entry));
    }
}

// ---------------------------------------------------------------------------
// Challenge / Accept / Pre-Accept HTTP calls
// ---------------------------------------------------------------------------

bool LobbySession::DoChallenge(int targetPlayerId, const std::string& ipPort)
{
    char path[512];
    std::snprintf(
        path,
        sizeof(path),
        "/l?action=challenge&id=%d&p=%d&secret=%d&t=%d&ip=%s",
        m_lobbyNumericId,
        m_playerId,
        m_secret,
        targetPlayerId,
        UrlEncode(ipPort).c_str());

    const std::string body = DoHttpGet(path);
    mod::Log("LobbySession::DoChallenge: target=%d ip=%s response='%s'",
        targetPlayerId, ipPort.c_str(), body.c_str());

    return !body.empty() && body.find("\"OK\"") != std::string::npos;
}

bool LobbySession::DoPreAccept(int challengerPlayerId)
{
    char path[512];
    std::snprintf(
        path,
        sizeof(path),
        "/l?action=pre_accept&id=%d&p=%d&secret=%d&t=%d",
        m_lobbyNumericId,
        m_playerId,
        m_secret,
        challengerPlayerId);

    const std::string body = DoHttpGet(path);
    mod::Log("LobbySession::DoPreAccept: challenger=%d response='%s'",
        challengerPlayerId, body.c_str());

    return !body.empty();
}

bool LobbySession::DoAccept(int challengerPlayerId)
{
    char path[512];
    std::snprintf(
        path,
        sizeof(path),
        "/l?action=accept&id=%d&p=%d&secret=%d&t=%d",
        m_lobbyNumericId,
        m_playerId,
        m_secret,
        challengerPlayerId);

    const std::string body = DoHttpGet(path);
    mod::Log("LobbySession::DoAccept: challenger=%d response='%s'",
        challengerPlayerId, body.c_str());

    return !body.empty();
}

bool LobbySession::DoEnd()
{
    char path[512];
    std::snprintf(
        path,
        sizeof(path),
        "/l?action=end&id=%d&p=%d&secret=%d",
        m_lobbyNumericId,
        m_playerId,
        m_secret);

    const std::string body = DoHttpGet(path);
    mod::Log("LobbySession::DoEnd: response='%s'", body.c_str());

    return !body.empty();
}

// ---------------------------------------------------------------------------
// Public IP discovery
// ---------------------------------------------------------------------------

void LobbySession::DiscoverPublicIp()
{
    // Try the embedded TLS client first (works on all platforms).
    if (netplay::tls::IsAvailable())
    {
        std::string body;
        std::string error;
        if (netplay::tls::HttpGet("https://api.ipify.org", false, 5000, &body, &error))
        {
            // Trim whitespace.
            while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' '))
            {
                body.pop_back();
            }
            if (!body.empty())
            {
                m_publicIp = body;
                std::lock_guard<std::mutex> lock(m_mutex);
                m_status.publicIp = m_publicIp;
                mod::Log("LobbySession::DiscoverPublicIp: resolved=%s", m_publicIp.c_str());
                return;
            }
        }
        mod::Log("LobbySession::DiscoverPublicIp: TLS request failed: %s", error.c_str());
    }

    // Fallback: plain HTTP to 4.ident.me (same service Concerto uses) via WinINet.
    static auto TrimIpBody = [](std::string& s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        {
            s.pop_back();
        }
    };

    std::string body = DoHttpGetViaWinInet("http://4.ident.me");
    TrimIpBody(body);
    if (!body.empty())
    {
        m_publicIp = body;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.publicIp = m_publicIp;
        mod::Log("LobbySession::DiscoverPublicIp: resolved=%s via 4.ident.me", m_publicIp.c_str());
        return;
    }

    body = DoHttpGetViaWinInet("http://4.tnedi.me");
    TrimIpBody(body);
    if (!body.empty())
    {
        m_publicIp = body;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.publicIp = m_publicIp;
        mod::Log("LobbySession::DiscoverPublicIp: resolved=%s via 4.tnedi.me", m_publicIp.c_str());
        return;
    }

    mod::Log("LobbySession::DiscoverPublicIp: unable to resolve public IP");
}

// ---------------------------------------------------------------------------
// Pending action processing
// ---------------------------------------------------------------------------

void LobbySession::ProcessPendingActions()
{
    std::vector<PendingAction> actions;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        actions.swap(m_pendingActions);
    }

    for (const auto& action : actions)
    {
        if (m_shouldStop.load())
        {
            break;
        }

        switch (action.type)
        {
        case PendingAction::Challenge:
            mod::Log("LobbySession: processing challenge target=%d ip=%s",
                action.targetPlayerId, action.ipPort.c_str());
            DoChallenge(action.targetPlayerId, action.ipPort);
            break;

        case PendingAction::PreAccept:
            mod::Log("LobbySession: processing pre_accept challenger=%d",
                action.targetPlayerId);
            m_pendingAcceptTargetId = action.targetPlayerId;
            DoPreAccept(action.targetPlayerId);
            break;

        case PendingAction::ConfirmAccept:
            if (m_pendingAcceptTargetId != 0)
            {
                mod::Log("LobbySession: processing deferred accept target=%d",
                    m_pendingAcceptTargetId);
                DoAccept(m_pendingAcceptTargetId);
                m_pendingAcceptTargetId = 0;
            }
            else
            {
                mod::Log("LobbySession: ConfirmAccept with no pending target, ignoring");
            }
            break;

        case PendingAction::End:
            mod::Log("LobbySession: processing end");
            DoEnd();
            m_pendingAcceptTargetId = 0;
            break;
        }
    }
}
}
