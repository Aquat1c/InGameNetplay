// Console I/O text capture and delay prompt parsing for the Revival takeover.

#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/mod_settings.h"

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
    metrics.minDelay = 0;
    metrics.maxDelay = 20;
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

    int minDelay = 0;
    int maxDelay = 20;
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
        g_injectedBlock->delayAveragePingMs = metrics.averagePingMs;
        g_injectedBlock->delayMinPingMs = metrics.minPingMs;
        g_injectedBlock->delayMaxPingMs = metrics.maxPingMs;
        g_injectedBlock->delayRecommended = metrics.recommendedDelay;
        g_injectedBlock->delayRangeMin = metrics.minDelay;
        g_injectedBlock->delayRangeMax = metrics.maxDelay;
        InterlockedExchange(&g_injectedBlock->delayMetricsSerial, serial);
    }
}

// Accumulates recent console lines so that metrics printed on preceding lines
// ("Average Ping:", "Min Ping:", etc.) are available when the delay prompt
// line is detected.  Cleared on consumption and on session reset.
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

// Revival's low-level socket wrappers print "Socket error: <op>, <code>" for
// EVERY failed WinSock call. For the in-match UDP data-path operations
// (recv/recvfrom/sendto/select) the wrapper only logs the failure and then
// CONTINUES: stock Revival tolerates ~8 s of continuous peer silence and
// resumes the instant packets return, and if the outage is permanent its own
// peer-silence timeout ("Remote timed out" / "Source quit or timed out") ends
// the session - which NoteConsolePromptLine already treats as fatal below. So a
// transient data-path socket error - the classic faulty-cable case
// (WSAECONNRESET from a bounced datagram, WSAENETUNREACH from a briefly
// unplugged cable) - must NOT be promoted to a latching fatal, or we tear down
// a session that stock Revival would have transparently recovered.
//
// Setup / control-channel ops (socket/bind/connect/setsockopt, and the TCP
// relay-channel send) are intentionally absent here: those failures mean the
// session genuinely could not start, so they retain the fatal treatment.
//
// Decomp-verified log-and-continue sites (1.02i EfzRevival.exe): recv
// :11295-11304, select :11311-11316, sendto :10582-10595.
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

    // Accumulate lines for delay prompt metrics parsing.  EfzRevival.exe prints
    // "Average Ping:", "Min Ping:" etc. on separate lines BEFORE the prompt.
    if (!g_delayMetricsAccumulator.empty())
    {
        g_delayMetricsAccumulator.push_back('\n');
    }
    g_delayMetricsAccumulator.append(text);
    // Cap to prevent unbounded growth between sessions.
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
            PublishConsoleError("Source quit or timed out");
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

    // --- Connection error detection ---
    // Revival prints these messages to console on failure. Publish them
    // through IPC so the host process can transition to Failed phase.
    // Map known Revival console error strings to friendly messages. Mirrors the
    // set Concerto recognises (efz.py `error_strings`) plus Revival-specific
    // ones. The published TEXT is display-only (the consoleErrorSerial drives
    // recovery logic), so friendlier wording here is safe. More specific needles
    // must precede more general ones (e.g. the "Already playing..." variant
    // before the bare "Spectators have been disabled").
    struct ConsoleErrorMapping
    {
        const char* needle;
        const char* friendly;
    };
    static const ConsoleErrorMapping kConsoleErrorMap[] = {
        {"header crc mismatch", "Desync detected by Revival (header CRC mismatch)."},
        {"state not recoverable", "Desync detected by Revival (state not recoverable)."},
        {"Connection timed out", "Connection timed out."},
        {"Source quit or timed out", "Opponent quit or timed out."},
        {"Host timed out", "Host timed out - no one connected."},
        {"Remote timed out", "Opponent timed out."},
        {"Invalid address", "Invalid address - check the IP and port."},
        {"Only one net instance allowed", "A netplay session is already running."},
        {"Remote is using a deprecated version", "Opponent is on an outdated Revival version."},
        {"New version available", "A new Revival version is available - please update."},
        {"Invalid delay", "Invalid delay value."},
        {"Already playing, spectators have been disabled", "Host is already playing; spectators are disabled."},
        {"Spectators have been disabled", "Spectators have been disabled by the host."},
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
        // A transient in-match UDP data-path socket error (recv/sendto/select)
        // is logged-and-ignored by Revival itself; promoting it to a latching
        // fatal here would kill a session Revival recovers within its ~8 s
        // peer-silence window. Defer to Revival's own timeout instead - its
        // terminal "Remote timed out" / "Source quit or timed out" line is
        // mapped to a fatal above, so a genuinely dead link still tears down.
        if (SocketErrorNamesTransientDataPathOp(text))
        {
            // A flapping link can emit these every poll; rate-limit the
            // breadcrumb so a sustained outage does not flood the log.
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
        // Use the full text since it includes the socket error details
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

    const LONG serial = InterlockedIncrement(&g_injectedDelayPromptSerial);
    PublishDelayPromptSerial(
        serial,
        controlWakeRequestSerialSnapshot);
    // Parse the accumulated buffer (includes preceding "Average Ping:" etc.
    // lines) rather than just the prompt line itself.
    bool hasMetrics = false;
    const DelayPromptMetrics metrics = ParseDelayPromptMetricsFromText(
        g_delayMetricsAccumulator, &hasMetrics);
    g_delayMetricsAccumulator.clear();
    PublishDelayPromptMetrics(metrics, serial);
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

static void AppendOwnedLogEfzLine(const char* sourceTag, const std::string& line);
static bool EnqueueConsoleParseChunk(
    const char* sourceTag,
    const void* data,
    size_t bytes,
    bool wide,
    LONG controlWakeRequestSerialSnapshot);
static bool EnqueueConsoleParseFlush();
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

// Public seam for every console/log write interception in the injected
// EfzRevival helper. The host EFZ process is deliberately not IAT-patched.
// Helper writes can still occur on timing-sensitive rollback/network paths, so
// the producer stays bounded and non-blocking: it only reserves a preallocated
// ingress slot, copies raw bytes, and publishes with atomics. The managed-log
// worker owns text validation, conversion, line assembly, prompt detection,
// and mirror logging. A full/unavailable ingress drops the whole write; the
// worker reports the loss and discards pending fragments so text on opposite
// sides of a drop can never be joined into a synthetic line.
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
        /*wide=*/false,
        controlWakeRequestSerialSnapshot);
}

