#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/i18n.h"
#include "netplay/bridge/console_capture_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <limits>
#include <share.h>
#include <thread>

#include <windows.h>

namespace netplay::bridge::takeover
{

void CopyString(char* dst, size_t dstSize, const char* src)
{
    if (dst == nullptr || dstSize == 0)
    {
        return;
    }
    if (src == nullptr)
    {
        dst[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(dst, dstSize, src, _TRUNCATE);
#else
    std::snprintf(dst, dstSize, "%s", src);
#endif
}

bool ExtractConsoleScriptLine(const char* script, LONG* inOutOffset, char* outLine, size_t outLineSize, bool* outHasMore)
{
    if (outLine == nullptr || outLineSize == 0)
    {
        return false;
    }

    outLine[0] = '\0';
    if (outHasMore != nullptr)
    {
        *outHasMore = false;
    }

    if (script == nullptr || script[0] == '\0')
    {
        return false;
    }

    const size_t len = std::strlen(script);
    LONG offset = (inOutOffset != nullptr) ? *inOutOffset : 0;
    if (offset < 0)
    {
        offset = 0;
    }

    size_t start = static_cast<size_t>(offset);
    if (start >= len)
    {
        if (inOutOffset != nullptr)
        {
            *inOutOffset = static_cast<LONG>(len);
        }
        return false;
    }

    size_t end = start;
    while (end < len && script[end] != '\n')
    {
        ++end;
    }
    if (end < len && script[end] == '\n')
    {
        ++end;
    }

    const size_t bytes = end - start;
    const size_t copyBytes = (std::min)(bytes, outLineSize - 1);
    if (copyBytes > 0)
    {
        std::memcpy(outLine, script + start, copyBytes);
    }
    outLine[copyBytes] = '\0';

    if (inOutOffset != nullptr)
    {
        *inOutOffset = static_cast<LONG>(end);
    }
    if (outHasMore != nullptr)
    {
        *outHasMore = (end < len);
    }
    return copyBytes > 0;
}

bool IsLikelyTextChunk(const char* text, size_t length)
{
    if (text == nullptr || length == 0)
    {
        return false;
    }

    size_t printable = 0;
    size_t checked = 0;
    for (size_t i = 0; i < length; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\0')
        {
            break;
        }
        ++checked;
        if ((c >= 0x20 && c <= 0x7E) || c >= 0x80 || c == '\r' || c == '\n' || c == '\t')
        {
            ++printable;
        }
    }
    if (checked == 0)
    {
        return false;
    }
    return printable >= (checked / 2);
}

std::string TrimAscii(const std::string& text)
{
    size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n'))
    {
        ++start;
    }
    size_t end = text.size();
    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' || text[end - 1] == '\n'))
    {
        --end;
    }
    return text.substr(start, end - start);
}

std::string ToLowerAscii(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool StartsWithCaseInsensitive(const std::string& text, const char* prefix)
{
    if (prefix == nullptr || prefix[0] == '\0')
    {
        return false;
    }

    const size_t prefixLen = std::strlen(prefix);
    if (text.size() < prefixLen)
    {
        return false;
    }

    for (size_t index = 0; index < prefixLen; ++index)
    {
        const char a =
            static_cast<char>(std::tolower(static_cast<unsigned char>(text[index])));
        const char b =
            static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[index])));
        if (a != b)
        {
            return false;
        }
    }

    return true;
}

bool TryExtractDiedEndpoint(const std::string& text, std::string* outEndpoint)
{
    if (outEndpoint != nullptr)
    {
        outEndpoint->clear();
    }

    const std::string lowered = ToLowerAscii(text);
    const size_t diedPos = lowered.find(" died");
    if (diedPos == std::string::npos)
    {
        return false;
    }

    const std::string endpoint = TrimAscii(text.substr(0, diedPos));
    if (endpoint.empty())
    {
        return false;
    }

    if (outEndpoint != nullptr)
    {
        *outEndpoint = endpoint;
    }
    return true;
}

bool TryExtractQuitEndpoint(const std::string& text, std::string* outEndpoint)
{
    if (outEndpoint != nullptr)
    {
        outEndpoint->clear();
    }

    static const char* const kQuitPrefixes[] = {
        "Recv quit from:",
        "Received quit from:",
    };

    for (const char* prefix : kQuitPrefixes)
    {
        if (!StartsWithCaseInsensitive(text, prefix))
        {
            continue;
        }

        const std::string endpoint = TrimAscii(text.substr(std::strlen(prefix)));
        if (endpoint.empty())
        {
            return false;
        }

        if (outEndpoint != nullptr)
        {
            *outEndpoint = endpoint;
        }
        return true;
    }

    return false;
}

std::string LoadConfiguredHolePunchServer()
{
    if (g_holePunchServerConfigLoaded)
    {
        return g_configuredHolePunchServer;
    }

    g_holePunchServerConfigLoaded = true;
    g_configuredHolePunchServer.clear();

    char exePath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) == 0)
    {
        return g_configuredHolePunchServer;
    }

    char* slash = std::strrchr(exePath, '\\');
    if (slash == nullptr)
    {
        slash = std::strrchr(exePath, '/');
    }
    if (slash == nullptr)
    {
        return g_configuredHolePunchServer;
    }
    slash[1] = '\0';
    const std::string iniPath = std::string(exePath) + "EfzRevival.ini";

    char serverBuffer[128] = {};
    (void)GetPrivateProfileStringA(
        "Network",
        "HolePunchingServer",
        "",
        serverBuffer,
        static_cast<DWORD>(sizeof(serverBuffer)),
        iniPath.c_str());

    g_configuredHolePunchServer = ToLowerAscii(TrimAscii(serverBuffer));
    if (!g_configuredHolePunchServer.empty())
    {
        mod::Log(
            "Takeover: configured HolePunchingServer='%s'",
            g_configuredHolePunchServer.c_str());
    }
    return g_configuredHolePunchServer;
}

bool ContainsCaseInsensitive(const std::string& text, const char* needle)
{
    if (needle == nullptr || needle[0] == '\0')
    {
        return false;
    }

    const size_t needleLen = std::strlen(needle);
    if (needleLen == 0 || text.size() < needleLen)
    {
        return false;
    }

    for (size_t i = 0; i + needleLen <= text.size(); ++i)
    {
        bool match = true;
        for (size_t j = 0; j < needleLen; ++j)
        {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i + j])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b)
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return true;
        }
    }

    return false;
}

bool ParseIntAt(const std::string& text, size_t start, int* outValue, size_t* outEnd)
{
    if (outValue == nullptr)
    {
        return false;
    }

    size_t i = start;
    while (i < text.size())
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isdigit(c) || c == '-')
        {
            break;
        }
        ++i;
    }
    if (i >= text.size())
    {
        return false;
    }

    int sign = 1;
    if (text[i] == '-')
    {
        sign = -1;
        ++i;
    }
    if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i])))
    {
        return false;
    }

    int value = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])))
    {
        value = value * 10 + (text[i] - '0');
        ++i;
    }

    *outValue = value * sign;
    if (outEnd != nullptr)
    {
        *outEnd = i;
    }
    return true;
}

bool ExtractIntAfterToken(const std::string& text, const char* token, int* outValue)
{
    if (token == nullptr || token[0] == '\0' || outValue == nullptr)
    {
        return false;
    }

    const size_t pos = text.find(token);
    if (pos == std::string::npos)
    {
        return false;
    }
    const size_t start = pos + std::strlen(token);
    return ParseIntAt(text, start, outValue, nullptr);
}

bool ExtractDelayRange(const std::string& text, int* outMin, int* outMax)
{
    if (outMin == nullptr || outMax == nullptr)
    {
        return false;
    }

    const size_t betweenPos = text.find("between");
    if (betweenPos == std::string::npos)
    {
        return false;
    }

    size_t afterMin = 0;
    int minValue = 0;
    if (!ParseIntAt(text, betweenPos + 7, &minValue, &afterMin))
    {
        return false;
    }

    const size_t andPos = text.find("and", afterMin);
    if (andPos == std::string::npos)
    {
        return false;
    }

    int maxValue = 0;
    if (!ParseIntAt(text, andPos + 3, &maxValue, nullptr))
    {
        return false;
    }

    *outMin = minValue;
    *outMax = maxValue;
    return true;
}

