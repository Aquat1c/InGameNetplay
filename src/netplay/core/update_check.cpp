#include "netplay/core/update_check.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <string>

#include "logger.h"
#include "mod_version.h"
#include "netplay/core/lobby_client.h"
#include "netplay/core/mod_settings.h"
#include "netplay/core/tls_http_client.h"
#include "netplay/core/version_compare.h"

namespace netplay::update_check
{
namespace
{
// GitHub's "latest release" endpoint (drafts and pre-releases excluded). One
// small JSON document; we only read "tag_name". Unauthenticated rate limit is
// 60/h per IP - a single request per process is far inside that.
constexpr const char kLatestReleaseApiUrl[] =
    "https://api.github.com/repos/Aquat1c/InGameNetplay/releases/latest";
constexpr const char kReleasesPageUrl[] =
    "https://github.com/Aquat1c/InGameNetplay/releases/latest";

// Mod-owned state file next to the DLL. Deliberately NOT EfzRevival.ini: the
// options menu rewrites that file from its own cached line snapshot on save,
// which would silently drop a key written underneath it while it is open.
constexpr const char kStateFileName[] = "update_check.ini";
constexpr const char kStateSection[] = "UpdateCheck";
constexpr const char kStateKeyAcknowledged[] = "AcknowledgedVersion";

constexpr DWORD kConnectTimeoutMs = 6000;
constexpr DWORD kReceiveTimeoutMs = 10000;
constexpr size_t kMaxTagLength = 32;

struct Lock
{
    CRITICAL_SECTION cs;
    Lock() { InitializeCriticalSection(&cs); }
    ~Lock() { DeleteCriticalSection(&cs); }
};
Lock g_lock;

struct ScopedLock
{
    explicit ScopedLock(Lock& l) : m_l(l) { EnterCriticalSection(&m_l.cs); }
    ~ScopedLock() { LeaveCriticalSection(&m_l.cs); }
    Lock& m_l;
};

volatile LONG g_started = 0;          // Start() ran (thread spawned or gated off)
volatile LONG g_newerRelease = 0;     // fetch done: latest > build
volatile LONG g_updateAvailable = 0;  // g_newerRelease && not acknowledged
std::string g_latest;                 // guarded by g_lock
std::string g_acknowledged;           // guarded by g_lock

std::string ModuleDirectory()
{
    char modulePath[MAX_PATH] = {};
    HMODULE selfModule = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&ModuleDirectory),
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

std::string StateFilePath()
{
    const std::string dir = ModuleDirectory();
    if (dir.empty())
    {
        return {};
    }
    return dir + "\\" + kStateFileName;
}

std::string ReadAcknowledgedFromDisk()
{
    const std::string path = StateFilePath();
    if (path.empty())
    {
        return {};
    }
    char buffer[64] = {};
    GetPrivateProfileStringA(
        kStateSection, kStateKeyAcknowledged, "", buffer,
        static_cast<DWORD>(sizeof(buffer)), path.c_str());
    return buffer;
}

bool WriteAcknowledgedToDisk(const std::string& version)
{
    const std::string path = StateFilePath();
    if (path.empty())
    {
        return false;
    }
    return WritePrivateProfileStringA(
               kStateSection, kStateKeyAcknowledged, version.c_str(), path.c_str())
        != FALSE;
}

// Tag charset guard: a release tag is "0.5.0" / "0.4.2_beta1" / "v1.0"; anything
// else (or anything long) is treated as unparsable rather than displayed.
bool IsSafeTag(const std::string& tag)
{
    if (tag.empty() || tag.size() > kMaxTagLength)
    {
        return false;
    }
    for (char c : tag)
    {
        const auto uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '.' || c == '_' || c == '-'))
        {
            return false;
        }
    }
    return true;
}

// Minimal extractor for a top-level string member. GitHub tags never carry
// escapes; a backslash inside the value fails the parse on purpose.
bool ExtractJsonString(const std::string& json, const char* key, std::string* out)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
    {
        return false;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
    {
        return false;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos)
    {
        return false;
    }
    const size_t end = json.find('"', pos + 1);
    if (end == std::string::npos)
    {
        return false;
    }
    const std::string value = json.substr(pos + 1, end - pos - 1);
    if (value.find('\\') != std::string::npos)
    {
        return false;
    }
    *out = value;
    return true;
}

std::string NormalizeTag(std::string tag)
{
    if (tag.size() >= 2 && (tag[0] == 'v' || tag[0] == 'V')
        && std::isdigit(static_cast<unsigned char>(tag[1])))
    {
        tag.erase(0, 1);
    }
    return tag;
}

void RecomputeAvailabilityLocked()
{
    const bool newer = !g_latest.empty()
        && netplay::version::IsNewer(g_latest, netplay::build_info::kVersion);
    InterlockedExchange(&g_newerRelease, newer ? 1 : 0);
    InterlockedExchange(&g_updateAvailable, (newer && g_latest != g_acknowledged) ? 1 : 0);
}