// Wide-text variant: the UTF-16 -> UTF-8 conversion also moves to the worker
// (it allocates and scans; WriteConsoleOutputCharacterW repaints are frequent).
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
        /*wide=*/true,
        controlWakeRequestSerialSnapshot);
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
    // NOTE: CaptureRevivalNativeLogsEnabled() gate intentionally removed.
    // This function only does workflow signal detection (NoteConsolePromptLine)
    // and no longer logs to the mod log.  Gating it prevents detection of
    // connection errors, delay prompts, and spectate confirmations.
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

    auto flushLine = [&](bool /*partial*/) {
        const std::string trimmed = TrimAscii(*line);
        line->clear();
        if (trimmed.empty())
        {
            return;
        }
        // logEfz.txt is a host-process disk log. Do not let helper console
        // prompt/output capture create or overwrite that file.
        if (!IsCurrentProcessRevival()
            && sourceTag != nullptr
            && std::strcmp(sourceTag, "WriteFileDisk") == 0)
        {
            AppendOwnedLogEfzLine(sourceTag, trimmed);
        }
        // Parse workflow signals (delay prompt, spectate confirm, quit packet,
        // peer died diagnostics)
        // but don't echo Revival's debug text into the mod log - Revival
        // already writes to its own log files.
        NoteConsolePromptLine(
            trimmed,
            controlWakeRequestSerialSnapshot);
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

    // Revival's WriteConsoleOutputCharacter path injects a synthetic newline
    // when its cursor moves to another row. Listener observations are only
    // published from that completed-line path. Publishing a parseable partial
    // port here is unsafe: expected port 10 is also a valid prefix of 10800.
    // The bounded listener-ack timeout diagnoses a genuinely missing boundary.
    if (line != nullptr && !line->empty() && line->size() >= 12)
    {
        static const char* kImmediateFlushKeywords[] = {
            "header crc mismatch",
            "state not recoverable",
            "Recv quit from:",
            "Received quit from:",
            "Connection timed out",
            "Source quit or timed out",
            "Host timed out",
            "Remote timed out",
            "Spectators have been disabled",
            "Socket error",
            "Host already playing, join as a spectator",
            "Host not yet playing, join as a player",
            "Waiting for game to begin",
            "Enter the initial input delay",
            "Enter the input delay to use",
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

    // For unknown/untracked sources, don't hold partial fragments indefinitely.
    if (SelectPendingConsoleLine(sourceTag) == nullptr && !line->empty())
    {
        flushLine(true);
    }
}

static void FlushPendingConsoleOutputLocked()
{
    auto flushOne = [&](const char* sourceTag, std::string* line) {
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

        if (!IsCurrentProcessRevival()
            && sourceTag != nullptr
            && std::strcmp(sourceTag, "WriteFileDisk") == 0)
        {
            AppendOwnedLogEfzLine(sourceTag, trimmed);
        }

        // Parse workflow signals but don't echo Revival debug text.
        NoteConsolePromptLine(trimmed, -1);
    };

    flushOne("WriteFile", &g_consolePendingWriteFile);
    flushOne("WriteFileDisk", &g_consolePendingWriteFileDisk);
    flushOne("WriteConsoleA", &g_consolePendingWriteConsoleA);
    flushOne("WriteConsoleW", &g_consolePendingWriteConsoleW);
    flushOne("WriteConsoleOutputCharacterA", &g_consolePendingWriteConsoleOutputCharacterA);
    flushOne("WriteConsoleOutputCharacterW", &g_consolePendingWriteConsoleOutputCharacterW);
    flushOne("OutputDebugStringA", &g_consolePendingOutputDebugStringA);
    flushOne("OutputDebugStringW", &g_consolePendingOutputDebugStringW);
}

void FlushPendingConsoleOutput(const char* /*reason*/)
{
    // NOTE: CaptureRevivalNativeLogsEnabled() gate intentionally removed.
    // Flushing must always happen so NoteConsolePromptLine can detect
    // workflow signals (errors, delay prompts, spectate confirms).
    // The flush carries a snapshot of the ingress write position, so the
    // worker drains all chunks accepted before this call before flushing.
    if (EnqueueConsoleParseFlush())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_consoleLogMutex);
    FlushPendingConsoleOutputLocked();
}

// ---------------------------------------------------------------------------
// Managed logEfz capture - host-side ownership of the actual EFZ disk log.
//
// Rather than trying to repair a corrupted native logEfz.txt after the fact,
// we capture the host process's intercepted disk writes and append them to a
// single clean root logEfz.txt. Each init session writes a clear separator so
// the file remains readable across reconnects/restarts.
//
// The file is append-only (FILE_APPEND_DATA): existing content is never
// rewritten, so no in-memory history is kept and session boundaries cost one
// header write instead of a full-file rebuild.  The only truncation is the
// fresh-launch reset on the first open when the user keeps
// PreserveRevivalLogsAcrossLaunches off.
// ---------------------------------------------------------------------------
static std::mutex g_ownedLogEfzMutex;
static HANDLE g_ownedLogEfzCurrentHandle = INVALID_HANDLE_VALUE;
static std::string g_ownedLogEfzCurrentPath;
static uint32_t g_ownedLogEfzSessionOrdinal = 0;
static bool g_ownedLogEfzOpenedThisProcess = false;

// The native WriteFile call has already completed when capture reaches us.
// Mirroring that line to the mod-owned log on the same thread used to add a
// second synchronous disk write to Revival's simulation path, and the parse
// itself (line assembly + keyword scans) was still synchronous per write.
// Both now ride this worker: raw captured bytes enter through the preallocated
// ring and are parsed on the worker (which mirror-writes directly, keeping
// order); Line entries remain only for lifecycle-side flushes when the worker
// is unavailable. Session close is a ring-watermark barrier, so lines cannot
// cross SESSION headers.
struct OwnedLogEfzEntry
{
    enum class Kind
    {
        Line,
        CloseBoundary,
        ParseFlush,
    } kind = Kind::Line;
    std::string sourceTag;
    std::string line;
    SYSTEMTIME timestamp = {};
    unsigned long long sequence = 0;
    // The worker must consume every raw ingress reservation below this
    // position before executing the queue record. This keeps flush/close
    // controls ordered with the otherwise lock-free producer stream.
    size_t ingressBoundary = 0;
    unsigned long ingressLossGeneration = 0;
};

constexpr size_t kOwnedLogEfzQueueLimit = 8192;

// Raw intercepted writes use a separate, fully preallocated ingress. Keeping
// the previous 16 KiB per-write bound avoids fragmenting a single native write
// across records; 256 slots cost 4 MiB in the 32-bit process and absorb bursts
// while the worker performs a managed-log disk append.
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
    // Snapshot at the intercepted native write. Deferred parsing may execute
    // after a later WCI transition; preserving this value keeps causal order.
    LONG controlWakeRequestSerialSnapshot = -1;
    unsigned long lossGeneration = 0;
};