DelayPromptMetrics ParseDelayPromptMetricsFromText(const std::string& text, bool* outHasMetrics)
{
    DelayPromptMetrics metrics = {};
    bool hasMetrics = false;
    metrics.averagePingMs = -1;
    metrics.minPingMs = -1;
    metrics.maxPingMs = -1;
    metrics.recommendedDelay = -1;
    metrics.minDelay = -1;
    metrics.maxDelay = -1;
    metrics.inputSerial = 0;
    metrics.inputValue = -1;

    int value = 0;
    if (ExtractIntAfterToken(text, "Average Ping:", &value))
    {
        metrics.averagePingMs = value;
        hasMetrics = true;
    }
    if (ExtractIntAfterToken(text, "Min Ping:", &value))
    {
        metrics.minPingMs = value;
        hasMetrics = true;
    }
    if (ExtractIntAfterToken(text, "Max Ping:", &value))
    {
        metrics.maxPingMs = value;
        hasMetrics = true;
    }
    if (ExtractIntAfterToken(text, "Recommended input delay:", &value))
    {
        metrics.recommendedDelay = value;
        hasMetrics = true;
    }

    int minDelay = -1;
    int maxDelay = -1;
    if (ExtractDelayRange(text, &minDelay, &maxDelay))
    {
        metrics.minDelay = minDelay;
        metrics.maxDelay = maxDelay;
        hasMetrics = true;
    }

    if (outHasMetrics != nullptr)
    {
        *outHasMetrics = hasMetrics;
    }
    return metrics;
}

bool TryParseHostListenerLine(
    const std::string& text,
    network::NetworkFamily* outFamily,
    uint16_t* outPort)
{
    if (outFamily == nullptr || outPort == nullptr)
    {
        return false;
    }

    static constexpr char kIpv4Prefix[] =
        "Hosting using UDP IPv4 on port ";
    static constexpr char kIpv6Prefix[] =
        "Hosting using UDP IPv6 on port ";

    network::NetworkFamily family = network::NetworkFamily::IPv4;
    size_t prefixLength = 0;
    if (text.compare(0, sizeof(kIpv4Prefix) - 1, kIpv4Prefix) == 0)
    {
        family = network::NetworkFamily::IPv4;
        prefixLength = sizeof(kIpv4Prefix) - 1;
    }
    else if (text.compare(0, sizeof(kIpv6Prefix) - 1, kIpv6Prefix) == 0)
    {
        family = network::NetworkFamily::IPv6;
        prefixLength = sizeof(kIpv6Prefix) - 1;
    }
    else
    {
        return false;
    }

    if (text.size() <= prefixLength)
    {
        return false;
    }

    uint32_t port = 0;
    for (size_t index = prefixLength; index < text.size(); ++index)
    {
        const char c = text[index];
        if (c < '0' || c > '9')
        {
            return false;
        }
        port = port * 10u + static_cast<uint32_t>(c - '0');
        if (port > std::numeric_limits<uint16_t>::max())
        {
            return false;
        }
    }

    if (port == 0)
    {
        return false;
    }

    *outFamily = family;
    *outPort = static_cast<uint16_t>(port);
    return true;
}

void PublishDelayPromptMetrics(const DelayPromptMetrics& metrics, LONG serial)
{
    g_delayPromptMetrics = metrics;
    g_delayPromptMetrics.serial = static_cast<int>(serial);

    if (g_injectedBlock != nullptr)
    {
        InterlockedExchange(&g_injectedBlock->delayMetricsSerial, 0);
        g_injectedBlock->delayAveragePingMs = metrics.averagePingMs;
        g_injectedBlock->delayMinPingMs = metrics.minPingMs;
        g_injectedBlock->delayMaxPingMs = metrics.maxPingMs;
        g_injectedBlock->delayRecommended = metrics.recommendedDelay;
        g_injectedBlock->delayRangeMin = metrics.minDelay;
        g_injectedBlock->delayRangeMax = metrics.maxDelay;
        InterlockedExchange(&g_injectedBlock->delayMetricsSerial, serial);
    }
}

static std::string g_delayMetricsAccumulator;

void ResetNativeWorkflowFlags()
{
    g_nativeWorkflowLoadedSeen = false;
    g_nativeWorkflowMatchLoopSeen = false;
    g_nativeWorkflowTournamentSeen = false;
    g_nativeWorkflowPeerDiedSeen = false;
    g_nativeWorkflowHolePunchDiedSeen = false;
    g_holePunchServerConfigLoaded = false;
    g_configuredHolePunchServer.clear();
    g_delayMetricsAccumulator.clear();
}

bool SocketErrorNamesTransientDataPathOp(const std::string& text)
{
    const std::string lowered = ToLowerAscii(text);
    const size_t errorPos = lowered.find("socket error");
    if (errorPos == std::string::npos)
    {
        return false;
    }
    const size_t colonPos = lowered.find(':', errorPos);
    if (colonPos == std::string::npos)
    {
        return false;
    }
    const size_t commaPos = lowered.find(',', colonPos + 1);
    const std::string op = TrimAscii(
        commaPos == std::string::npos
            ? lowered.substr(colonPos + 1)
            : lowered.substr(colonPos + 1, commaPos - (colonPos + 1)));
    static const char* const kTransientDataPathOps[] = {
        "recv",
        "recvfrom",
        "sendto",
        "select",
    };
    for (const char* candidate : kTransientDataPathOps)
    {
        if (op == candidate)
        {
            return true;
        }
    }
    return false;
}