bool FetchLatestReleaseJson(std::string* outBody)
{
    // Embedded mbedTLS first: deterministic on every OS the mod supports (XP's
    // SChannel cannot reach GitHub's TLS 1.2-only endpoint). WinINet second, for
    // a build without the embedded backend or an exotic network setup.
    std::string error;
    if (netplay::tls::HttpGet(
            kLatestReleaseApiUrl, /*verifyPeer=*/true,
            kConnectTimeoutMs, kReceiveTimeoutMs, outBody, &error)
        && !outBody->empty())
    {
        return true;
    }
    const std::string tlsError = error.empty() ? "unavailable" : error;
    *outBody = netplay::lobby::HttpGetViaWinInet(
        kLatestReleaseApiUrl, kConnectTimeoutMs, kReceiveTimeoutMs);
    if (!outBody->empty())
    {
        mod::Log("UpdateCheck: embedded TLS failed (%s); WinINet succeeded",
                 tlsError.c_str());
        return true;
    }
    mod::Log("UpdateCheck: fetch failed (embedded TLS: %s; WinINet: no response)",
             tlsError.c_str());
    return false;
}

DWORD WINAPI WorkerThreadProc(LPVOID)
{
    std::string body;
    if (!FetchLatestReleaseJson(&body))
    {
        return 0;
    }
    std::string tag;
    if (!ExtractJsonString(body, "tag_name", &tag))
    {
        mod::Log("UpdateCheck: response had no tag_name (%u bytes)",
                 static_cast<unsigned>(body.size()));
        return 0;
    }
    tag = NormalizeTag(tag);
    if (!IsSafeTag(tag))
    {
        mod::Log("UpdateCheck: ignoring unexpected tag format");
        return 0;
    }
    bool newer = false;
    bool available = false;
    {
        ScopedLock guard(g_lock);
        g_latest = tag;
        RecomputeAvailabilityLocked();
        newer = g_newerRelease != 0;
        available = g_updateAvailable != 0;
    }
    mod::Log("UpdateCheck: latest release %s, running %s -> %s%s",
             tag.c_str(),
             netplay::build_info::kVersion,
             newer ? "update available" : "up to date",
             (newer && !available) ? " (already acknowledged)" : "");
    return 0;
}
} // namespace

void Start()
{
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0)
    {
        return;   // once per process
    }
    if (!netplay::mod_settings::IsUpdateCheckEnabled())
    {
        mod::Log("UpdateCheck: disabled (CheckForUpdates=0)");
        return;
    }
    {
        ScopedLock guard(g_lock);
        g_acknowledged = ReadAcknowledgedFromDisk();
    }
    HANDLE thread = CreateThread(nullptr, 0, WorkerThreadProc, nullptr, 0, nullptr);
    if (thread == nullptr)
    {
        mod::Log("UpdateCheck: worker thread creation failed (%lu)",
                 static_cast<unsigned long>(GetLastError()));
        return;
    }
    CloseHandle(thread);
}

std::string LatestVersion()
{
    ScopedLock guard(g_lock);
    return g_latest;
}

bool HasNewerRelease()
{
    return InterlockedCompareExchange(&g_newerRelease, 0, 0) != 0;
}

bool IsUpdateAvailable()
{
    return InterlockedCompareExchange(&g_updateAvailable, 0, 0) != 0;
}

const char* BadgeText()
{
    return IsUpdateAvailable() ? "[!]" : "";
}

void AcknowledgeLatest()
{
    if (!IsUpdateAvailable())
    {
        return;   // nothing newer, or this tag was already acknowledged
    }
    std::string latest;
    {
        ScopedLock guard(g_lock);
        latest = g_latest;
        g_acknowledged = latest;
        RecomputeAvailabilityLocked();
    }
    if (!WriteAcknowledgedToDisk(latest))
    {
        mod::Log("UpdateCheck: could not persist acknowledgement of %s", latest.c_str());
    }
    else
    {
        mod::Log("UpdateCheck: acknowledged %s (badge hidden until a newer release)",
                 latest.c_str());
    }
}

const char* ReleasesPageUrl()
{
    return kReleasesPageUrl;
}

bool OpenReleasesPage()
{
    // shell32 is resolved at call time so the DLL keeps no static import on it
    // (the mod links no shell32.lib today; this is the only shell call).
    using ShellExecuteAFn = HINSTANCE(WINAPI*)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
    HMODULE shell32 = LoadLibraryA("shell32.dll");
    if (shell32 == nullptr)
    {
        mod::Log("UpdateCheck: shell32 unavailable; cannot open %s", kReleasesPageUrl);
        return false;
    }
    const auto shellExecute =
        reinterpret_cast<ShellExecuteAFn>(GetProcAddress(shell32, "ShellExecuteA"));
    if (shellExecute == nullptr)
    {
        mod::Log("UpdateCheck: ShellExecuteA missing; cannot open %s", kReleasesPageUrl);
        return false;
    }
    const auto result = reinterpret_cast<INT_PTR>(
        shellExecute(nullptr, "open", kReleasesPageUrl, nullptr, nullptr, SW_SHOWNORMAL));
    const bool ok = result > 32;   // ShellExecute reports success as a value above 32
    mod::Log("UpdateCheck: open %s in browser -> %s (%ld)",
             kReleasesPageUrl, ok ? "ok" : "failed", static_cast<long>(result));
    return ok;
}
} // namespace netplay::update_check
