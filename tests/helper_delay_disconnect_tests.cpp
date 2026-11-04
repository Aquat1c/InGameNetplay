#include "netplay/bridge/helper_delay_disconnect.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
int failures = 0;
void Expect(bool ok, const char* message)
{
    if (!ok)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Fixture
{
    const char* tag;
    const char* directory;
    DWORD timestamp;
    DWORD imageSize;
    DWORD quitSlot;
    DWORD deathSlot;
    DWORD original;
    DWORD replacement;
};

constexpr Fixture fixtures[] = {
    {"1.02e", "1.02e", 0x5EA87633, 0xFD000, 0x9A010, 0x9A030, 0x44330, 0x496C0},
    {"1.02f", "1.02f", 0x5F8C58B0, 0xFD000, 0x9A010, 0x9A030, 0x44370, 0x496E0},
    {"1.02f-framestepping", "1.02f", 0x5F8C58B0, 0xFD000, 0x9A010, 0x9A030, 0x44370, 0x496E0},
    {"1.02g", "1.02g", 0x623F85D8, 0xFE000, 0x9B010, 0x9B030, 0x446A0, 0x49A50},
    {"1.02h", "1.02h", 0x6292935E, 0xFE000, 0x9B038, 0x9B058, 0x44630, 0x49C00},
    {"1.02i", "1.02i", 0x63BF27B6, 0xFE000, 0x99FD8, 0x99FF8, 0x45780, 0x4AEA0},
    {"1.02j", "1.02j", 0x6A3696C8, 0x294000, 0x1ECE30, 0x1ECE50, 0x82490, 0x94AA0},
};

struct Image
{
    unsigned char* bytes;
    explicit Image(DWORD size)
        : bytes(static_cast<unsigned char*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) {}
    ~Image() { if (bytes != nullptr) VirtualFree(bytes, 0, MEM_RELEASE); }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    HMODULE Module() const { return reinterpret_cast<HMODULE>(bytes); }
    uintptr_t Address(DWORD rva) const { return reinterpret_cast<uintptr_t>(bytes + rva); }
    uintptr_t& Slot(DWORD rva) { return *reinterpret_cast<uintptr_t*>(bytes + rva); }
};

void InitializeSynthetic(Image& image, const Fixture& f)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.bytes);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.bytes + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
    nt->FileHeader.TimeDateStamp = f.timestamp;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
    nt->OptionalHeader.SizeOfImage = f.imageSize;
    nt->OptionalHeader.ImageBase = 0x400000;
    image.Slot(f.quitSlot) = image.Address(f.original);
    image.Slot(f.deathSlot) = image.Address(f.replacement);
    const unsigned char returnTwoArguments[] = {0xC2, 0x08, 0x00};
    std::memcpy(image.bytes + f.original, returnTwoArguments, sizeof(returnTwoArguments));
    const unsigned char head[] = {0x55, 0x8B, 0xEC};
    const unsigned char headJ[] = {0x55, 0x89, 0xE5};
    std::memcpy(image.bytes + f.replacement, std::strcmp(f.tag, "1.02j") == 0 ? headJ : head, sizeof(head));
}