void NoteConsolePromptLine(
    const std::string& text,
    LONG controlWakeRequestSerialSnapshot)
{
    if (text.empty())
    {
        return;
    }

    network::NetworkFamily listenerFamily = network::NetworkFamily::IPv4;
    uint16_t listenerPort = 0;
    if (TryParseHostListenerLine(
            text,
            &listenerFamily,
            &listenerPort))
    {
        PublishHostListenerObservation(listenerFamily, listenerPort);
        mod::Log(
            "Takeover: Revival netplay session listener acknowledged "
            "family=%s port=%u text='%s'",
            network::FamilyName(listenerFamily),
            static_cast<unsigned>(listenerPort),
            text.c_str());
        return;
    }

    if (!g_delayMetricsAccumulator.empty())
    {
        g_delayMetricsAccumulator.push_back('\n');
    }
    g_delayMetricsAccumulator.append(text);
    if (g_delayMetricsAccumulator.size() > 2048)
    {
        g_delayMetricsAccumulator.erase(0, g_delayMetricsAccumulator.size() - 2048);
    }

    if (!g_nativeWorkflowLoadedSeen && ContainsCaseInsensitive(text, "Successfully loaded"))
    {
        g_nativeWorkflowLoadedSeen = true;
        mod::Log("Takeover: native workflow event=helper_loaded text='%s'", text.c_str());
    }
    if (!g_nativeWorkflowMatchLoopSeen
        && ContainsCaseInsensitive(text, "CurrentFrame")
        && ContainsCaseInsensitive(text, "Current State"))
    {
        g_nativeWorkflowMatchLoopSeen = true;
        mod::Log("Takeover: native workflow event=match_loop_started text='%s'", text.c_str());
    }
    if (!g_nativeWorkflowTournamentSeen && ContainsCaseInsensitive(text, "Starting Tournament Mode"))
    {
        g_nativeWorkflowTournamentSeen = true;
        mod::Log("Takeover: native workflow event=unexpected_tournament_mode text='%s'", text.c_str());
    }
    std::string quitEndpoint;
    if (TryExtractQuitEndpoint(text, &quitEndpoint))
    {
        bool isActivePeer = false;
        bool isSpectator = false;
        const bool classified = TryClassifyRevivalQuitEndpoint(
            quitEndpoint.c_str(),
            &isActivePeer,
            &isSpectator);
        mod::Log(
            "Takeover: native workflow event=quit_packet endpoint='%s' classified=%d activePeer=%d spectator=%d text='%s'",
            quitEndpoint.c_str(),
            classified ? 1 : 0,
            isActivePeer ? 1 : 0,
            isSpectator ? 1 : 0,
            text.c_str());
        if (classified && isActivePeer)
        {
            PublishConsoleError(I18N_KEY("Source quit or timed out"));
        }
        return;
    }
    std::string diedEndpoint;
    if (TryExtractDiedEndpoint(text, &diedEndpoint))
    {
        const std::string configuredHolePunch = LoadConfiguredHolePunchServer();
        const bool isHolePunchDeath =
            !configuredHolePunch.empty()
            && ToLowerAscii(TrimAscii(diedEndpoint)) == configuredHolePunch;
        if (isHolePunchDeath)
        {
            if (!g_nativeWorkflowHolePunchDiedSeen)
            {
                g_nativeWorkflowHolePunchDiedSeen = true;
                mod::Log(
                    "Takeover: native workflow event=hole_punch_server_died endpoint='%s' text='%s'",
                    diedEndpoint.c_str(),
                    text.c_str());
            }
        }
        else if (!g_nativeWorkflowPeerDiedSeen)
        {
            g_nativeWorkflowPeerDiedSeen = true;
            mod::Log(
                "Takeover: native workflow event=peer_died endpoint='%s' text='%s'",
                diedEndpoint.c_str(),
                text.c_str());
        }
    }

    // More specific needles must precede more general ones.
    struct ConsoleErrorMapping
    {
        const char* needle;
        const char* friendly;
    };
    static const ConsoleErrorMapping kConsoleErrorMap[] = {
        {"header crc mismatch", I18N_KEY("Desync detected by Revival (header CRC mismatch).")},
        {"state not recoverable", I18N_KEY("Desync detected by Revival (state not recoverable).")},
        {"Connection timed out", I18N_KEY("Connection timed out.")},
        {"Source quit or timed out", I18N_KEY("Opponent quit or timed out.")},
        {"Host timed out", I18N_KEY("Host timed out - no one connected.")},
        {"Remote timed out", I18N_KEY("Opponent quit or timed out.")},
        {"Invalid address", I18N_KEY("Invalid address - check the IP and port.")},
        {"Only one net instance allowed", I18N_KEY("A netplay session is already running.")},
        {"Remote is using a deprecated version", I18N_KEY("Opponent is on an outdated Revival version.")},
        {"New version available", I18N_KEY("A new Revival version is available - please update.")},
        {"Invalid delay", I18N_KEY("Invalid delay value.")},
        {"Already playing, spectators have been disabled", I18N_KEY("Host is already playing; spectators are disabled.")},
        {"Spectators have been disabled", I18N_KEY("Spectators have been disabled by the host.")},
    };
    for (const ConsoleErrorMapping& mapping : kConsoleErrorMap)
    {
        if (ContainsCaseInsensitive(text, mapping.needle))
        {
            if (std::strcmp(mapping.needle, "Remote timed out") == 0
                && TryPublishHeldHostDelayTimeout(
                    controlWakeRequestSerialSnapshot))
            {
                mod::Log(
                    "Takeover: native held-delay timeout observed; "
                    "deferring console error until reader cleanup text='%s'",
                    text.c_str());
                return;
            }
            mod::Log("Takeover: console error detected='%s' text='%s'", mapping.needle, text.c_str());
            PublishConsoleError(mapping.friendly);
            return;
        }
    }
    if (ContainsCaseInsensitive(text, "Socket error"))
    {
        if (SocketErrorNamesTransientDataPathOp(text))
        {
            static volatile LONG s_lastTransientSocketErrorLogTick = 0;
            const DWORD nowTick = GetTickCount();
            const LONG previous = InterlockedCompareExchange(
                &s_lastTransientSocketErrorLogTick, 0, 0);
            if (previous == 0
                || static_cast<DWORD>(nowTick - static_cast<DWORD>(previous)) >= 1000u)
            {
                InterlockedExchange(
                    &s_lastTransientSocketErrorLogTick,
                    static_cast<LONG>(nowTick));
                mod::Log(
                    "Takeover: transient data-path socket error ignored "
                    "(Revival self-recovers or times out on its own) text='%s'",
                    text.c_str());
            }
            return;
        }
        mod::Log("Takeover: console error detected='Socket error' text='%s'", text.c_str());
        std::string errorMsg = text;
        if (errorMsg.size() > 120)
        {
            errorMsg.resize(120);
        }
        PublishConsoleError(errorMsg.c_str());
        return;
    }

    const bool isJoinAsSpectatorPrompt =
        ContainsCaseInsensitive(text, "Host already playing, join as a spectator");
    if (isJoinAsSpectatorPrompt)
    {
        const LONG serial = InterlockedIncrement(&g_injectedSpectateConfirmPromptSerial);
        PublishSpectateConfirmPromptSerial(
            serial,
            static_cast<int>(NetbridgeSpectatePromptKind::HostAlreadyPlaying));
        g_injectedSpectateConfirmPromptWaitStartTick = GetTickCount();
        mod::Log(
            "Takeover: console prompt detected type=join_as_spectator serial=%ld text='%s'",
            static_cast<long>(serial),
            text.c_str());
        return;
    }

    const bool isHostNotYetPlayingPrompt =
        ContainsCaseInsensitive(text, "Host not yet playing, join as a player");
    if (isHostNotYetPlayingPrompt)
    {
        const LONG serial = InterlockedIncrement(&g_injectedSpectateConfirmPromptSerial);
        PublishSpectateConfirmPromptSerial(
            serial,
            static_cast<int>(NetbridgeSpectatePromptKind::HostNotYetPlaying));
        g_injectedSpectateConfirmPromptWaitStartTick = GetTickCount();
        mod::Log(
            "Takeover: console prompt detected type=host_not_yet_playing serial=%ld text='%s'",
            static_cast<long>(serial),
            text.c_str());
        return;
    }

    if (ContainsCaseInsensitive(text, "Waiting for game to begin"))
    {
        mod::Log(
            "Takeover: console state detected type=waiting_for_game_begin text='%s'",
            text.c_str());
        return;
    }

    const bool isDelayPrompt =
        ContainsCaseInsensitive(text, "Enter the initial input delay")
        || ContainsCaseInsensitive(text, "Enter the input delay to use");

    if (!isDelayPrompt)
    {
        static DWORD s_lastUnknownSpectateConsoleLogTick = 0;
        static std::string s_lastUnknownSpectateConsoleLine;
        const DWORD now = GetTickCount();
        const bool spectateConsoleActive =
            g_revivalProcess != nullptr
            && g_hostBlock != nullptr
            && g_hostBlock->initParams[0] == kLocalRoleSpectate;
        const bool shouldLogUnknownSpectateLine =
            spectateConsoleActive
            && (!s_lastUnknownSpectateConsoleLine.empty()
                ? s_lastUnknownSpectateConsoleLine != text || now - s_lastUnknownSpectateConsoleLogTick >= 3000
                : true);
        if (shouldLogUnknownSpectateLine)
        {
            s_lastUnknownSpectateConsoleLogTick = now;
            s_lastUnknownSpectateConsoleLine = text;
            mod::Log(
                "Takeover: spectate console raw line='%s'",
                text.c_str());
        }
        return;
    }

    if (IsDelayPromptTerminated(g_injectedBlock != nullptr ? g_injectedBlock : g_hostBlock))
    {
        g_delayMetricsAccumulator.clear();
        return;
    }

    bool hasMetrics = false;
    DelayPromptMetrics metrics = ParseDelayPromptMetricsFromText(
        g_delayMetricsAccumulator, &hasMetrics);
    g_delayMetricsAccumulator.clear();
    if (ContainsCaseInsensitive(text, "Enter the input delay to use")
        && g_delayPromptMetrics.serial > 0)
    {
        if (metrics.averagePingMs < 0)
        {
            metrics.averagePingMs = g_delayPromptMetrics.averagePingMs;
        }
        if (metrics.minPingMs < 0)
        {
            metrics.minPingMs = g_delayPromptMetrics.minPingMs;
        }
        if (metrics.maxPingMs < 0)
        {
            metrics.maxPingMs = g_delayPromptMetrics.maxPingMs;
        }
        if (metrics.recommendedDelay < 0)
        {
            metrics.recommendedDelay = g_delayPromptMetrics.recommendedDelay;
        }
    }
    const LONG serial = InterlockedIncrement(&g_injectedDelayPromptSerial);
    PublishDelayPromptMetrics(metrics, serial);
    PublishDelayPromptSerial(
        serial,
        controlWakeRequestSerialSnapshot);
    if (hasMetrics)
    {
        mod::Log(
            "Takeover: console prompt detected type=delay serial=%ld avg=%d minPing=%d maxPing=%d rec=%d range=%d..%d text='%s'",
            static_cast<long>(serial),
            metrics.averagePingMs,
            metrics.minPingMs,
            metrics.maxPingMs,
            metrics.recommendedDelay,
            metrics.minDelay,
            metrics.maxDelay,
            text.c_str());
        return;
    }
    mod::Log(
        "Takeover: console prompt detected type=delay serial=%ld text='%s'",
        static_cast<long>(serial),
        text.c_str());
}