static std::array<ConsoleIngressSlot, kConsoleIngressCapacity> g_consoleIngress;
static std::atomic<size_t> g_consoleIngressEnqueuePosition{0};
// Single-consumer state. Only the managed-log worker mutates this value.
static size_t g_consoleIngressDequeuePosition = 0;
static std::atomic<bool> g_consoleIngressAccepting{false};
static std::atomic<bool> g_consoleIngressPollingEnabled{false};
static std::atomic<unsigned long> g_consoleIngressActiveProducers{0};
static std::atomic<unsigned long> g_consoleIngressFullDrops{0};
static std::atomic<unsigned long> g_consoleIngressContentionDrops{0};
static std::atomic<unsigned long> g_consoleIngressOversizeDrops{0};
static std::atomic<unsigned long> g_consoleIngressUnavailableDrops{0};
static std::atomic<unsigned long> g_consoleIngressLossGeneration{0};
// Single-consumer state, updated only by the managed-log worker.
static unsigned long g_consoleIngressConsumedLossGeneration = 0;

static std::mutex g_ownedLogEfzQueueMutex;
static std::condition_variable g_ownedLogEfzQueueCv;
static std::condition_variable g_ownedLogEfzDrainCv;
static std::deque<OwnedLogEfzEntry> g_ownedLogEfzQueue;
static std::thread g_ownedLogEfzWorker;
static bool g_ownedLogEfzWorkerStarted = false;
static bool g_ownedLogEfzWorkerStopping = false;
static bool g_ownedLogEfzAccepting = false;
static volatile LONG g_ownedLogEfzEmergencyStop = 0;
static unsigned long long g_ownedLogEfzEnqueuedSequence = 0;
static unsigned long long g_ownedLogEfzProcessedSequence = 0;
static unsigned long g_ownedLogEfzDroppedLines = 0;
// Worker thread id: lets AppendOwnedLogEfzLine mirror-write DIRECTLY when the
// parse itself is running on the worker (deferred path), preserving strict
// FIFO order with session close boundaries instead of re-enqueueing behind
// them.
static volatile DWORD g_ownedLogEfzWorkerThreadId = 0;