void TestSyntheticImages()
{
    using netplay::bridge::takeover::InstallHelperDelayDisconnectHandler;
    Expect(!InstallHelperDelayDisconnectHandler(nullptr, "1.02h"), "null image is rejected");
    Expect(!InstallHelperDelayDisconnectHandler(reinterpret_cast<HMODULE>(1), "1.02h"), "unreadable image is rejected");
    for (const Fixture& f : fixtures)
    {
        Image image(f.imageSize);
        Expect(image.bytes != nullptr, "synthetic image allocation succeeds");
        if (image.bytes == nullptr) continue;
        InitializeSynthetic(image, f);
        const auto original = image.Slot(f.quitSlot);
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), "unknown"), "unknown profile is rejected");
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), nullptr), "absent profile is rejected");
        Expect(image.Slot(f.quitSlot) == original, "unknown profile leaves vtable untouched");
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image.bytes + 0x80);
        ++nt->FileHeader.TimeDateStamp;
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "mismatched helper timestamp is rejected");
        --nt->FileHeader.TimeDateStamp;
        ++nt->OptionalHeader.SizeOfImage;
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "mismatched helper size is rejected");
        --nt->OptionalHeader.SizeOfImage;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "non-x86 helper is rejected");
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_I386;
        nt->OptionalHeader.ImageBase = 0x12340000;
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "unrelated declared image base is rejected");
        nt->OptionalHeader.ImageBase = 0x400000;
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.bytes);
        dos->e_lfanew = -1;
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "invalid PE header offset is rejected");
        dos->e_lfanew = 0x80;
        image.Slot(f.quitSlot) = image.Address(f.original + 1);
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "unexpected quit callback is preserved");
        Expect(image.Slot(f.quitSlot) == image.Address(f.original + 1), "existing foreign callback is not replaced");
        image.Slot(f.quitSlot) = original;
        image.Slot(f.deathSlot) = image.Address(f.replacement + 1);
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "unexpected death callback is rejected");
        image.Slot(f.deathSlot) = image.Address(f.replacement);
        image.bytes[f.original] = 0x90;
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "nonempty quit callback code is rejected");
        image.bytes[f.original] = 0xC2;
        image.bytes[f.replacement] = 0x90;
        Expect(!InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "mismatched termination callback code is rejected");
        image.bytes[f.replacement] = 0x55;
        const std::vector<unsigned char> before(image.bytes, image.bytes + f.imageSize);
        DWORD previous = 0;
        Expect(VirtualProtect(image.bytes + f.quitSlot, sizeof(uintptr_t), PAGE_READONLY, &previous) != FALSE,
            "fixture vtable becomes read only");
        Expect(InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "known helper callback installs");
        Expect(image.Slot(f.quitSlot) == image.Address(f.replacement), "only delay quit uses the native death callback");
        Expect(image.Slot(f.deathSlot) == image.Address(f.replacement), "native death slot is unchanged");
        Expect(std::memcmp(image.bytes, before.data(), f.quitSlot) == 0
            && std::memcmp(image.bytes + f.quitSlot + sizeof(uintptr_t), before.data() + f.quitSlot + sizeof(uintptr_t),
                f.imageSize - f.quitSlot - sizeof(uintptr_t)) == 0,
            "installation changes only the four-byte quit slot");
        Expect(InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "installation is idempotent");
        MEMORY_BASIC_INFORMATION memory = {};
        VirtualQuery(image.bytes + f.quitSlot, &memory, sizeof(memory));
        Expect(memory.Protect == PAGE_READONLY, "original vtable page protection is restored");

        Image relocated(f.imageSize);
        Expect(relocated.bytes != nullptr, "rebased image allocation succeeds");
        if (relocated.bytes == nullptr) continue;
        InitializeSynthetic(relocated, f);
        auto* relocatedNt = reinterpret_cast<IMAGE_NT_HEADERS32*>(relocated.bytes + 0x80);
        relocatedNt->OptionalHeader.ImageBase = static_cast<DWORD>(relocated.Address(0));
        Expect(InstallHelperDelayDisconnectHandler(relocated.Module(), f.tag),
            "loader-rebased image header accepts validated relocated callbacks");
        Expect(relocated.Slot(f.quitSlot) == relocated.Address(f.replacement),
            "rebased helper installs its native disconnect callback");
    }
}

void VerifyShippedImages(const char* root)
{
    using netplay::bridge::takeover::InstallHelperDelayDisconnectHandler;
    for (const Fixture& f : fixtures)
    {
        const std::string path = std::string(root) + '/' + f.directory + "/EfzRevival.exe";
        std::ifstream stream(path, std::ios::binary);
        const std::vector<unsigned char> file((std::istreambuf_iterator<char>(stream)), {});
        Expect(file.size() >= sizeof(IMAGE_DOS_HEADER), "shipped helper fixture is present");
        if (file.size() < sizeof(IMAGE_DOS_HEADER)) continue;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
        if (dos->e_lfanew < 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) > file.size())
        {
            Expect(false, "fixture PE headers are bounded");
            continue;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(file.data() + dos->e_lfanew);
        Expect(nt->FileHeader.TimeDateStamp == f.timestamp && nt->OptionalHeader.SizeOfImage == f.imageSize,
            "shipped helper identity matches the calibrated profile");
        Image image(f.imageSize);
        if (image.bytes == nullptr || nt->OptionalHeader.SizeOfHeaders > file.size()
            || nt->OptionalHeader.SizeOfHeaders > f.imageSize)
        {
            Expect(false, "fixture image headers fit mapped image");
            continue;
        }
        std::memcpy(image.bytes, file.data(), nt->OptionalHeader.SizeOfHeaders);
        const auto* section = IMAGE_FIRST_SECTION(nt);
        bool bounded = true;
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            if (reinterpret_cast<const unsigned char*>(&section[i + 1]) > file.data() + file.size()
                || static_cast<uint64_t>(section[i].PointerToRawData) + section[i].SizeOfRawData > file.size()
                || static_cast<uint64_t>(section[i].VirtualAddress) + section[i].SizeOfRawData > f.imageSize)
            {
                bounded = false;
                break;
            }
            std::memcpy(image.bytes + section[i].VirtualAddress,
                file.data() + section[i].PointerToRawData, section[i].SizeOfRawData);
        }
        Expect(bounded, "fixture sections fit the file and mapped image");
        if (!bounded) continue;
        Expect(image.Slot(f.quitSlot) == nt->OptionalHeader.ImageBase + f.original,
            "shipped quit slot points to the calibrated native null handler");
        Expect(image.Slot(f.deathSlot) == nt->OptionalHeader.ImageBase + f.replacement,
            "shipped death slot points to the calibrated native termination handler");
        image.Slot(f.quitSlot) = image.Address(f.original);
        image.Slot(f.deathSlot) = image.Address(f.replacement);
        Expect(InstallHelperDelayDisconnectHandler(image.Module(), f.tag), "shipped helper bytes satisfy installation guards");
        Expect(image.Slot(f.quitSlot) == image.Address(f.replacement), "shipped helper copy installs the native handler");
        std::cout << "Verified helper " << f.tag << '\n';
    }
}
}

int main(int argc, char** argv)
{
    TestSyntheticImages();
    if (argc == 2) VerifyShippedImages(argv[1]);
    return failures == 0 ? 0 : 1;
}
