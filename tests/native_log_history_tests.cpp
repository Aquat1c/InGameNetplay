#include "../src/netplay/bridge/native_log_history.cpp"

#include <iostream>
#include <vector>

namespace
{
int failures = 0;
bool preserve = false;
void Expect(bool value, const char* message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
std::string Read(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { Expect(false, "log can be read"); return {}; }
    DWORD size = GetFileSize(file, nullptr), read = 0;
    std::string bytes(size, '\0');
    Expect(ReadFile(file, &bytes[0], size, &read, nullptr) != FALSE && read == size, "log bytes are readable");
    CloseHandle(file);
    return bytes;
}
void Write(HANDLE file, const char* text)
{
    Expect(file != INVALID_HANDLE_VALUE, "log handle is valid");
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(std::strlen(text));
    Expect(WriteFile(file, text, size, &written, nullptr) != FALSE && written == size, "native write succeeds");
}
void Seed(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Write(file, "previous-launch\n");
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}
void ResetLaunch(const std::wstring& root, bool keep, bool adopted = false)
{
    using namespace netplay::bridge::takeover;
    g_nativeLogPath.clear();
    g_nativeLogOpened = false;
    g_nativeLogSession = 0;
    preserve = keep;
    ConfigureNativeLogHistory(root, adopted);
}
HANDLE Open(const std::wstring& path)
{
    return netplay::bridge::takeover::OpenHostLogFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

ULONG_PTR* ImportSlot(HMODULE module, const char* name)
{
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor)
    {
        if (descriptor->OriginalFirstThunk == 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++slots)
            if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)
                && std::strcmp(reinterpret_cast<const char*>(base + names->u1.AddressOfData + 2), name) == 0)
                return &slots->u1.Function;
    }
    return nullptr;
}
void TestNativeCrt(const std::wstring& root, const wchar_t* fixture)
{
    using namespace netplay::bridge::takeover;
    HMODULE module = LoadLibraryW(fixture);
    Expect(module != nullptr, "native CRT fixture loads");
    if (module == nullptr) return;
    auto* slot = ImportSlot(module, "CreateFileW");
    auto* writeSlot = ImportSlot(module, "WriteFile");
    Expect(slot != nullptr && writeSlot != nullptr, "native fixture imports open and write");
    using WriteFn = int (__cdecl*)(const wchar_t*, const char*);
    auto write = reinterpret_cast<WriteFn>(GetProcAddress(module, "WriteNativeLog"));
    Expect(write != nullptr, "native writer is exported");
    if (slot != nullptr && writeSlot != nullptr && write != nullptr)
    {
        const ULONG_PTR originalWrite = *writeSlot;
        DWORD previous = 0;
        const bool writable = VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous) != FALSE;
        Expect(writable, "native open slot is writable");
        if (writable)
        {
            const ULONG_PTR originalOpen = *slot;
            *slot = reinterpret_cast<ULONG_PTR>(&OpenHostLogFileW);
            DWORD ignored = 0;
            VirtualProtect(slot, sizeof(*slot), previous, &ignored);
            ResetLaunch(root, false);
            const std::wstring path = root + L"\\logEfz.txt";
            Seed(path);
            Expect(write(path.c_str(), "match-one\n") != 0, "native CRT first session writes");
            Expect(write(path.c_str(), "match-two\n") != 0, "native CRT reopen writes");
            const std::string bytes = Read(path);
            Expect(bytes.find("previous-launch") == std::string::npos
                && bytes.find("match-one") != std::string::npos && bytes.find("match-two") != std::string::npos,
                "native CRT truncating reopens preserve every match within the launch");
            Expect(bytes.find('\0') == std::string::npos, "native CRT history contains no zero-filled gaps");
            Expect(*writeSlot == originalWrite, "native WriteFile import stays untouched");
            VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous);
            *slot = originalOpen;
            VirtualProtect(slot, sizeof(*slot), previous, &ignored);
        }
    }
    FreeLibrary(module);
}
}

namespace mod { void Log(const char*, ...) {} }
namespace netplay::mod_settings { bool PreserveRevivalLogsAcrossLaunches() { return preserve; } }

