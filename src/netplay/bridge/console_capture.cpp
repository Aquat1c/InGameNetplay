// Console I/O text capture and delay prompt parsing for the Revival takeover.

#include "netplay/bridge/takeover_internal.h"
#include "netplay/core/mod_settings.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <share.h>

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
struct NativeMatchStateSnapshot
{
    bool valid;
    int matchIndex;
    int currentFrame;
    int currentState;
    int inputSizeA;
    int inputSizeB;
    int inputDelay;
    int maxRoll;
    DWORD updateTick;
};

static NativeMatchStateSnapshot g_lastNativeMatchState = {};
static DWORD g_lastNativeMatchStateLogTick = 0;
constexpr DWORD kNativeMatchStateLogIntervalMs = 30000u;
struct NativeInterestingLine
{
    DWORD tick;
    std::string text;
};

static std::deque<NativeInterestingLine> g_recentNativeInterestingLines;
constexpr size_t kMaxRecentNativeInterestingLines = 64u;
constexpr size_t kMaxLoggedNativeInterestingContextLines = 12u;

bool ContainsAnyCaseInsensitive(
    const std::string& text,
    const char* const* needles,
    size_t needleCount)
{
    if (needles == nullptr || needleCount == 0)
    {
        return false;
    }

    for (size_t i = 0; i < needleCount; ++i)
    {
        const char* needle = needles[i];
        if (needle != nullptr && needle[0] != '\0'
            && ContainsCaseInsensitive(text, needle))
        {
            return true;
        }
    }
    return false;
}

const char* MatchNativeDesyncSignal(const std::string& text)
{
    static const char* kSignals[] = {
        "Desync detected",
        "header crc mismatch",
        "state not recoverable",
    };

    for (const char* signal : kSignals)
    {
        if (ContainsCaseInsensitive(text, signal))
        {
            return signal;
        }
    }
    return nullptr;
}

const char* MatchNativeDesyncWarning(const std::string& text)
{
    static const char* kWarnings[] = {
        "CurrentFrame > playerInput.size",
        "Passing null state",
        "Remote is using a deprecated version",
    };

    for (const char* warning : kWarnings)
    {
        if (ContainsCaseInsensitive(text, warning))
        {
            return warning;
        }
    }
    return nullptr;
}

bool IsInterestingNativeDebugLine(const std::string& text)
{
    static const char* kKeywords[] = {
        "Desync detected",
        "header crc mismatch",
        "state not recoverable",
        "CurrentFrame > playerInput.size",
        "Passing null state",
        "CurrentFrame",
        "Current State",
        "StateP1",
        "StateP2",
        "Frame Step:",
        "inputDelay",
        "MaxRollback",
        "Frames to log:",
        "Loc Frame",
        "Rem Frame",
        "Render Frame",
        "Add frame, remote is ahead",
        "Pause, need more remote frames",
        "Pause, too few remote inputs, ping",
        "Pause, too little inputDelay",
        "Full Sync queue",
        "Update, available frames",
        "Connection timed out",
        "Source quit or timed out",
        "Host timed out",
        "Remote timed out",
        "Peer died",
        "Socket error",
        "Received quit from:",
        "Recv quit from:",
        "Remote is using a deprecated version",
    };
    return ContainsAnyCaseInsensitive(
        text,
        kKeywords,
        sizeof(kKeywords) / sizeof(kKeywords[0]));
}

void RememberNativeInterestingLine(const std::string& text)
{
    if (text.empty() || !IsInterestingNativeDebugLine(text))
    {
        return;
    }

    const DWORD now = GetTickCount();
    if (!g_recentNativeInterestingLines.empty()
        && g_recentNativeInterestingLines.back().text == text)
    {
        g_recentNativeInterestingLines.back().tick = now;
        return;
    }

    g_recentNativeInterestingLines.push_back({now, text});
    while (g_recentNativeInterestingLines.size() > kMaxRecentNativeInterestingLines)
    {
        g_recentNativeInterestingLines.pop_front();
    }
}