std::string* SelectPendingConsoleLine(const char* sourceTag)
{
    if (sourceTag == nullptr)
    {
        return nullptr;
    }
    if (std::strcmp(sourceTag, "WriteFile") == 0)
    {
        return &g_consolePendingWriteFile;
    }
    if (std::strcmp(sourceTag, "WriteFileDisk") == 0)
    {
        return &g_consolePendingWriteFileDisk;
    }
    if (std::strcmp(sourceTag, "WriteConsoleA") == 0)
    {
        return &g_consolePendingWriteConsoleA;
    }
    if (std::strcmp(sourceTag, "WriteConsoleW") == 0)
    {
        return &g_consolePendingWriteConsoleW;
    }
    if (std::strcmp(sourceTag, "WriteConsoleOutputCharacterA") == 0)
    {
        return &g_consolePendingWriteConsoleOutputCharacterA;
    }
    if (std::strcmp(sourceTag, "WriteConsoleOutputCharacterW") == 0)
    {
        return &g_consolePendingWriteConsoleOutputCharacterW;
    }
    if (std::strcmp(sourceTag, "OutputDebugStringA") == 0)
    {
        return &g_consolePendingOutputDebugStringA;
    }
    if (std::strcmp(sourceTag, "OutputDebugStringW") == 0)
    {
        return &g_consolePendingOutputDebugStringW;
    }
    return nullptr;
}

bool IsLikelyRevivalDiskLogPath(const std::string& path)
{
    if (path.empty())
    {
        return false;
    }

    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    auto contains = [&](const char* needle) -> bool {
        return lower.find(needle) != std::string::npos;
    };

    const bool looksTextFile =
        contains(".log")
        || contains(".txt")
        || contains(".ini");
    const bool looksRevivalOwned =
        contains("efzrevival")
        || contains("revival")
        || contains("protobuf");

    return looksRevivalOwned || looksTextFile;
}

static bool EnqueueConsoleParseChunk(
    const char* sourceTag,
    const void* data,
    size_t bytes,
    bool wide,
    LONG controlWakeRequestSerialSnapshot);
static bool EnqueueConsoleParseFlush(bool waitForDrain);
static void ProcessConsoleTextChunk(
    const char* sourceTag,
    const char* text,
    size_t length,
    LONG controlWakeRequestSerialSnapshot);

static LONG CaptureControlWakeRequestSerialSnapshot()
{
    auto capture = [](SharedBlock* block) -> LONG
    {
        if (block == nullptr
            || block->magic != kIpcMagic
            || block->version != kIpcVersion
            || block->hostPid == 0)
        {
            return -1;
        }
        return InterlockedCompareExchange(
            &block->consoleControlWakeRequestSerial,
            0,
            0);
    };

    const LONG injected = capture(g_injectedBlock);
    if (injected >= 0)
    {
        return injected;
    }
    return capture(g_hostBlock);
}

void LogConsoleTextChunk(const char* sourceTag, const char* text, size_t length)
{
    if (sourceTag == nullptr)
    {
        sourceTag = "unknown";
    }
    if (text == nullptr || length == 0)
    {
        return;
    }
    const LONG controlWakeRequestSerialSnapshot =
        CaptureControlWakeRequestSerialSnapshot();
    (void)EnqueueConsoleParseChunk(
        sourceTag,
        text,
        length,
        false,
        controlWakeRequestSerialSnapshot);
}

static std::string ConvertConsoleWideToUtf8(const wchar_t* wideText, int wideLen)
{
    if (wideText == nullptr || wideLen <= 0)
    {
        return {};
    }
    int utf8Bytes = WideCharToMultiByte(
        CP_UTF8, 0, wideText, wideLen, nullptr, 0, nullptr, nullptr);
    UINT codePage = CP_UTF8;
    if (utf8Bytes <= 0)
    {
        codePage = CP_ACP;
        utf8Bytes = WideCharToMultiByte(
            codePage, 0, wideText, wideLen, nullptr, 0, nullptr, nullptr);
    }
    if (utf8Bytes <= 0)
    {
        return {};
    }
    std::string utf8;
    utf8.resize(static_cast<size_t>(utf8Bytes));
    if (WideCharToMultiByte(
            codePage, 0, wideText, wideLen, utf8.data(), utf8Bytes, nullptr, nullptr)
        <= 0)
    {
        return {};
    }
    return utf8;
}

static void LogConsoleWideTextChunk(
    const char* sourceTag, const wchar_t* wideText, int wideLen)
{
    if (sourceTag == nullptr)
    {
        sourceTag = "unknown";
    }
    if (wideText == nullptr || wideLen <= 0)
    {
        return;
    }
    const LONG controlWakeRequestSerialSnapshot =
        CaptureControlWakeRequestSerialSnapshot();
    const size_t wideChars = static_cast<size_t>(wideLen);
    const size_t bytes = wideChars > (std::numeric_limits<size_t>::max)() / sizeof(wchar_t)
        ? (std::numeric_limits<size_t>::max)()
        : wideChars * sizeof(wchar_t);
    (void)EnqueueConsoleParseChunk(
        sourceTag,
        wideText,
        bytes,
        true,
        controlWakeRequestSerialSnapshot);
}

struct PendingConsoleWakeSnapshot
{
    std::string* line;
    LONG serial;
};

static PendingConsoleWakeSnapshot g_pendingConsoleWakeSnapshots[] = {
    {&g_consolePendingWriteFile, -1},
    {&g_consolePendingWriteFileDisk, -1},
    {&g_consolePendingWriteConsoleA, -1},
    {&g_consolePendingWriteConsoleW, -1},
    {&g_consolePendingWriteConsoleOutputCharacterA, -1},
    {&g_consolePendingWriteConsoleOutputCharacterW, -1},
    {&g_consolePendingOutputDebugStringA, -1},
    {&g_consolePendingOutputDebugStringW, -1},
};

static LONG* SelectPendingConsoleWakeSnapshot(std::string* line)
{
    for (auto& snapshot : g_pendingConsoleWakeSnapshots)
    {
        if (snapshot.line == line)
        {
            return &snapshot.serial;
        }
    }
    return nullptr;
}

static void ProcessConsoleTextChunk(
    const char* sourceTag,
    const char* text,
    size_t length,
    LONG controlWakeRequestSerialSnapshot)
{
    if (sourceTag == nullptr)
    {
        sourceTag = "unknown";
    }
    if (text == nullptr || length == 0)
    {
        return;
    }
    if (!IsLikelyTextChunk(text, length))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_consoleLogMutex);
    std::string localLine;
    std::string* line = SelectPendingConsoleLine(sourceTag);
    if (line == nullptr)
    {
        line = &localLine;
    }
    if (line->capacity() < 256)
    {
        line->reserve(256);
    }

    LONG localWakeSnapshot = -1;
    LONG* wakeSnapshot = SelectPendingConsoleWakeSnapshot(line);
    if (wakeSnapshot == nullptr)
    {
        wakeSnapshot = &localWakeSnapshot;
    }
    auto flushLine = [&](bool) {
        const std::string trimmed = TrimAscii(*line);
        line->clear();
        const LONG rawWriteWakeSnapshot = *wakeSnapshot;
        *wakeSnapshot = -1;
        if (trimmed.empty())
        {
            return;
        }
        NoteConsolePromptLine(
            trimmed,
            rawWriteWakeSnapshot);
    };

    for (size_t i = 0; i < length; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\0')
        {
            break;
        }
        if (c == '\r' || c == '\n')
        {
            flushLine(false);
            continue;
        }
        if (line->empty())
        {
            *wakeSnapshot = controlWakeRequestSerialSnapshot;
        }
        if (c == '\t')
        {
            line->push_back(' ');
        }
        else if ((c >= 0x20 && c <= 0x7E) || c >= 0x80)
        {
            line->push_back(static_cast<char>(c));
        }
        else
        {
            line->push_back(' ');
        }

        if (line->size() >= 240)
        {
            flushLine(true);
        }
    }

    // Lines complete only via the synthetic newline; flushing "10" of "10800" is unsafe.
    if (line != nullptr && !line->empty() && line->size() >= 12)
    {
        static const char* kImmediateFlushKeywords[] = {
            "header crc mismatch",
            "state not recoverable",
            "Connection timed out",
            "Source quit or timed out",
            "Host timed out",
            "Remote timed out",
            "Spectators have been disabled",
            "Socket error",
            "Host already playing, join as a spectator",
            "Host not yet playing, join as a player",
            "Waiting for game to begin",
        };
        for (const char* kw : kImmediateFlushKeywords)
        {
            if (ContainsCaseInsensitive(*line, kw))
            {
                flushLine(true);
                break;
            }
        }
    }

    if (SelectPendingConsoleLine(sourceTag) == nullptr && !line->empty())
    {
        flushLine(true);
    }
}