bool StartConsoleCaptureWorker(bool enableRawIngress);
static void CloseOwnedLogEfzForBoundary();

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
        // Never splice text preceding a lost write to text following it. The
        // generation is carried by the first later accepted record (or by a
        // lifecycle watermark), so older queued records still drain in order.
        DiscardPendingConsoleFragmentsLocked();
    }
    g_consoleIngressConsumedLossGeneration = lossGeneration;
}

static unsigned long long QueryExistingOwnedLogEfzSize(const std::string& path, bool* outExists)
{
    if (outExists != nullptr)
    {
        *outExists = false;
    }

    if (path.empty())
    {
        return 0;
    }

    WIN32_FILE_ATTRIBUTE_DATA attrs = {};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs))
    {
        return 0;
    }
    if ((attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        return 0;
    }

    if (outExists != nullptr)
    {
        *outExists = true;
    }

    return (static_cast<unsigned long long>(attrs.nFileSizeHigh) << 32)
        | static_cast<unsigned long long>(attrs.nFileSizeLow);
}

static void LogOwnedLogEfzIntegrityIssue(
    const char* outcome,
    const std::string& path,
    const char* detail)
{
    if (outcome == nullptr || outcome[0] == '\0')
    {
        outcome = "unknown";
    }

    if (detail != nullptr && detail[0] != '\0')
    {
        mod::Log(
            "CAPTURE_LOG: managed logEfz integrity issue outcome=%s current='%s' %s",
            outcome,
            path.c_str(),
            detail);
    }
    else
    {
        mod::Log(
            "CAPTURE_LOG: managed logEfz integrity issue outcome=%s current='%s'",
            outcome,
            path.c_str());
    }
}

static std::string GetConsoleCaptureModuleDirectory()
{
    char modulePath[MAX_PATH] = {};
    HMODULE selfModule = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&GetConsoleCaptureModuleDirectory),
        &selfModule);
    if (selfModule == nullptr || GetModuleFileNameA(selfModule, modulePath, MAX_PATH) == 0)
    {
        return {};
    }

    std::string dir(modulePath);
    const size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos)
    {
        return {};
    }
    dir.resize(slash);
    return dir;
}

static std::string ParentDirectory(const std::string& path)
{
    if (path.empty())
    {
        return {};
    }

    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos)
    {
        return path.substr(0, slash);
    }
    return {};
}

static std::string GetOwnedLogEfzPath()
{
    const std::string modDir = GetConsoleCaptureModuleDirectory();
    if (modDir.empty())
    {
        return {};
    }

    const std::string modsDir = ParentDirectory(modDir);
    const std::string gameRoot = ParentDirectory(modsDir.empty() ? modDir : modsDir);
    const std::string rootDir = gameRoot.empty() ? modDir : gameRoot;
    return rootDir + "\\logEfz.txt";
}

static std::string FormatOwnedLogEfzTimestamp()
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);

    char buffer[64] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        static_cast<unsigned>(st.wYear),
        static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay),
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond),
        static_cast<unsigned>(st.wMilliseconds));

    return buffer;
}

static bool WriteOwnedLogEfzBytes(
    const std::string& path,
    const char* reason,
    const char* data,
    size_t size)
{
    if (g_ownedLogEfzCurrentHandle == INVALID_HANDLE_VALUE || data == nullptr || size == 0)
    {
        return true;
    }

    const DWORD requested = static_cast<DWORD>(size);
    DWORD written = 0;
    SetLastError(NO_ERROR);
    const BOOL writeOk = WriteFile(
        g_ownedLogEfzCurrentHandle,
        data,
        requested,
        &written,
        nullptr);
    const DWORD writeError = GetLastError();
    if (!writeOk || written != requested)
    {
        char detail[256] = {};
        std::snprintf(
            detail,
            sizeof(detail),
            "reason=%s short_write requested=%lu written=%lu winerr=%lu ok=%d",
            reason != nullptr ? reason : "unknown",
            static_cast<unsigned long>(size),
            static_cast<unsigned long>(written),
            static_cast<unsigned long>(writeError),
            writeOk ? 1 : 0);
        LogOwnedLogEfzIntegrityIssue("short_write", path, detail);
        return false;
    }

    return true;
}