int wmain(int argc, wchar_t** argv)
{
    using namespace netplay::bridge::takeover;
    wchar_t temp[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) return 1;
    const std::wstring root = std::wstring(temp) + L"efz_log_history_" + std::to_wstring(GetCurrentProcessId());
    if (!CreateDirectoryW(root.c_str(), nullptr)) return 1;
    const std::wstring path = root + L"\\logEfz.txt";
    for (bool keep : {false, true})
    {
        Seed(path);
        ResetLaunch(root, keep);
        HANDLE first = Open(path);
        Write(first, "first-match\n");
        HANDLE second = Open(path);
        Write(second, "second-match\n");
        SetFilePointer(first, 0, nullptr, FILE_BEGIN);
        Write(first, "late-first-match\n");
        if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
        if (first != INVALID_HANDLE_VALUE) CloseHandle(first);
        const std::string bytes = Read(path);
        Expect((bytes.find("previous-launch") != std::string::npos) == keep, "existing preservation setting controls first open");
        Expect(bytes.find("first-match") != std::string::npos && bytes.find("second-match") != std::string::npos
            && bytes.find("late-first-match") != std::string::npos, "overlapping native handles append without overwriting");
        Expect(bytes.find("SESSION 1") != std::string::npos && bytes.find("SESSION 2") != std::string::npos,
            "native opens retain session separators");
    }

    Seed(path);
    ResetLaunch(root, false);
    HANDLE truncated = OpenHostLogFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Write(truncated, "truncate-existing-first\n");
    if (truncated != INVALID_HANDLE_VALUE) CloseHandle(truncated);
    Expect(Read(path).find("previous-launch") == std::string::npos,
        "TRUNCATE_EXISTING first open resets this launch with append-only rights");
    HANDLE truncateAgain = OpenHostLogFileW(path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Write(truncateAgain, "truncate-existing-second\n");
    if (truncateAgain != INVALID_HANDLE_VALUE) CloseHandle(truncateAgain);
    Expect(Read(path).find("truncate-existing-first") != std::string::npos,
        "later TRUNCATE_EXISTING opens retain earlier sessions");

    Seed(path);
    ResetLaunch(root, false, true);
    HANDLE adopted = Open(path);
    Write(adopted, "adopted-session\n");
    if (adopted != INVALID_HANDLE_VALUE) CloseHandle(adopted);
    Expect(Read(path).find("previous-launch") != std::string::npos, "adoption preserves already-started native log");

    Seed(path);
    ResetLaunch(root, false);
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(locked != INVALID_HANDLE_VALUE, "exclusive lock fixture opens");
    HANDLE blocked = Open(path);
    const DWORD blockedError = GetLastError();
    Expect(blocked == INVALID_HANDLE_VALUE && blockedError == ERROR_SHARING_VIOLATION, "locked log returns original failure");
    Expect(!g_nativeLogOpened, "failed first open does not consume launch reset");
    if (blocked != INVALID_HANDLE_VALUE) CloseHandle(blocked);
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    HANDLE retry = Open(path);
    Write(retry, "retried\n");
    if (retry != INVALID_HANDLE_VALUE) CloseHandle(retry);
    Expect(Read(path).find("previous-launch") == std::string::npos, "successful retry still applies launch reset");

    char ansi[MAX_PATH] = {};
    WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, ansi, MAX_PATH, nullptr, nullptr);
    HANDLE narrow = OpenHostLogFileA(ansi, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Write(narrow, "ansi-reopen\n");
    if (narrow != INVALID_HANDLE_VALUE) CloseHandle(narrow);
    Expect(Read(path).find("retried") != std::string::npos, "ANSI and wide opens share launch history");

    const std::wstring other = root + L"\\other.txt";
    Seed(other);
    HANDLE unchanged = Open(other);
    Write(unchanged, "replacement\n");
    if (unchanged != INVALID_HANDLE_VALUE) CloseHandle(unchanged);
    Expect(Read(other) == "replacement\n", "unrelated files retain native truncation behavior");

    const std::wstring unicodeRoot = root + L"\\\u6C38\u9060";
    Expect(CreateDirectoryW(unicodeRoot.c_str(), nullptr) != FALSE, "Unicode directory exists");
    ResetLaunch(unicodeRoot, true);
    const std::wstring unicodePath = unicodeRoot + L"\\logEfz.txt";
    Seed(unicodePath);
    HANDLE unicode = Open(unicodePath);
    Write(unicode, "unicode\n");
    if (unicode != INVALID_HANDLE_VALUE) CloseHandle(unicode);
    Expect(Read(unicodePath).find("previous-launch") != std::string::npos, "Unicode path retains history");

    Expect(argc == 2, "native CRT fixture argument is present");
    if (argc == 2) TestNativeCrt(root, argv[1]);
    Expect(DeleteFileW(unicodePath.c_str()) && RemoveDirectoryW(unicodeRoot.c_str())
        && DeleteFileW(other.c_str()) && DeleteFileW(path.c_str()) && RemoveDirectoryW(root.c_str()),
        "isolated log fixtures are removed");
    return failures == 0 ? 0 : 1;
}