static void FlushPendingConsoleOutputLocked()
{
    auto flushOne = [&](std::string* line) {
        if (line == nullptr || line->empty())
        {
            return;
        }

        const std::string trimmed = TrimAscii(*line);
        line->clear();
        if (trimmed.empty())
        {
            return;
        }

        LONG* wakeSnapshot = SelectPendingConsoleWakeSnapshot(line);
        const LONG rawWriteWakeSnapshot = wakeSnapshot != nullptr ? *wakeSnapshot : -1;
        if (wakeSnapshot != nullptr)
        {
            *wakeSnapshot = -1;
        }
        NoteConsolePromptLine(trimmed, rawWriteWakeSnapshot);
    };

    flushOne(&g_consolePendingWriteFile);
    flushOne(&g_consolePendingWriteFileDisk);
    flushOne(&g_consolePendingWriteConsoleA);
    flushOne(&g_consolePendingWriteConsoleW);
    flushOne(&g_consolePendingWriteConsoleOutputCharacterA);
    flushOne(&g_consolePendingWriteConsoleOutputCharacterW);
    flushOne(&g_consolePendingOutputDebugStringA);
    flushOne(&g_consolePendingOutputDebugStringW);
}

struct OwnedLogEfzEntry
{
    enum class Kind
    {
        CloseBoundary,
        ParseFlush,
    } kind = Kind::ParseFlush;
    unsigned long long sequence = 0;
    size_t ingressBoundary = 0;
    unsigned long ingressLossGeneration = 0;
};

constexpr size_t kConsoleIngressCapacity = 256;
constexpr size_t kConsoleIngressPayloadBytes = 16384;
constexpr size_t kConsoleIngressSourceTagBytes = 40;
static_assert(
    (kConsoleIngressCapacity & (kConsoleIngressCapacity - 1)) == 0,
    "console ingress capacity must be a power of two");

struct ConsoleIngressSlot
{
    std::atomic<size_t> sequence;
    char sourceTag[kConsoleIngressSourceTagBytes] = {};
    alignas(wchar_t) unsigned char payload[kConsoleIngressPayloadBytes] = {};
    size_t bytes = 0;
    bool wide = false;
    LONG controlWakeRequestSerialSnapshot = -1;
    unsigned long lossGeneration = 0;
};

static std::array<ConsoleIngressSlot, kConsoleIngressCapacity> g_consoleIngress;
static std::atomic<size_t> g_consoleIngressEnqueuePosition{0};
static size_t g_consoleIngressDequeuePosition = 0;
static std::atomic<bool> g_consoleIngressAccepting{false};
static std::atomic<bool> g_consoleIngressPollingEnabled{false};
static std::atomic<unsigned long> g_consoleIngressActiveProducers{0};
static std::atomic<unsigned long> g_consoleIngressFullDrops{0};
static std::atomic<unsigned long> g_consoleIngressContentionDrops{0};
static std::atomic<unsigned long> g_consoleIngressOversizeDrops{0};
static std::atomic<unsigned long> g_consoleIngressUnavailableDrops{0};
static std::atomic<unsigned long> g_consoleIngressLossGeneration{0};
static unsigned long g_consoleIngressConsumedLossGeneration = 0;

static std::mutex g_ownedLogEfzQueueMutex;
static std::condition_variable g_ownedLogEfzQueueCv;
static std::condition_variable g_ownedLogEfzDrainCv;
static std::deque<OwnedLogEfzEntry> g_ownedLogEfzQueue;
static std::thread g_ownedLogEfzWorker;
static console_capture::WorkerLifecycle g_consoleWorker;
static volatile LONG g_ownedLogEfzEmergencyStop = 0;
static unsigned long long g_ownedLogEfzEnqueuedSequence = 0;
static unsigned long long g_ownedLogEfzProcessedSequence = 0;
static volatile DWORD g_ownedLogEfzWorkerThreadId = 0;

bool StartConsoleCaptureWorker(bool enableRawIngress);

static void ResetConsoleIngress()
{
    g_consoleIngressAccepting.store(false, std::memory_order_release);
    g_consoleIngressEnqueuePosition.store(0, std::memory_order_relaxed);
    g_consoleIngressDequeuePosition = 0;
    g_consoleIngressConsumedLossGeneration =
        g_consoleIngressLossGeneration.load(std::memory_order_acquire);
    for (size_t index = 0; index < kConsoleIngressCapacity; ++index)
    {
        g_consoleIngress[index].sequence.store(index, std::memory_order_relaxed);
    }
}

static size_t CaptureConsoleIngressBoundary()
{
    return g_consoleIngressEnqueuePosition.load(std::memory_order_acquire);
}

static bool PauseConsoleIngress()
{
    const bool wasAccepting =
        g_consoleIngressAccepting.exchange(false, std::memory_order_acq_rel);
    while (g_consoleIngressActiveProducers.load(std::memory_order_acquire) != 0)
    {
        std::this_thread::yield();
    }
    return wasAccepting;
}

static bool ConsoleIngressHasReadyRecord()
{
    const size_t position = g_consoleIngressDequeuePosition;
    const ConsoleIngressSlot& slot =
        g_consoleIngress[position & (kConsoleIngressCapacity - 1)];
    return slot.sequence.load(std::memory_order_acquire) == position + 1;
}

static ConsoleIngressSlot* TryAcquireConsoleIngressRecord(size_t* outPosition)
{
    const size_t position = g_consoleIngressDequeuePosition;
    ConsoleIngressSlot& slot =
        g_consoleIngress[position & (kConsoleIngressCapacity - 1)];
    if (slot.sequence.load(std::memory_order_acquire) != position + 1)
    {
        return nullptr;
    }
    if (outPosition != nullptr)
    {
        *outPosition = position;
    }
    return &slot;
}

static void ReleaseConsoleIngressRecord(
    ConsoleIngressSlot* slot,
    size_t position)
{
    if (slot == nullptr)
    {
        return;
    }
    slot->sequence.store(
        position + kConsoleIngressCapacity,
        std::memory_order_release);
    g_consoleIngressDequeuePosition = position + 1;
}

static void DiscardPendingConsoleFragmentsLocked()
{
    g_consolePendingWriteFile.clear();
    g_consolePendingWriteFileDisk.clear();
    g_consolePendingWriteConsoleA.clear();
    g_consolePendingWriteConsoleW.clear();
    g_consolePendingWriteConsoleOutputCharacterA.clear();
    g_consolePendingWriteConsoleOutputCharacterW.clear();
    g_consolePendingOutputDebugStringA.clear();
    g_consolePendingOutputDebugStringW.clear();
    for (auto& snapshot : g_pendingConsoleWakeSnapshots)
    {
        snapshot.serial = -1;
    }
}