static void CloseOwnedLogEfzCurrentFileLocked(const char* reason)
{
    if (g_ownedLogEfzCurrentHandle == INVALID_HANDLE_VALUE)
    {
        return;
    }

    const HANDLE handle = g_ownedLogEfzCurrentHandle;
    g_ownedLogEfzCurrentHandle = INVALID_HANDLE_VALUE;

    SetLastError(NO_ERROR);
    const BOOL flushOk = FlushFileBuffers(handle);
    const DWORD flushError = GetLastError();
    SetLastError(NO_ERROR);
    const BOOL closeOk = CloseHandle(handle);
    const DWORD closeError = GetLastError();

    if (!flushOk)
    {
        char detail[256] = {};
        std::snprintf(
            detail,
            sizeof(detail),
            "reason=%s close_flush_failed winerr=%lu",
            reason != nullptr ? reason : "unknown",
            static_cast<unsigned long>(flushError));
        LogOwnedLogEfzIntegrityIssue("close_flush_failed", g_ownedLogEfzCurrentPath, detail);
    }

    if (!closeOk)
    {
        char detail[256] = {};
        std::snprintf(
            detail,
            sizeof(detail),
            "reason=%s close_failed winerr=%lu",
            reason != nullptr ? reason : "unknown",
            static_cast<unsigned long>(closeError));
        LogOwnedLogEfzIntegrityIssue("close_failed", g_ownedLogEfzCurrentPath, detail);
    }
}

static std::string BuildOwnedLogEfzHeader(uint32_t sessionOrdinal)
{
    const std::string timestamp = FormatOwnedLogEfzTimestamp();

    char buffer[256] = {};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "============================================================\r\n"
        "SESSION %lu  start=%s  pid=%lu\r\n"
        "============================================================\r\n"
        "\r\n",
        static_cast<unsigned long>(sessionOrdinal),
        timestamp.c_str(),
        static_cast<unsigned long>(GetCurrentProcessId()));

    return buffer;
}

// (The old in-memory history priming/sanitizing lived here.  Append-only
// writing made it unnecessary: prior file content is never re-read or
// rewritten, so there is nothing to prime or sanitize.)

