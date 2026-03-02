// Console I/O text capture and delay prompt parsing for the Revival takeover.

#include "netplay/bridge/takeover_internal.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwchar>

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

void ResetNativeWorkflowFlags()
{
    g_nativeWorkflowLoadedSeen = false;
    g_nativeWorkflowMatchLoopSeen = false;
    g_nativeWorkflowTournamentSeen = false;
    g_nativeWorkflowPeerDiedSeen = false;
    g_nativeWorkflowHolePunchDiedSeen = false;
    g_holePunchServerConfigLoaded = false;
    g_configuredHolePunchServer.clear();
}

void NoteConsolePromptLine(const std::string& text)
{
    if (text.empty())
    {
        return;
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

    const bool isSpectateConfirmPrompt =
        ContainsCaseInsensitive(text, "Host already playing, join as a spectator");

    if (isSpectateConfirmPrompt)
    {
        const LONG serial = InterlockedIncrement(&g_injectedSpectateConfirmPromptSerial);
        PublishSpectateConfirmPromptSerial(serial);
        g_injectedSpectateConfirmPromptWaitStartTick = GetTickCount();
        mod::Log(
            "Takeover: console prompt detected type=spectate_confirm serial=%ld text='%s'",
            static_cast<long>(serial),
            text.c_str());
        return;
    }

    const bool isDelayPrompt =
        ContainsCaseInsensitive(text, "Enter the initial input delay")
        || ContainsCaseInsensitive(text, "Enter the input delay to use");

    if (!isDelayPrompt)
    {
        return;
    }

    const LONG serial = InterlockedIncrement(&g_injectedDelayPromptSerial);
    PublishDelayPromptSerial(serial);
    g_injectedDelayPromptWaitStartTick = GetTickCount();
    bool hasMetrics = false;
    const DelayPromptMetrics metrics = ParseDelayPromptMetricsFromText(text, &hasMetrics);
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
    if (!CaptureRevivalNativeLogsEnabled())
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

    auto flushLine = [&](bool /*partial*/) {
        const std::string trimmed = TrimAscii(*line);
        line->clear();
        if (trimmed.empty())
        {
            return;
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

    // For unknown/untracked sources, don't hold partial fragments indefinitely.
    if (SelectPendingConsoleLine(sourceTag) == nullptr && !line->empty())
    {
        flushLine(true);
    }
}

void FlushPendingConsoleOutput(const char* /*reason*/)
{
    if (!CaptureRevivalNativeLogsEnabled())
    {
        std::lock_guard<std::mutex> lock(g_consoleLogMutex);
        g_consolePendingWriteFile.clear();
        g_consolePendingWriteFileDisk.clear();
        g_consolePendingWriteConsoleA.clear();
        g_consolePendingWriteConsoleW.clear();
        g_consolePendingWriteConsoleOutputCharacterA.clear();
        g_consolePendingWriteConsoleOutputCharacterW.clear();
        g_consolePendingOutputDebugStringA.clear();
        g_consolePendingOutputDebugStringW.clear();
        return;
    }

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

void MaybeLogConsoleOutputChunk(HANDLE hFile, LPCVOID lpBuffer, DWORD nBytes)
{
    if (lpBuffer == nullptr || nBytes == 0)
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
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
        // GetFinalPathNameByHandleA is Vista+ (not available on XP).
        // Resolve it at runtime so the binary links on XP but still uses
        // the API on newer systems where it exists.
        typedef DWORD (WINAPI *PFN_GetFinalPathNameByHandleA)(HANDLE, LPSTR, DWORD, DWORD);
        static PFN_GetFinalPathNameByHandleA s_pfnGetFinalPath = []() -> PFN_GetFinalPathNameByHandleA {
            HMODULE kernel = GetModuleHandleA("kernel32.dll");
            return kernel
                ? reinterpret_cast<PFN_GetFinalPathNameByHandleA>(GetProcAddress(kernel, "GetFinalPathNameByHandleA"))
                : nullptr;
        }();

        if (s_pfnGetFinalPath == nullptr)
        {
            // Running on XP — cannot resolve disk paths, skip disk capture.
            return;
        }

        char path[1024] = {};
        const DWORD pathLen = s_pfnGetFinalPath(
            hFile,
            path,
            static_cast<DWORD>(sizeof(path)),
            FILE_NAME_NORMALIZED);
        if (pathLen == 0 || pathLen >= sizeof(path))
        {
            return;
        }

        const std::string pathText(path, pathLen);
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
        (void)pathHitCount;

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
    if (!CaptureRevivalNativeLogsEnabled())
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
    if (!CaptureRevivalNativeLogsEnabled())
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

void MaybeLogConsoleOutputCharacterAChunk(const VOID* lpBuffer, DWORD nChars)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
    {
        return;
    }

    LogConsoleTextChunk("WriteConsoleOutputCharacterA", reinterpret_cast<const char*>(lpBuffer), static_cast<size_t>(nChars));
}

void MaybeLogConsoleOutputCharacterWChunk(const VOID* lpBuffer, DWORD nChars)
{
    if (lpBuffer == nullptr || nChars == 0)
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
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

    LogConsoleTextChunk("WriteConsoleOutputCharacterW", utf8.c_str(), utf8.size());
}

void MaybeLogOutputDebugStringA(LPCSTR lpOutputString)
{
    if (lpOutputString == nullptr || lpOutputString[0] == '\0')
    {
        return;
    }
    if (!CaptureRevivalNativeLogsEnabled())
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
    if (!CaptureRevivalNativeLogsEnabled())
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