static void ReportConsoleIngressDropsOnWorker()
{
    const unsigned long full =
        g_consoleIngressFullDrops.exchange(0, std::memory_order_acq_rel);
    const unsigned long contention =
        g_consoleIngressContentionDrops.exchange(0, std::memory_order_acq_rel);
    const unsigned long oversized =
        g_consoleIngressOversizeDrops.exchange(0, std::memory_order_acq_rel);
    const unsigned long unavailable =
        g_consoleIngressUnavailableDrops.exchange(0, std::memory_order_acq_rel);
    if (full == 0 && contention == 0 && oversized == 0 && unavailable == 0)
    {
        return;
    }

    mod::Log(
        "CAPTURE_LOG: console ingress dropped writes full=%lu contention=%lu "
        "oversized=%lu unavailable=%lu; no synchronous fallback",
        full,
        contention,
        oversized,
        unavailable);
}

static void ApplyConsoleIngressLossBoundary(unsigned long lossGeneration)
{
    if (lossGeneration == g_consoleIngressConsumedLossGeneration)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> parserLock(g_consoleLogMutex);
        DiscardPendingConsoleFragmentsLocked();
    }
    g_consoleIngressConsumedLossGeneration = lossGeneration;
}



static void ProcessConsoleIngressRecord(const ConsoleIngressSlot& slot)
{
    const char* sourceTag = slot.sourceTag[0] != '\0'
        ? slot.sourceTag
        : "unknown";
    if (slot.wide)
    {
        const std::string utf8 = ConvertConsoleWideToUtf8(
            reinterpret_cast<const wchar_t*>(slot.payload),
            static_cast<int>(slot.bytes / sizeof(wchar_t)));
        if (!utf8.empty() && IsLikelyTextChunk(utf8.c_str(), utf8.size()))
        {
            ProcessConsoleTextChunk(
                sourceTag,
                utf8.c_str(),
                utf8.size(),
                slot.controlWakeRequestSerialSnapshot);
        }
        return;
    }

    const char* text = reinterpret_cast<const char*>(slot.payload);
    if (IsLikelyTextChunk(text, slot.bytes))
    {
        ProcessConsoleTextChunk(
            sourceTag,
            text,
            slot.bytes,
            slot.controlWakeRequestSerialSnapshot);
    }
}

static void ManagedLogEfzWorkerMain()
{
    g_ownedLogEfzWorkerThreadId = GetCurrentThreadId();
    for (;;)
    {
        ReportConsoleIngressDropsOnWorker();

        OwnedLogEfzEntry entry;
        bool haveQueueEntry = false;
        ConsoleIngressSlot* ingressSlot = nullptr;
        size_t ingressPosition = 0;
        {
            std::unique_lock<std::mutex> lock(g_ownedLogEfzQueueMutex);
            const auto wakePredicate = [] {
                    return g_consoleWorker.Stopping()
                        || InterlockedCompareExchange(
                               &g_ownedLogEfzEmergencyStop, 0, 0) != 0
                        || (!g_ownedLogEfzQueue.empty()
                            && g_consoleIngressDequeuePosition
                                >= g_ownedLogEfzQueue.front().ingressBoundary)
                        || (g_consoleIngressPollingEnabled.load(
                                std::memory_order_acquire)
                            && ConsoleIngressHasReadyRecord());
                };
            if (g_consoleIngressPollingEnabled.load(std::memory_order_acquire))
            {
                g_ownedLogEfzQueueCv.wait_for(
                    lock, std::chrono::milliseconds(1), wakePredicate);
            }
            else
            {
                g_ownedLogEfzQueueCv.wait(lock, wakePredicate);
            }
            if (InterlockedCompareExchange(
                    &g_ownedLogEfzEmergencyStop, 0, 0) != 0)
            {
                break;
            }

            if (!g_ownedLogEfzQueue.empty()
                && g_consoleIngressDequeuePosition
                    >= g_ownedLogEfzQueue.front().ingressBoundary)
            {
                entry = std::move(g_ownedLogEfzQueue.front());
                g_ownedLogEfzQueue.pop_front();
                haveQueueEntry = true;
            }
            else
            {
                ingressSlot =
                    TryAcquireConsoleIngressRecord(&ingressPosition);
                if (ingressSlot == nullptr)
                {
                    if (g_consoleWorker.Stopping()
                        && g_ownedLogEfzQueue.empty()
                        && g_consoleIngressDequeuePosition
                            >= CaptureConsoleIngressBoundary())
                    {
                        break;
                    }
                    continue;
                }
            }
        }

        if (ingressSlot != nullptr)
        {
            ApplyConsoleIngressLossBoundary(ingressSlot->lossGeneration);
            ProcessConsoleIngressRecord(*ingressSlot);
            ReleaseConsoleIngressRecord(ingressSlot, ingressPosition);
            continue;
        }
        if (!haveQueueEntry)
        {
            continue;
        }

        ApplyConsoleIngressLossBoundary(entry.ingressLossGeneration);

        if (entry.kind == OwnedLogEfzEntry::Kind::ParseFlush)
        {
            std::lock_guard<std::mutex> parserLock(g_consoleLogMutex);
            FlushPendingConsoleOutputLocked();
        }

        {
            std::lock_guard<std::mutex> lock(g_ownedLogEfzQueueMutex);
            g_ownedLogEfzProcessedSequence = entry.sequence;
        }
        g_ownedLogEfzDrainCv.notify_all();
    }

    ReportConsoleIngressDropsOnWorker();
    g_ownedLogEfzDrainCv.notify_all();
}