static bool EnsureOwnedLogEfzFilesOpenLocked()
{
    if (g_ownedLogEfzCurrentHandle != INVALID_HANDLE_VALUE)
    {
        return true;
    }

    g_ownedLogEfzCurrentPath = GetOwnedLogEfzPath();
    if (g_ownedLogEfzCurrentPath.empty())
    {
        return false;
    }

    const bool preserveAcrossLaunches =
        netplay::mod_settings::PreserveRevivalLogsAcrossLaunches();
    const bool firstOpenThisProcess = !g_ownedLogEfzOpenedThisProcess;
    bool previousExists = false;
    const unsigned long long previousBytes =
        QueryExistingOwnedLogEfzSize(g_ownedLogEfzCurrentPath, &previousExists);

    // Append-only: FILE_APPEND_DATA makes every WriteFile land at end of
    // file, so session reopens never rewrite existing content.  The only
    // truncation is the fresh-launch reset on the first open of a launch.
    const bool truncateNow = firstOpenThisProcess && !preserveAcrossLaunches;
    g_ownedLogEfzCurrentHandle = CreateFileA(
        g_ownedLogEfzCurrentPath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        truncateNow ? CREATE_ALWAYS : OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (g_ownedLogEfzCurrentHandle == INVALID_HANDLE_VALUE)
    {
        mod::Log(
            "CAPTURE_LOG: failed to open managed logEfz current='%s'",
            g_ownedLogEfzCurrentPath.c_str());
        char detail[256] = {};
        std::snprintf(
            detail,
            sizeof(detail),
            "reason=append_open_failed winerr=%lu",
            static_cast<unsigned long>(GetLastError()));
        LogOwnedLogEfzIntegrityIssue("open_failed", g_ownedLogEfzCurrentPath, detail);
        g_ownedLogEfzCurrentPath.clear();
        return false;
    }

    if (firstOpenThisProcess)
    {
        g_ownedLogEfzOpenedThisProcess = true;
        mod::Log(
            "CAPTURE_LOG: %s managed logEfz current='%s' preserveAcrossLaunches=%d previousExists=%d previousBytes=%llu",
            truncateNow ? "starting fresh" : "appending to preserved",
            g_ownedLogEfzCurrentPath.c_str(),
            preserveAcrossLaunches ? 1 : 0,
            previousExists ? 1 : 0,
            previousBytes);
    }

    const uint32_t sessionOrdinal = g_ownedLogEfzSessionOrdinal + 1;
    const std::string header = BuildOwnedLogEfzHeader(sessionOrdinal);
    if (!WriteOwnedLogEfzBytes(
            g_ownedLogEfzCurrentPath,
            "session_header",
            header.data(),
            header.size()))
    {
        CloseOwnedLogEfzCurrentFileLocked("session_header_failed");
        return false;
    }

    g_ownedLogEfzSessionOrdinal = sessionOrdinal;

    mod::Log(
        "CAPTURE_LOG: opened managed logEfz current='%s' session=%lu mode=append fileBytes=%llu preserveAcrossLaunches=%d",
        g_ownedLogEfzCurrentPath.c_str(),
        static_cast<unsigned long>(g_ownedLogEfzSessionOrdinal),
        previousBytes,
        preserveAcrossLaunches ? 1 : 0);
    return true;
}

static void WriteOwnedLogEfzLine(
    const char* sourceTag,
    const std::string& line,
    const SYSTEMTIME& timestamp)
{
    if (line.empty())
    {
        return;
    }

    // --- Timing guard: detect slow logEfz appends -------------------------
    LARGE_INTEGER appendQpcStart = {};
    QueryPerformanceCounter(&appendQpcStart);

    std::lock_guard<std::mutex> lock(g_ownedLogEfzMutex);
    if (!EnsureOwnedLogEfzFilesOpenLocked())
    {
        return;
    }

    char prefix[96] = {};
    std::snprintf(
        prefix,
        sizeof(prefix),
        "[%02u:%02u:%02u.%03u][%s] ",
        static_cast<unsigned>(timestamp.wHour),
        static_cast<unsigned>(timestamp.wMinute),
        static_cast<unsigned>(timestamp.wSecond),
        static_cast<unsigned>(timestamp.wMilliseconds),
        sourceTag != nullptr ? sourceTag : "unknown");

    std::string entry(prefix);
    entry.append(line);
    entry.append("\r\n");

    if (!WriteOwnedLogEfzBytes(
            g_ownedLogEfzCurrentPath,
            "append_entry",
            entry.data(),
            entry.size()))
    {
        CloseOwnedLogEfzCurrentFileLocked("append_entry_failed");
    }

    // --- End timing guard ---------------------------------------------------
    {
        static uint32_t s_appendSlowCount = 0;
        LARGE_INTEGER appendQpcEnd = {}, freq = {};
        QueryPerformanceCounter(&appendQpcEnd);
        QueryPerformanceFrequency(&freq);
        const double elapsedMs =
            static_cast<double>(appendQpcEnd.QuadPart - appendQpcStart.QuadPart)
            * 1000.0 / static_cast<double>(freq.QuadPart);
        if (elapsedMs > 5.0)
        {
            ++s_appendSlowCount;
            if (s_appendSlowCount <= 5 || (s_appendSlowCount % 500 == 0))
            {
                mod::Log(
                    "PERF_WARN: AppendOwnedLogEfzLine took %.1fms "
                    "(slowCount=%u) - logEfz disk write stalling",
                    elapsedMs, s_appendSlowCount);
            }
        }
    }
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
        unsigned long droppedLines = 0;
        {
            std::unique_lock<std::mutex> lock(g_ownedLogEfzQueueMutex);
            // Raw producers deliberately do not signal a kernel primitive.
            // Polling at a short cadence keeps their hot path to copies and
            // atomics while control records can still wake us immediately.
            const auto wakePredicate = [] {
                    return g_ownedLogEfzWorkerStopping
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
                // The EFZ host has no raw IAT producers. Sleep until an actual
                // mirror/control record arrives instead of waking at 1 kHz in
                // the simulation process.
                g_ownedLogEfzQueueCv.wait(lock, wakePredicate);
            }
            if (InterlockedCompareExchange(
                    &g_ownedLogEfzEmergencyStop, 0, 0) != 0)
            {
                break;
            }

            // A queued lifecycle record owns a ring watermark. Drain only the
            // reservations preceding it; later writes stay behind the control
            // record and therefore belong to the next lifecycle interval.
            if (!g_ownedLogEfzQueue.empty()
                && g_consoleIngressDequeuePosition
                    >= g_ownedLogEfzQueue.front().ingressBoundary)
            {
                entry = std::move(g_ownedLogEfzQueue.front());
                g_ownedLogEfzQueue.pop_front();
                droppedLines = g_ownedLogEfzDroppedLines;
                g_ownedLogEfzDroppedLines = 0;
                haveQueueEntry = true;
            }
            else
            {
                ingressSlot =
                    TryAcquireConsoleIngressRecord(&ingressPosition);
                if (ingressSlot == nullptr)
                {
                    if (g_ownedLogEfzWorkerStopping
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

        if (droppedLines != 0)
        {
            mod::Log(
                "CAPTURE_LOG: managed logEfz mirror queue overflow "
                "dropped=%lu",
                droppedLines);
        }
        if (entry.kind == OwnedLogEfzEntry::Kind::CloseBoundary)
        {
            CloseOwnedLogEfzForBoundary();
        }
        else if (entry.kind == OwnedLogEfzEntry::Kind::ParseFlush)
        {
            std::lock_guard<std::mutex> parserLock(g_consoleLogMutex);
            FlushPendingConsoleOutputLocked();
        }
        else
        {
            WriteOwnedLogEfzLine(
                entry.sourceTag.c_str(),
                entry.line,
                entry.timestamp);
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
    if (g_ownedLogEfzWorkerStarted)
    {
        return !enableRawIngress
            || g_consoleIngressAccepting.load(std::memory_order_acquire);
    }

    g_consoleIngressPollingEnabled.store(
        enableRawIngress, std::memory_order_release);
    ResetConsoleIngress();
    g_ownedLogEfzWorkerStopping = false;
    g_ownedLogEfzAccepting = false;
    InterlockedExchange(&g_ownedLogEfzEmergencyStop, 0);
    try
    {
        g_ownedLogEfzWorker = std::thread(ManagedLogEfzWorkerMain);
        g_ownedLogEfzWorkerStarted = true;
        g_ownedLogEfzAccepting = true;
        g_consoleIngressAccepting.store(
            enableRawIngress, std::memory_order_release);
        return true;
    }
    catch (...)
    {
        g_ownedLogEfzWorkerStarted = false;
        g_ownedLogEfzAccepting = false;
        g_consoleIngressAccepting.store(false, std::memory_order_release);
        g_consoleIngressPollingEnabled.store(false, std::memory_order_release);
        mod::Log(
            "CAPTURE_LOG: failed to start managed logEfz worker; "
            "mirror disabled to keep disk I/O off the simulation thread");
        return false;
    }
}

static void AppendOwnedLogEfzLine(const char* sourceTag, const std::string& line)
{
    if (line.empty())
    {
        return;
    }

    // Deferred-parse path: when the parse producing this mirror line is
    // already running ON the worker, write directly instead of re-enqueueing.
    // A re-enqueued line would land BEHIND any already-queued session close
    // boundary and leak into the next session's file; the direct write keeps
    // strict FIFO order (the worker is the only mirror writer).
    if (g_ownedLogEfzWorkerThreadId != 0
        && GetCurrentThreadId() == g_ownedLogEfzWorkerThreadId)
    {
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        WriteOwnedLogEfzLine(sourceTag, line, now);
        return;
    }

    OwnedLogEfzEntry entry;
    entry.sourceTag = sourceTag != nullptr ? sourceTag : "unknown";
    entry.line = line;
    GetLocalTime(&entry.timestamp);

    {
        std::lock_guard<std::mutex> lock(g_ownedLogEfzQueueMutex);
        if (!g_ownedLogEfzWorkerStarted || g_ownedLogEfzWorkerStopping
            || !g_ownedLogEfzAccepting
            || InterlockedCompareExchange(
                   &g_ownedLogEfzEmergencyStop, 0, 0) != 0)
        {
            ++g_ownedLogEfzDroppedLines;
            return;
        }
        if (g_ownedLogEfzQueue.size() >= kOwnedLogEfzQueueLimit)
        {
            // Preserve all previously accepted output and the ordering of its
            // eventual session barrier. Dropping is explicit and observable.
            ++g_ownedLogEfzDroppedLines;
            return;
        }
        entry.sequence = ++g_ownedLogEfzEnqueuedSequence;
        entry.ingressBoundary = CaptureConsoleIngressBoundary();
        entry.ingressLossGeneration =
            g_consoleIngressLossGeneration.load(std::memory_order_acquire);
        g_ownedLogEfzQueue.push_back(std::move(entry));
    }
    g_ownedLogEfzQueueCv.notify_one();
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

// Wait-free, fixed-capacity raw ingress. There is deliberately no allocation,
// mutex, notification, logging, retry loop, or synchronous parse here. A
// producer losing the single reservation CAS drops its complete write rather
// than contending with another intercepted thread.
static bool EnqueueConsoleParseChunk(
    const char* sourceTag,
    const void* data,
    size_t bytes,
    bool wide,
    LONG controlWakeRequestSerialSnapshot)
{
    if (data == nullptr || bytes == 0)
    {
        return true; // nothing to parse
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

static bool EnqueueConsoleParseFlush()
{
    OwnedLogEfzEntry entry;
    entry.kind = OwnedLogEfzEntry::Kind::ParseFlush;

    {
        std::lock_guard<std::mutex> lock(g_ownedLogEfzQueueMutex);
        if (!g_ownedLogEfzWorkerStarted || g_ownedLogEfzWorkerStopping
            || !g_ownedLogEfzAccepting
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
        try
        {
            // Control records are not subject to the mirror-line limit.
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
    return true;
}

void StopManagedLogEfzWorker(bool waitForDrain)
{
    if (!waitForDrain)
    {
        // Loader-lock/process-termination path. Do not acquire a mutex that a
        // suspended thread could own, and do not wait for disk I/O.
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
        if (!g_ownedLogEfzWorkerStarted)
        {
            g_consoleIngressAccepting.store(false, std::memory_order_release);
            return;
        }
        // Stop accepting before the final FIFO close marker. Nothing can
        // reopen the file between this boundary and worker termination.
        g_ownedLogEfzAccepting = false;
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
        g_ownedLogEfzWorkerStopping = true;
        joinWorker = g_ownedLogEfzWorker.joinable();
    }
    g_ownedLogEfzQueueCv.notify_all();

    if (joinWorker)
    {
        g_ownedLogEfzWorker.join();
    }
    {
        std::lock_guard<std::mutex> lock(g_ownedLogEfzQueueMutex);
        g_ownedLogEfzWorkerStarted = false;
        g_ownedLogEfzWorkerStopping = false;
        g_ownedLogEfzAccepting = false;
        g_consoleIngressAccepting.store(false, std::memory_order_release);
        g_consoleIngressPollingEnabled.store(false, std::memory_order_release);
        g_ownedLogEfzQueue.clear();
        // Clear the worker id so a recycled OS thread id can't satisfy the
        // direct-mirror-write check between worker generations.
        g_ownedLogEfzWorkerThreadId = 0;
    }
    g_ownedLogEfzDrainCv.notify_all();
}

static void ResetLogEfzDiskHandleCache(); // forward declaration

static void CloseOwnedLogEfzForBoundary()
{
    std::lock_guard<std::mutex> lock(g_ownedLogEfzMutex);
    CloseOwnedLogEfzCurrentFileLocked("close_mirror_logs");

    if (!g_ownedLogEfzCurrentPath.empty())
    {
        mod::Log(
            "CAPTURE_LOG: closed managed logEfz current='%s'",
            g_ownedLogEfzCurrentPath.c_str());
    }
    g_ownedLogEfzCurrentPath.clear();
}

void CloseMirrorLogFiles()
{
    unsigned long long closeSequence = 0;
    {
        // Pause new reservations and wait only for bounded in-flight copies.
        // Both controls use the resulting ring watermark; writes accepted
        // after ingress resumes remain behind the close and therefore cannot
        // leak into the prior session's managed log.
        std::lock_guard<std::mutex> parserLock(g_consoleLogMutex);
        std::lock_guard<std::mutex> queueLock(g_ownedLogEfzQueueMutex);
        if (g_ownedLogEfzWorkerStarted
            && !g_ownedLogEfzWorkerStopping
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
            // Control records are never dropped, even at the line limit.
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
            // Worker unavailable: no raw producer ever parses synchronously.
            // Flush only fragments assembled earlier by the worker.
            g_consoleIngressAccepting.store(false, std::memory_order_release);
            FlushPendingConsoleOutputLocked();
        }
    }
    if (closeSequence != 0)
    {
        g_ownedLogEfzQueueCv.notify_one();
        // FIFO ordering, not synchronous completion, defines the boundary.
        // Waiting here would put managed-mirror disk latency back onto the
        // active Revival tick that calls this function before exported init().
    }
    else
    {
        CloseOwnedLogEfzForBoundary();
    }
    ResetLogEfzDiskHandleCache();
}

void PrimeManagedLogEfzHistory()
{
    // Append-only writing keeps no history; opening the file up front still
    // matters so the fresh-launch truncation (and its log line) happens at
    // startup rather than at the first captured write.
    (void)StartConsoleCaptureWorker(false);
    std::lock_guard<std::mutex> lock(g_ownedLogEfzMutex);
    (void)EnsureOwnedLogEfzFilesOpenLocked();
}

void BeginManagedLogEfzWrite()
{
}

void EndManagedLogEfzWrite()
{
}

bool IsManagedLogEfzWriteActive()
{
    return false;
}

// --- Preclassified WriteFile handles --------------------------------------
// StubCreateFileA/W performs path classification next to the inherently
// blocking native open and publishes positive text-log classifications here.
// StubWriteFile can then make a single bounded lookup without file APIs,
// strings, locks, or logging. Unknown handles (including non-text files and
// stdout/stderr pipes inherited before our IAT hook) are still handed to the
// worker and filtered there. Never cache an "ignore" classification: a closed
// numeric handle can be reused without passing through our CreateFile stub.
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

    // Inject synthetic newline when cursor Y changes.  Revival uses cursor
    // moves for newlines - actual '\n' characters are never written via
    // WriteConsoleOutputCharacterA.
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

    // Inject synthetic newline when cursor Y changes.  Revival uses cursor
    // moves for newlines - actual '\n' characters are never written via
    // WriteConsoleOutputCharacterW.
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

} // namespace netplay::bridge::takeover