void LogRecentNativeInterestingContext(const char* reason)
{
    if (g_recentNativeInterestingLines.empty())
    {
        return;
    }

    const size_t total = g_recentNativeInterestingLines.size();
    const size_t start =
        (total > kMaxLoggedNativeInterestingContextLines)
            ? (total - kMaxLoggedNativeInterestingContextLines)
            : 0u;

    mod::Log(
        "Takeover: native debug context before '%s' lines=%lu showing=%lu",
        reason != nullptr ? reason : "",
        static_cast<unsigned long>(total),
        static_cast<unsigned long>(total - start));

    const DWORD now = GetTickCount();
    for (size_t i = start; i < total; ++i)
    {
        const NativeInterestingLine& entry = g_recentNativeInterestingLines[i];
        const DWORD ageMs = (entry.tick != 0) ? (now - entry.tick) : 0;
        mod::Log(
            "Takeover: native debug[%lu/%lu] ageMs=%lu text='%s'",
            static_cast<unsigned long>(i - start + 1),
            static_cast<unsigned long>(total - start),
            static_cast<unsigned long>(ageMs),
            entry.text.c_str());
    }
}

bool TryParseNativeMatchStateLine(
    const std::string& text,
    NativeMatchStateSnapshot* outSnapshot)
{
    if (outSnapshot == nullptr)
    {
        return false;
    }

    NativeMatchStateSnapshot snapshot = {};
    snapshot.valid = false;
    snapshot.matchIndex = -1;
    snapshot.currentFrame = -1;
    snapshot.currentState = -1;
    snapshot.inputSizeA = -1;
    snapshot.inputSizeB = -1;
    snapshot.inputDelay = -1;
    snapshot.maxRoll = -1;
    snapshot.updateTick = GetTickCount();

    if (!ContainsCaseInsensitive(text, "CurrentFrame")
        || !ContainsCaseInsensitive(text, "Current State"))
    {
        return false;
    }

    if (!ExtractIntAfterToken(text, "Match", &snapshot.matchIndex)
        || !ExtractIntAfterToken(text, "CurrentFrame", &snapshot.currentFrame)
        || !ExtractIntAfterToken(text, "Current State:", &snapshot.currentState))
    {
        return false;
    }

    const size_t inputSizesPos = text.find("input sizes");
    if (inputSizesPos != std::string::npos)
    {
        size_t afterFirst = 0;
        if (ParseIntAt(text,
                       inputSizesPos + std::strlen("input sizes"),
                       &snapshot.inputSizeA,
                       &afterFirst))
        {
            (void)ParseIntAt(text, afterFirst, &snapshot.inputSizeB, nullptr);
        }
    }

    (void)ExtractIntAfterToken(text, "inputDelay", &snapshot.inputDelay);
    (void)ExtractIntAfterToken(text, "MaxRoll", &snapshot.maxRoll);
    snapshot.valid = true;
    *outSnapshot = snapshot;
    return true;
}

void LogNativeMatchStateSnapshot(
    const char* prefix,
    const NativeMatchStateSnapshot& snapshot,
    const std::string& rawText)
{
    mod::Log(
        "%s match=%d frame=%d state=%d input=%d/%d delay=%d maxRoll=%d text='%s'",
        prefix != nullptr ? prefix : "Takeover: native match snapshot",
        snapshot.matchIndex,
        snapshot.currentFrame,
        snapshot.currentState,
        snapshot.inputSizeA,
        snapshot.inputSizeB,
        snapshot.inputDelay,
        snapshot.maxRoll,
        rawText.c_str());
}

void LogLastNativeMatchStateContext(const char* reason)
{
    if (!g_lastNativeMatchState.valid)
    {
        return;
    }

    DWORD ageMs = 0;
    if (g_lastNativeMatchState.updateTick != 0)
    {
        ageMs = GetTickCount() - g_lastNativeMatchState.updateTick;
    }

    mod::Log(
        "Takeover: native match context before '%s' match=%d frame=%d "
        "state=%d input=%d/%d delay=%d maxRoll=%d ageMs=%lu",
        reason != nullptr ? reason : "",
        g_lastNativeMatchState.matchIndex,
        g_lastNativeMatchState.currentFrame,
        g_lastNativeMatchState.currentState,
        g_lastNativeMatchState.inputSizeA,
        g_lastNativeMatchState.inputSizeB,
        g_lastNativeMatchState.inputDelay,
        g_lastNativeMatchState.maxRoll,
        static_cast<unsigned long>(ageMs));
}

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
    g_lastNativeMatchState = {};
    g_lastNativeMatchStateLogTick = 0;
    g_recentNativeInterestingLines.clear();
}