bool StartConsoleCaptureWorker(bool enableRawIngress)
{
    std::lock_guard<std::mutex> lock(g_ownedLogEfzQueueMutex);
    if (g_consoleWorker.HasWorker())
    {
        return !enableRawIngress
            || g_consoleIngressAccepting.load(std::memory_order_acquire);
    }

    g_consoleIngressPollingEnabled.store(
        enableRawIngress, std::memory_order_release);
    ResetConsoleIngress();
    g_consoleWorker.Joined();
    InterlockedExchange(&g_ownedLogEfzEmergencyStop, 0);
    try
    {
        g_ownedLogEfzWorker = std::thread(ManagedLogEfzWorkerMain);
        g_consoleWorker.Started();
        g_consoleIngressAccepting.store(
            enableRawIngress, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        g_consoleWorker.Joined();
        g_consoleIngressAccepting.store(false, std::memory_order_release);
        g_consoleIngressPollingEnabled.store(false, std::memory_order_release);
        mod::Log(
            "CAPTURE_LOG: failed to start console parser worker; "
            "raw capture disabled");
        return false;
    }
}

static void CopyConsoleIngressSourceTag(char* destination, const char* sourceTag)
{
    if (sourceTag == nullptr)
    {
        sourceTag = "unknown";
    }
    size_t length = 0;
    while (length + 1 < kConsoleIngressSourceTagBytes
        && sourceTag[length] != '\0')
    {
        ++length;
    }
    if (length != 0)
    {
        std::memcpy(destination, sourceTag, length);
    }
    destination[length] = '\0';
}

static bool EnqueueConsoleParseChunk(
    const char* sourceTag,
    const void* data,
    size_t bytes,
    bool wide,
    LONG controlWakeRequestSerialSnapshot)
{
    if (data == nullptr || bytes == 0)
    {
        return true;
    }

    g_consoleIngressActiveProducers.fetch_add(1, std::memory_order_acquire);
    if (!g_consoleIngressAccepting.load(std::memory_order_acquire))
    {
        g_consoleIngressUnavailableDrops.fetch_add(1, std::memory_order_relaxed);
        g_consoleIngressLossGeneration.fetch_add(1, std::memory_order_release);
        g_consoleIngressActiveProducers.fetch_sub(1, std::memory_order_release);
        return false;
    }
    if (bytes > kConsoleIngressPayloadBytes
        || (wide && (bytes % sizeof(wchar_t)) != 0))
    {
        g_consoleIngressOversizeDrops.fetch_add(1, std::memory_order_relaxed);
        g_consoleIngressLossGeneration.fetch_add(1, std::memory_order_release);
        g_consoleIngressActiveProducers.fetch_sub(1, std::memory_order_release);
        return false;
    }

    size_t position =
        g_consoleIngressEnqueuePosition.load(std::memory_order_relaxed);
    ConsoleIngressSlot& slot =
        g_consoleIngress[position & (kConsoleIngressCapacity - 1)];
    if (slot.sequence.load(std::memory_order_acquire) != position)
    {
        g_consoleIngressFullDrops.fetch_add(1, std::memory_order_relaxed);
        g_consoleIngressLossGeneration.fetch_add(1, std::memory_order_release);
        g_consoleIngressActiveProducers.fetch_sub(1, std::memory_order_release);
        return false;
    }
    if (!g_consoleIngressEnqueuePosition.compare_exchange_strong(
            position,
            position + 1,
            std::memory_order_relaxed,
            std::memory_order_relaxed))
    {
        g_consoleIngressContentionDrops.fetch_add(1, std::memory_order_relaxed);
        g_consoleIngressLossGeneration.fetch_add(1, std::memory_order_release);
        g_consoleIngressActiveProducers.fetch_sub(1, std::memory_order_release);
        return false;
    }

    CopyConsoleIngressSourceTag(slot.sourceTag, sourceTag);
    std::memcpy(slot.payload, data, bytes);
    slot.bytes = bytes;
    slot.wide = wide;
    slot.controlWakeRequestSerialSnapshot =
        controlWakeRequestSerialSnapshot;
    slot.lossGeneration =
        g_consoleIngressLossGeneration.load(std::memory_order_acquire);
    slot.sequence.store(position + 1, std::memory_order_release);
    g_consoleIngressActiveProducers.fetch_sub(1, std::memory_order_release);
    return true;
}

static bool EnqueueConsoleParseFlush(bool waitForDrain)
{
    unsigned long long flushSequence = 0;
    OwnedLogEfzEntry entry;
    entry.kind = OwnedLogEfzEntry::Kind::ParseFlush;

    {
        std::lock_guard<std::mutex> lock(g_ownedLogEfzQueueMutex);
        if (!g_consoleWorker.Accepting()
            || InterlockedCompareExchange(
                   &g_ownedLogEfzEmergencyStop, 0, 0) != 0)
        {
            return false;
        }

        const bool resumeIngress = PauseConsoleIngress();
        entry.ingressBoundary = CaptureConsoleIngressBoundary();
        entry.ingressLossGeneration =
            g_consoleIngressLossGeneration.load(std::memory_order_acquire);
        entry.sequence = ++g_ownedLogEfzEnqueuedSequence;
        flushSequence = entry.sequence;
        try
        {
            g_ownedLogEfzQueue.push_back(std::move(entry));
        }
        catch (...)
        {
            if (resumeIngress)
            {
                g_consoleIngressAccepting.store(true, std::memory_order_release);
            }
            return false;
        }
        if (resumeIngress)
        {
            g_consoleIngressAccepting.store(true, std::memory_order_release);
        }
    }
    g_ownedLogEfzQueueCv.notify_one();
    if (waitForDrain)
    {
        std::unique_lock<std::mutex> lock(g_ownedLogEfzQueueMutex);
        g_ownedLogEfzDrainCv.wait(lock, [flushSequence] {
            return g_ownedLogEfzProcessedSequence >= flushSequence
                || !g_consoleWorker.HasWorker()
                || InterlockedCompareExchange(
                       &g_ownedLogEfzEmergencyStop, 0, 0) != 0;
        });
        return g_ownedLogEfzProcessedSequence >= flushSequence;
    }
    return true;
}

bool FlushPendingConsoleOutput(const char*, bool waitForDrain)
{
    if (waitForDrain && GetCurrentThreadId() == g_ownedLogEfzWorkerThreadId)
    {
        return false;
    }
    if (EnqueueConsoleParseFlush(waitForDrain))
    {
        return true;
    }
    if (waitForDrain)
    {
        std::lock_guard<std::mutex> lock(g_ownedLogEfzQueueMutex);
        if (g_consoleWorker.HasWorker()
            || InterlockedCompareExchange(&g_ownedLogEfzEmergencyStop, 0, 0) != 0)
        {
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(g_consoleLogMutex);
    FlushPendingConsoleOutputLocked();
    return true;
}

void StopManagedLogEfzWorker(bool waitForDrain)
{
    if (!waitForDrain)
    {
        // Loader-lock/termination path: never take a mutex a suspended thread could own, never wait on disk I/O.
        g_consoleIngressAccepting.store(false, std::memory_order_release);
        g_consoleIngressPollingEnabled.store(false, std::memory_order_release);
        InterlockedExchange(&g_ownedLogEfzEmergencyStop, 1);
        g_ownedLogEfzQueueCv.notify_all();
        g_ownedLogEfzDrainCv.notify_all();
        if (g_ownedLogEfzWorker.joinable())
        {
            g_ownedLogEfzWorker.detach();
        }
        return;
    }

    bool joinWorker = false;
    unsigned long long closeSequence = 0;
    {
        std::lock_guard<std::mutex> lock(g_ownedLogEfzQueueMutex);
        if (!g_consoleWorker.HasWorker())
        {
            g_consoleIngressAccepting.store(false, std::memory_order_release);
            return;
        }
        g_consoleWorker.BeginDrain();
        (void)PauseConsoleIngress();
        OwnedLogEfzEntry closeEntry;
        closeEntry.kind = OwnedLogEfzEntry::Kind::CloseBoundary;
        closeEntry.ingressBoundary = CaptureConsoleIngressBoundary();
        closeEntry.ingressLossGeneration =
            g_consoleIngressLossGeneration.load(std::memory_order_acquire);
        closeEntry.sequence = ++g_ownedLogEfzEnqueuedSequence;
        closeSequence = closeEntry.sequence;
        g_ownedLogEfzQueue.push_back(std::move(closeEntry));
    }
    g_ownedLogEfzQueueCv.notify_all();
    {
        std::unique_lock<std::mutex> lock(g_ownedLogEfzQueueMutex);
        g_ownedLogEfzDrainCv.wait(lock, [closeSequence] {
            return g_ownedLogEfzProcessedSequence >= closeSequence
                || InterlockedCompareExchange(
                       &g_ownedLogEfzEmergencyStop, 0, 0) != 0;
        });
        g_consoleWorker.DrainFinished();
        joinWorker = g_ownedLogEfzWorker.joinable();
    }
    g_ownedLogEfzQueueCv.notify_all();

    if (joinWorker)
    {
        g_ownedLogEfzWorker.join();
    }
    {
        std::lock_guard<std::mutex> lock(g_ownedLogEfzQueueMutex);
        g_consoleWorker.Joined();
        g_consoleIngressAccepting.store(false, std::memory_order_release);
        g_consoleIngressPollingEnabled.store(false, std::memory_order_release);
        g_ownedLogEfzQueue.clear();
        g_ownedLogEfzWorkerThreadId = 0;
    }
    g_ownedLogEfzDrainCv.notify_all();
}

static void ResetLogEfzDiskHandleCache();

void CloseMirrorLogFiles()
{
    unsigned long long closeSequence = 0;
    {
        std::lock_guard<std::mutex> parserLock(g_consoleLogMutex);
        std::lock_guard<std::mutex> queueLock(g_ownedLogEfzQueueMutex);
        if (g_consoleWorker.StartedOrDraining()
            && InterlockedCompareExchange(
                   &g_ownedLogEfzEmergencyStop, 0, 0) == 0)
        {
            const bool resumeIngress = PauseConsoleIngress();
            const size_t ingressBoundary = CaptureConsoleIngressBoundary();
            const unsigned long ingressLossGeneration =
                g_consoleIngressLossGeneration.load(std::memory_order_acquire);

            OwnedLogEfzEntry flushEntry;
            flushEntry.kind = OwnedLogEfzEntry::Kind::ParseFlush;
            flushEntry.ingressBoundary = ingressBoundary;
            flushEntry.ingressLossGeneration = ingressLossGeneration;
            flushEntry.sequence = ++g_ownedLogEfzEnqueuedSequence;
            g_ownedLogEfzQueue.push_back(std::move(flushEntry));

            OwnedLogEfzEntry closeEntry;
            closeEntry.kind = OwnedLogEfzEntry::Kind::CloseBoundary;
            closeEntry.ingressBoundary = ingressBoundary;
            closeEntry.ingressLossGeneration = ingressLossGeneration;
            closeEntry.sequence = ++g_ownedLogEfzEnqueuedSequence;
            closeSequence = closeEntry.sequence;
            g_ownedLogEfzQueue.push_back(std::move(closeEntry));

            if (resumeIngress)
            {
                g_consoleIngressAccepting.store(true, std::memory_order_release);
            }
        }
        else
        {
            g_consoleIngressAccepting.store(false, std::memory_order_release);
            FlushPendingConsoleOutputLocked();
        }
    }
    if (closeSequence != 0)
    {
        g_ownedLogEfzQueueCv.notify_one();
    }
    ResetLogEfzDiskHandleCache();
}

void PrimeManagedLogEfzHistory()
{
    (void)StartConsoleCaptureWorker(false);
}

// Never cache an "ignore" classification: a recycled numeric handle can bypass our CreateFile stub.
enum : LONG
{
    kConsoleFileUnknown = 0,
    kConsoleFileIgnore = 1,
    kConsoleFileTextLog = 2,
};

constexpr size_t kConsoleFileRegistryCapacity = 256;

struct ConsoleFileRegistryEntry
{
    std::atomic<uintptr_t> handle{0};
    std::atomic<LONG> kind{kConsoleFileUnknown};
};

static std::array<ConsoleFileRegistryEntry, kConsoleFileRegistryCapacity>
    g_consoleFileRegistry;

static size_t ConsoleFileRegistryIndex(HANDLE hFile)
{
    const uintptr_t value = reinterpret_cast<uintptr_t>(hFile);
    return ((value >> 2u) ^ (value >> 10u))
        & (kConsoleFileRegistryCapacity - 1u);
}

void RegisterConsoleCaptureFileHandle(HANDLE hFile, bool captureAsTextLog)
{
    if (!captureAsTextLog
        || hFile == nullptr
        || hFile == INVALID_HANDLE_VALUE)
    {
        return;
    }

    ConsoleFileRegistryEntry& entry =
        g_consoleFileRegistry[ConsoleFileRegistryIndex(hFile)];
    entry.handle.store(0, std::memory_order_release);
    entry.kind.store(kConsoleFileTextLog, std::memory_order_relaxed);
    entry.handle.store(
        reinterpret_cast<uintptr_t>(hFile),
        std::memory_order_release);
}

static LONG ClassifyRegisteredConsoleFileHandle(HANDLE hFile)
{
    if (hFile == nullptr || hFile == INVALID_HANDLE_VALUE)
    {
        return kConsoleFileIgnore;
    }

    const ConsoleFileRegistryEntry& entry =
        g_consoleFileRegistry[ConsoleFileRegistryIndex(hFile)];
    const uintptr_t expectedHandle = reinterpret_cast<uintptr_t>(hFile);
    const uintptr_t handleBefore =
        entry.handle.load(std::memory_order_acquire);
    if (handleBefore != expectedHandle)
    {
        return kConsoleFileUnknown;
    }
    const LONG kind = entry.kind.load(std::memory_order_relaxed);
    const uintptr_t handleAfter =
        entry.handle.load(std::memory_order_acquire);
    return handleAfter == handleBefore ? kind : kConsoleFileUnknown;
}

static void ResetLogEfzDiskHandleCache()
{
    for (auto& entry : g_consoleFileRegistry)
    {
        entry.handle.store(0, std::memory_order_release);
        entry.kind.store(kConsoleFileUnknown, std::memory_order_relaxed);
    }
}

void MaybeLogConsoleOutputChunk(HANDLE hFile, LPCVOID lpBuffer, DWORD nBytes)
{
    if (lpBuffer == nullptr || nBytes == 0)
    {
        return;
    }

    const LONG fileKind = ClassifyRegisteredConsoleFileHandle(hFile);
    if (fileKind == kConsoleFileIgnore)
    {
        return;
    }

    LogConsoleTextChunk(
        fileKind == kConsoleFileTextLog ? "WriteFileDisk" : "WriteFile",
        reinterpret_cast<const char*>(lpBuffer),
        static_cast<size_t>(nBytes));
}

void MaybeLogConsoleWriteAChunk(const VOID* lpBuffer, DWORD nChars)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }

    LogConsoleTextChunk("WriteConsoleA", reinterpret_cast<const char*>(lpBuffer), static_cast<size_t>(nChars));
}

void MaybeLogConsoleWriteWChunk(const VOID* lpBuffer, DWORD nChars)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }

    const wchar_t* wideText = reinterpret_cast<const wchar_t*>(lpBuffer);
    constexpr size_t kMaxIngressWideChars =
        kConsoleIngressPayloadBytes / sizeof(wchar_t);
    const int wideLen = static_cast<int>((std::min)(
        static_cast<size_t>(nChars),
        kMaxIngressWideChars + 1));
    if (wideLen <= 0)
    {
        return;
    }

    LogConsoleWideTextChunk("WriteConsoleW", wideText, wideLen);
}