void NoteConsolePromptLine(const std::string& text)
{
    if (text.empty())
    {
        return;
    }

    RememberNativeInterestingLine(text);

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
    NativeMatchStateSnapshot nativeMatchState = {};
    if (TryParseNativeMatchStateLine(text, &nativeMatchState))
    {
        const bool firstMatchLoop = !g_nativeWorkflowMatchLoopSeen;
        const bool hadPreviousSnapshot = g_lastNativeMatchState.valid;
        const bool stateChanged =
            !hadPreviousSnapshot
            || nativeMatchState.currentState != g_lastNativeMatchState.currentState;
        const bool frameRestarted =
            hadPreviousSnapshot
            && nativeMatchState.currentFrame < g_lastNativeMatchState.currentFrame;

        g_nativeWorkflowMatchLoopSeen = true;
        g_lastNativeMatchState = nativeMatchState;

        if (firstMatchLoop)
        {
            mod::Log(
                "Takeover: native workflow event=match_loop_started text='%s'",
                text.c_str());
        }

        if (firstMatchLoop
            || stateChanged
            || frameRestarted
            || nativeMatchState.updateTick - g_lastNativeMatchStateLogTick
                   >= kNativeMatchStateLogIntervalMs)
        {
            LogNativeMatchStateSnapshot(
                "Takeover: native match snapshot",
                nativeMatchState,
                text);
            g_lastNativeMatchStateLogTick = nativeMatchState.updateTick;
        }
    }
    if (!g_nativeWorkflowTournamentSeen && ContainsCaseInsensitive(text, "Starting Tournament Mode"))
    {
        g_nativeWorkflowTournamentSeen = true;
        mod::Log("Takeover: native workflow event=unexpected_tournament_mode text='%s'", text.c_str());
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
            LogLastNativeMatchStateContext("Peer died");
            LogRecentNativeInterestingContext("Peer died");
            // Also publish through IPC as a backup signal so the host process
            // can detect the disconnect even if no explicit timeout message
            // follows (e.g. "Host timed out" or "Remote timed out" may arrive
            // later, but this ensures immediate detection).
            PublishConsoleError("Peer died");
        }
    }

    // --- Connection error detection ---
    // Revival prints these messages to console on failure. Publish them
    // through IPC so the host process can transition to Failed phase.
    if (ContainsCaseInsensitive(text, "Connection timed out"))
    {
        mod::Log("Takeover: console error detected='Connection timed out' text='%s'", text.c_str());
        LogLastNativeMatchStateContext("Connection timed out");
        LogRecentNativeInterestingContext("Connection timed out");
        PublishConsoleError("Connection timed out");
        return;
    }
    if (ContainsCaseInsensitive(text, "Source quit or timed out"))
    {
        mod::Log("Takeover: console error detected='Source quit or timed out' text='%s'", text.c_str());
        LogLastNativeMatchStateContext("Source quit or timed out");
        LogRecentNativeInterestingContext("Source quit or timed out");
        PublishConsoleError("Source quit or timed out");
        return;
    }
    if (ContainsCaseInsensitive(text, "Host timed out"))
    {
        mod::Log("Takeover: console error detected='Host timed out' text='%s'", text.c_str());
        LogLastNativeMatchStateContext("Host timed out");
        LogRecentNativeInterestingContext("Host timed out");
        PublishConsoleError("Host timed out");
        return;
    }
    if (ContainsCaseInsensitive(text, "Remote timed out"))
    {
        mod::Log("Takeover: console error detected='Remote timed out' text='%s'", text.c_str());
        LogLastNativeMatchStateContext("Remote timed out");
        LogRecentNativeInterestingContext("Remote timed out");
        PublishConsoleError("Remote timed out");
        return;
    }
    if (ContainsCaseInsensitive(text, "Spectators have been disabled"))
    {
        mod::Log("Takeover: console error detected='Spectators disabled' text='%s'", text.c_str());
        PublishConsoleError("Spectators have been disabled by the host");
        return;
    }
    if (ContainsCaseInsensitive(text, "Socket error"))
    {
        mod::Log("Takeover: console error detected='Socket error' text='%s'", text.c_str());
        LogLastNativeMatchStateContext("Socket error");
        LogRecentNativeInterestingContext("Socket error");
        // Use the full text since it includes the socket error details
        std::string errorMsg = text;
        if (errorMsg.size() > 120)
        {
            errorMsg.resize(120);
        }
        PublishConsoleError(errorMsg.c_str());
        return;
    }

    if (const char* desyncSignal = MatchNativeDesyncSignal(text))
    {
        mod::Log(
            "Takeover: native desync signal detected='%s' text='%s'",
            desyncSignal,
            text.c_str());
        LogLastNativeMatchStateContext(desyncSignal);
        LogRecentNativeInterestingContext(desyncSignal);
        PublishConsoleError(desyncSignal);
        return;
    }

    if (const char* warningSignal = MatchNativeDesyncWarning(text))
    {
        mod::Log(
            "Takeover: native desync warning detected='%s' text='%s'",
            warningSignal,
            text.c_str());
        LogLastNativeMatchStateContext(warningSignal);
        LogRecentNativeInterestingContext(warningSignal);
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
    PublishDelayPromptSerial(serial);
    g_injectedDelayPromptWaitStartTick = GetTickCount();
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

bool TryGetDiskFilePathFromHandle(HANDLE hFile, std::string* outPath)
{
    if (outPath == nullptr || hFile == nullptr || hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    SetLastError(NO_ERROR);
    const DWORD fileType = GetFileType(hFile);
    if (fileType != FILE_TYPE_DISK)
    {
        return false;
    }

    typedef DWORD (WINAPI *PFN_GetFinalPathNameByHandleA)(HANDLE, LPSTR, DWORD, DWORD);
    static PFN_GetFinalPathNameByHandleA s_pfnGetFinalPath = []() -> PFN_GetFinalPathNameByHandleA {
        HMODULE kernel = GetModuleHandleA("kernel32.dll");
        return kernel
            ? reinterpret_cast<PFN_GetFinalPathNameByHandleA>(GetProcAddress(kernel, "GetFinalPathNameByHandleA"))
            : nullptr;
    }();

    if (s_pfnGetFinalPath == nullptr)
    {
        return false;
    }

    char path[1024] = {};
    const DWORD pathLen = s_pfnGetFinalPath(
        hFile,
        path,
        static_cast<DWORD>(sizeof(path)),
        FILE_NAME_NORMALIZED);
    if (pathLen == 0 || pathLen >= sizeof(path))
    {
        return false;
    }

    *outPath = std::string(path, pathLen);
    return true;
}

bool TryGetLogEfzDiskPath(HANDLE hFile, std::string* outPath)
{
    std::string pathText;
    if (TryGetRedirectedNativeLogEfzHandlePath(hFile, &pathText))
    {
        if (outPath != nullptr)
        {
            *outPath = pathText;
        }
        return true;
    }

    if (!TryGetDiskFilePathFromHandle(hFile, &pathText))
    {
        return false;
    }

    std::string lowerPath = pathText;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const size_t slash = lowerPath.find_last_of("\\/");
    const std::string baseName = (slash == std::string::npos) ? lowerPath : lowerPath.substr(slash + 1);
    if (baseName != "logefz.txt")
    {
        return false;
    }

    if (outPath != nullptr)
    {
        *outPath = pathText;
    }
    return true;
}

static void AppendOwnedLogEfzLine(const char* sourceTag, const std::string& line);

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
        // Parse workflow signals (delay prompt, spectate confirm, peer died)
        // but don't echo Revival's debug text into the mod log — Revival
        // already writes to its own log files.
        NoteConsolePromptLine(trimmed);
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

    // Revival writes characters without newlines via WriteConsoleOutputCharacterW
    // (newlines are cursor moves only). Check for known workflow/error keywords
    // in the partially-accumulated buffer and flush immediately so prompts and
    // failures are visible to the bridge without waiting for a newline.
    if (line != nullptr && !line->empty() && line->size() >= 12)
    {
        static const char* kImmediateFlushKeywords[] = {
            "Connection timed out",
            "Source quit or timed out",
            "Host timed out",
            "Remote timed out",
            "Spectators have been disabled",
            "Socket error",
            "Desync detected",
            "header crc mismatch",
            "state not recoverable",
            "CurrentFrame > playerInput.size",
            "Passing null state",
            "Remote is using a deprecated version",
            "Host already playing, join as a spectator",
            "Host not yet playing, join as a player",
            "Waiting for game to begin",
            "Enter the initial input delay",
            "Enter the input delay to use",
        };
        for (const char* kw : kImmediateFlushKeywords)
        {
            if (line->find(kw) != std::string::npos)
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

void FlushPendingConsoleOutput(const char* /*reason*/)
{
    // NOTE: CaptureRevivalNativeLogsEnabled() gate intentionally removed.
    // Flushing must always happen so NoteConsolePromptLine can detect
    // workflow signals (errors, delay prompts, spectate confirms).

    std::lock_guard<std::mutex> lock(g_consoleLogMutex);
    auto flushOne = [&](const char* /*sourceTag*/, std::string* line) {
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

        // Parse workflow signals but don't echo Revival debug text.
        NoteConsolePromptLine(trimmed);
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

// ---------------------------------------------------------------------------
// Managed logEfz capture — host-side ownership of the actual EFZ disk log.
//
// Rather than trying to repair a corrupted native logEfz.txt after the fact,
// we capture the host process's intercepted disk writes and append them to a
// single clean root logEfz.txt. Each init session writes a clear separator so
// the file remains readable across reconnects/restarts.
// ---------------------------------------------------------------------------
static std::mutex g_ownedLogEfzMutex;
static HANDLE g_ownedLogEfzCurrentHandle = INVALID_HANDLE_VALUE;
static std::string g_ownedLogEfzCurrentPath;
static std::string g_ownedLogEfzHistory;
static std::string g_ownedLogEfzHistoryPath;
static bool g_ownedLogEfzHistoryPrimed = false;
static uint32_t g_ownedLogEfzSessionOrdinal = 0;

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

std::string GetManagedLogEfzPath()
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

std::string GetNativeLogEfzShadowPath()
{
    const std::string modDir = GetConsoleCaptureModuleDirectory();
    if (modDir.empty())
    {
        return {};
    }
    return modDir + "\\logs\\logEfz_native_shadow.txt";
}

static unsigned long long QueryExistingFileSize(const std::string& path, bool* outExists)
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

    char riskyDetail[640] = {};
    if (detail != nullptr && detail[0] != '\0')
    {
        std::snprintf(
            riskyDetail,
            sizeof(riskyDetail),
            "outcome=%s current='%s' %s",
            outcome,
            path.c_str(),
            detail);
    }
    else
    {
        std::snprintf(
            riskyDetail,
            sizeof(riskyDetail),
            "outcome=%s current='%s'",
            outcome,
            path.c_str());
    }
    LogRiskyPathEvent("ManagedLogEfz", riskyDetail);
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

static void SanitizeOwnedLogEfzHistoryLocked(const std::string& path)
{
    if (g_ownedLogEfzHistory.empty())
    {
        return;
    }

    const size_t bytesBefore = g_ownedLogEfzHistory.size();
    const size_t firstNonNul = g_ownedLogEfzHistory.find_first_not_of('\0');
    if (firstNonNul == std::string::npos)
    {
        mod::Log(
            "CAPTURE_LOG: discarded all-zero managed logEfz history current='%s' bytes=%lu",
            path.c_str(),
            static_cast<unsigned long>(bytesBefore));
        char detail[256] = {};
        std::snprintf(
            detail,
            sizeof(detail),
            "reason=all_zero_history bytesDiscarded=%lu",
            static_cast<unsigned long>(bytesBefore));
        LogOwnedLogEfzIntegrityIssue("discarded_history", path, detail);
        g_ownedLogEfzHistory.clear();
        return;
    }

    size_t trimmedLeadingNuls = 0;
    if (firstNonNul > 0)
    {
        trimmedLeadingNuls = firstNonNul;
        g_ownedLogEfzHistory.erase(0, firstNonNul);
    }

    const auto newEnd = std::remove(g_ownedLogEfzHistory.begin(), g_ownedLogEfzHistory.end(), '\0');
    const size_t removedEmbeddedNuls = static_cast<size_t>(g_ownedLogEfzHistory.end() - newEnd);
    g_ownedLogEfzHistory.erase(newEnd, g_ownedLogEfzHistory.end());

    if (trimmedLeadingNuls > 0 || removedEmbeddedNuls > 0)
    {
        mod::Log(
            "CAPTURE_LOG: sanitized managed logEfz history current='%s' bytesBefore=%lu bytesAfter=%lu trimmedLeadingNuls=%lu removedEmbeddedNuls=%lu",
            path.c_str(),
            static_cast<unsigned long>(bytesBefore),
            static_cast<unsigned long>(g_ownedLogEfzHistory.size()),
            static_cast<unsigned long>(trimmedLeadingNuls),
            static_cast<unsigned long>(removedEmbeddedNuls));
        char detail[320] = {};
        std::snprintf(
            detail,
            sizeof(detail),
            "reason=nul_sanitized bytesBefore=%lu bytesAfter=%lu trimmedLeadingNuls=%lu removedEmbeddedNuls=%lu",
            static_cast<unsigned long>(bytesBefore),
            static_cast<unsigned long>(g_ownedLogEfzHistory.size()),
            static_cast<unsigned long>(trimmedLeadingNuls),
            static_cast<unsigned long>(removedEmbeddedNuls));
        LogOwnedLogEfzIntegrityIssue("history_sanitized", path, detail);
    }
}

static void PrimeOwnedLogEfzHistoryLocked(const std::string& path)
{
    if (path.empty())
    {
        return;
    }

    if (g_ownedLogEfzHistoryPrimed && _stricmp(g_ownedLogEfzHistoryPath.c_str(), path.c_str()) == 0)
    {
        return;
    }

    g_ownedLogEfzHistory.clear();
    if (!netplay::mod_settings::PreserveLogEfzAcrossLaunches())
    {
        bool existed = false;
        const unsigned long long previousBytes = QueryExistingFileSize(path, &existed);
        g_ownedLogEfzHistoryPath = path;
        g_ownedLogEfzHistoryPrimed = true;

        mod::Log(
            "CAPTURE_LOG: starting fresh managed logEfz current='%s' preserveAcrossLaunches=0 previousExists=%d previousBytes=%llu",
            path.c_str(),
            existed ? 1 : 0,
            previousBytes);
        return;
    }

    FILE* file = _fsopen(path.c_str(), "rb", _SH_DENYNO);
    if (file != nullptr)
    {
        if (std::fseek(file, 0, SEEK_END) == 0)
        {
            const long size = std::ftell(file);
            if (size > 0 && std::fseek(file, 0, SEEK_SET) == 0)
            {
                g_ownedLogEfzHistory.resize(static_cast<size_t>(size));
                const size_t read = std::fread(
                    g_ownedLogEfzHistory.data(),
                    1,
                    g_ownedLogEfzHistory.size(),
                    file);
                g_ownedLogEfzHistory.resize(read);
                if (read != static_cast<size_t>(size))
                {
                    char detail[256] = {};
                    std::snprintf(
                        detail,
                        sizeof(detail),
                        "reason=prime_short_read expected=%lu read=%lu ferror=%d",
                        static_cast<unsigned long>(size),
                        static_cast<unsigned long>(read),
                        std::ferror(file) ? 1 : 0);
                    LogOwnedLogEfzIntegrityIssue("prime_short_read", path, detail);
                }
            }
        }
        std::fclose(file);
    }

    SanitizeOwnedLogEfzHistoryLocked(path);

    g_ownedLogEfzHistoryPath = path;
    g_ownedLogEfzHistoryPrimed = true;

    mod::Log(
        "CAPTURE_LOG: primed managed logEfz history current='%s' bytes=%lu",
        path.c_str(),
        static_cast<unsigned long>(g_ownedLogEfzHistory.size()));
}

static bool EnsureOwnedLogEfzFilesOpenLocked()
{
    if (g_ownedLogEfzCurrentHandle != INVALID_HANDLE_VALUE)
    {
        return true;
    }

    g_ownedLogEfzCurrentPath = GetManagedLogEfzPath();
    if (g_ownedLogEfzCurrentPath.empty())
    {
        return false;
    }

    PrimeOwnedLogEfzHistoryLocked(g_ownedLogEfzCurrentPath);
    g_ownedLogEfzCurrentHandle = CreateFileA(
        g_ownedLogEfzCurrentPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
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
            "reason=rebuild_open_failed winerr=%lu",
            static_cast<unsigned long>(GetLastError()));
        LogOwnedLogEfzIntegrityIssue("open_failed", g_ownedLogEfzCurrentPath, detail);
        g_ownedLogEfzCurrentPath.clear();
        return false;
    }

    if (!g_ownedLogEfzHistory.empty())
    {
        if (!WriteOwnedLogEfzBytes(
            g_ownedLogEfzCurrentPath,
            "rebuild_history",
            g_ownedLogEfzHistory.data(),
            g_ownedLogEfzHistory.size()))
        {
            CloseOwnedLogEfzCurrentFileLocked("rebuild_history_failed");
            return false;
        }
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
    g_ownedLogEfzHistory.append(header);

    mod::Log(
        "CAPTURE_LOG: opened managed logEfz current='%s' session=%lu mode=rebuild historyBytes=%lu preserveAcrossLaunches=%d",
        g_ownedLogEfzCurrentPath.c_str(),
        static_cast<unsigned long>(g_ownedLogEfzSessionOrdinal),
        static_cast<unsigned long>(g_ownedLogEfzHistory.size()),
        netplay::mod_settings::PreserveLogEfzAcrossLaunches() ? 1 : 0);
    return true;
}

static void AppendOwnedLogEfzLine(const char* sourceTag, const std::string& line)
{
    if (line.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(g_ownedLogEfzMutex);
    if (!EnsureOwnedLogEfzFilesOpenLocked())
    {
        return;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);

    char prefix[96] = {};
    std::snprintf(
        prefix,
        sizeof(prefix),
        "[%02u:%02u:%02u.%03u][%s] ",
        static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute),
        static_cast<unsigned>(st.wSecond),
        static_cast<unsigned>(st.wMilliseconds),
        sourceTag != nullptr ? sourceTag : "unknown");

    std::string entry(prefix);
    entry.append(line);
    entry.append("\r\n");
    g_ownedLogEfzHistory.append(entry);
    if (!WriteOwnedLogEfzBytes(
            g_ownedLogEfzCurrentPath,
            "append_entry",
            entry.data(),
            entry.size()))
    {
        CloseOwnedLogEfzCurrentFileLocked("append_entry_failed");
    }
}

void CloseMirrorLogFiles()
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

void PrimeManagedLogEfzHistory()
{
    std::lock_guard<std::mutex> lock(g_ownedLogEfzMutex);
    PrimeOwnedLogEfzHistoryLocked(GetManagedLogEfzPath());
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

void MaybeLogConsoleOutputChunk(HANDLE hFile, LPCVOID lpBuffer, DWORD nBytes)
{
    if (lpBuffer == nullptr || nBytes == 0)
    {
        return;
    }

    const char* text = reinterpret_cast<const char*>(lpBuffer);
    const size_t textLen = static_cast<size_t>(nBytes);
    if (!IsLikelyTextChunk(text, textLen))
    {
        return;
    }

    // Console output can flow through redirected handles (pipes/unknown).
    // Keep console/pipe traffic visible, and selectively forward disk-backed
    // Revival text logs without enabling noisy binary file dumps.
    SetLastError(NO_ERROR);
    const DWORD fileType = GetFileType(hFile);
    if (fileType == FILE_TYPE_DISK)
    {
        std::string pathText;
        if (!TryGetDiskFilePathFromHandle(hFile, &pathText))
        {
            return;
        }
        if (!IsLikelyRevivalDiskLogPath(pathText))
        {
            return;
        }

        bool announcePath = false;
        LONG pathHitCount = 0;
        {
            std::lock_guard<std::mutex> lock(g_consoleLogMutex);
            LONG& hitRef = g_diskCapturePathHits[pathText];
            ++hitRef;
            pathHitCount = hitRef;
            announcePath = (hitRef == 1);
        }
        (void)announcePath;
        if (pathHitCount == 1)
        {
            std::string logEfzPath;
            if (TryGetLogEfzDiskPath(hFile, &logEfzPath))
            {
                mod::Log(
                    "CAPTURE_LOG: observed native logEfz WriteFile path='%s'",
                    logEfzPath.c_str());
            }
        }

        LogConsoleTextChunk("WriteFileDisk", text, textLen);
        return;
    }
    if (fileType == FILE_TYPE_UNKNOWN && GetLastError() != NO_ERROR)
    {
        return;
    }

    LogConsoleTextChunk("WriteFile", text, textLen);
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
    const int wideLen = (std::min)(static_cast<int>(nChars), 0x4000);
    if (wideLen <= 0)
    {
        return;
    }

    int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, wideText, wideLen, nullptr, 0, nullptr, nullptr);
    UINT codePage = CP_UTF8;
    if (utf8Bytes <= 0)
    {
        codePage = CP_ACP;
        utf8Bytes = WideCharToMultiByte(codePage, 0, wideText, wideLen, nullptr, 0, nullptr, nullptr);
    }
    if (utf8Bytes <= 0)
    {
        return;
    }

    std::string utf8;
    utf8.resize(static_cast<size_t>(utf8Bytes));
    if (WideCharToMultiByte(codePage, 0, wideText, wideLen, utf8.data(), utf8Bytes, nullptr, nullptr) <= 0)
    {
        return;
    }

    LogConsoleTextChunk("WriteConsoleW", utf8.c_str(), utf8.size());
}

void MaybeLogConsoleOutputCharacterAChunk(const VOID* lpBuffer, DWORD nChars, COORD writeCoord)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }

    // Inject synthetic newline when cursor Y changes.  Revival uses cursor
    // moves for newlines — actual '\n' characters are never written via
    // WriteConsoleOutputCharacterA.
    static SHORT s_lastY = -1;
    if (s_lastY >= 0 && writeCoord.Y != s_lastY)
    {
        LogConsoleTextChunk("WriteConsoleOutputCharacterA", "\n", 1);
    }
    s_lastY = writeCoord.Y;

    LogConsoleTextChunk("WriteConsoleOutputCharacterA", reinterpret_cast<const char*>(lpBuffer), static_cast<size_t>(nChars));
}

void MaybeLogConsoleOutputCharacterWChunk(const VOID* lpBuffer, DWORD nChars, COORD writeCoord)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }

    // Inject synthetic newline when cursor Y changes.  Revival uses cursor
    // moves for newlines — actual '\n' characters are never written via
    // WriteConsoleOutputCharacterW.
    static SHORT s_lastY = -1;
    if (s_lastY >= 0 && writeCoord.Y != s_lastY)
    {
        LogConsoleTextChunk("WriteConsoleOutputCharacterW", "\n", 1);
    }
    s_lastY = writeCoord.Y;

    const wchar_t* wideText = reinterpret_cast<const wchar_t*>(lpBuffer);
    const int wideLen = (std::min)(static_cast<int>(nChars), 0x4000);
    if (wideLen <= 0)
    {
        return;
    }

    int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, wideText, wideLen, nullptr, 0, nullptr, nullptr);
    UINT codePage = CP_UTF8;
    if (utf8Bytes <= 0)
    {
        codePage = CP_ACP;
        utf8Bytes = WideCharToMultiByte(codePage, 0, wideText, wideLen, nullptr, 0, nullptr, nullptr);
    }
    if (utf8Bytes <= 0)
    {
        return;
    }

    std::string utf8;
    utf8.resize(static_cast<size_t>(utf8Bytes));
    if (WideCharToMultiByte(codePage, 0, wideText, wideLen, utf8.data(), utf8Bytes, nullptr, nullptr) <= 0)
    {
        return;
    }

    LogConsoleTextChunk("WriteConsoleOutputCharacterW", utf8.c_str(), utf8.size());
}

void MaybeLogOutputDebugStringA(LPCSTR lpOutputString)
{
    if (lpOutputString == nullptr || lpOutputString[0] == '\0')
    {
        return;
    }

    const size_t len = std::strlen(lpOutputString);
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

    const int wideLen = static_cast<int>(wcslen(lpOutputString));
    if (wideLen <= 0)
    {
        return;
    }

    int utf8Bytes = WideCharToMultiByte(CP_UTF8, 0, lpOutputString, wideLen, nullptr, 0, nullptr, nullptr);
    UINT codePage = CP_UTF8;
    if (utf8Bytes <= 0)
    {
        codePage = CP_ACP;
        utf8Bytes = WideCharToMultiByte(codePage, 0, lpOutputString, wideLen, nullptr, 0, nullptr, nullptr);
    }
    if (utf8Bytes <= 0)
    {
        return;
    }

    std::string utf8;
    utf8.resize(static_cast<size_t>(utf8Bytes));
    if (WideCharToMultiByte(codePage, 0, lpOutputString, wideLen, utf8.data(), utf8Bytes, nullptr, nullptr) <= 0)
    {
        return;
    }

    LogConsoleTextChunk("OutputDebugStringW", utf8.c_str(), utf8.size());
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