void MaybeLogConsoleOutputCharacterAChunk(const VOID* lpBuffer, DWORD nChars, COORD writeCoord)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }

    // Synthetic newline on cursor Y change: Revival moves the cursor, never writes '\n'.
    static std::atomic<SHORT> s_lastY{-1};
    const SHORT lastY = s_lastY.exchange(writeCoord.Y, std::memory_order_relaxed);
    if (lastY >= 0 && writeCoord.Y != lastY)
    {
        LogConsoleTextChunk("WriteConsoleOutputCharacterA", "\n", 1);
    }

    LogConsoleTextChunk("WriteConsoleOutputCharacterA", reinterpret_cast<const char*>(lpBuffer), static_cast<size_t>(nChars));
}

void MaybeLogConsoleOutputCharacterWChunk(const VOID* lpBuffer, DWORD nChars, COORD writeCoord)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }

    static std::atomic<SHORT> s_lastY{-1};
    const SHORT lastY = s_lastY.exchange(writeCoord.Y, std::memory_order_relaxed);
    if (lastY >= 0 && writeCoord.Y != lastY)
    {
        LogConsoleTextChunk("WriteConsoleOutputCharacterW", "\n", 1);
    }

    const wchar_t* wideText = reinterpret_cast<const wchar_t*>(lpBuffer);
    constexpr size_t kMaxIngressWideChars =
        kConsoleIngressPayloadBytes / sizeof(wchar_t);
    const int wideLen = static_cast<int>((std::min)(
        static_cast<size_t>(nChars),
        kMaxIngressWideChars + 1));
    if (wideLen <= 0)
    {
        return;
    }

    LogConsoleWideTextChunk("WriteConsoleOutputCharacterW", wideText, wideLen);
}

void MaybeLogOutputDebugStringA(LPCSTR lpOutputString)
{
    if (lpOutputString == nullptr || lpOutputString[0] == '\0')
    {
        return;
    }

    size_t len = 0;
    while (len <= kConsoleIngressPayloadBytes
        && lpOutputString[len] != '\0')
    {
        ++len;
    }
    if (len == 0)
    {
        return;
    }

    LogConsoleTextChunk("OutputDebugStringA", lpOutputString, len);
}

void MaybeLogOutputDebugStringW(LPCWSTR lpOutputString)
{
    if (lpOutputString == nullptr || lpOutputString[0] == L'\0')
    {
        return;
    }

    size_t wideChars = 0;
    constexpr size_t kMaxIngressWideChars =
        kConsoleIngressPayloadBytes / sizeof(wchar_t);
    while (wideChars <= kMaxIngressWideChars
        && lpOutputString[wideChars] != L'\0')
    {
        ++wideChars;
    }
    const int wideLen = static_cast<int>(wideChars);
    if (wideLen <= 0)
    {
        return;
    }

    LogConsoleWideTextChunk("OutputDebugStringW", lpOutputString, wideLen);
}

std::string ErrorString(DWORD code)
{
    char buffer[256] = {};
    const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageA(flags, nullptr, code, 0, buffer, static_cast<DWORD>(sizeof(buffer)), nullptr);
    if (length == 0)
    {
        std::snprintf(buffer, sizeof(buffer), "win32_error_%lu", static_cast<unsigned long>(code));
    }
    std::string text = buffer;
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '\t'))
    {
        text.pop_back();
    }
    return text;
}

std::string BaseLower(const std::string& path)
{
    const size_t slash = path.find_last_of("\\/");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return base;
}

}
