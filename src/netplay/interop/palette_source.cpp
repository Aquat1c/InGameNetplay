#include "netplay/interop/palette_source.h"

#include <windows.h>

#include <cstring>

namespace netplay::interop::source
{
const char* CharFolderName(int charId)
{
    // EFZ Memorial select-id -> resource folder. Mirrors EfzRevival's
    // GetCharacterResourceName. VERIFY against on-disk folders before shipping
    // custom sends (a wrong name degrades silently to the stock palette).
    switch (charId)
    {
    case 0:  return "nanase";
    case 1:  return "ayu";
    case 2:  return "mai";
    case 3:  return "makoto";
    case 4:  return "akane";
    case 5:  return "mayu";
    case 6:  return "nagamori";
    case 7:  return "misaki";
    case 8:  return "shiori";
    case 9:  return "sayuri";
    case 10: return "nayuki";     // Nayuki (asleep)
    case 11: return "mio";
    case 12: return "exnanase";   // Doppel Nanase
    case 13: return "kaori";
    case 14: return "ikumi";
    case 15: return "mishio";
    case 16: return "akiko";
    case 17: return "nayukib";
    case 18: return "mizukab";
    case 19: return "kanna";
    case 20: return "kano";
    case 21: return "minagi";
    case 22: return "nayuki";     // Nayuki (awake) - shares resources
    case 23: return "misuzu";
    default: return nullptr;
    }
}

std::uint32_t PackKeyDword(std::uint8_t charId, std::uint8_t sourceFlag,
                           std::uint8_t slot)
{
    return static_cast<std::uint32_t>(slot)
        | (static_cast<std::uint32_t>(
               static_cast<std::uint32_t>(sourceFlag)
               | (static_cast<std::uint32_t>(charId) << 8)) << 16);
}

bool BuildPalRelPath(std::uint8_t charId, std::uint8_t slot, std::string* out)
{
    const char* folder = CharFolderName(charId);
    if (folder == nullptr || out == nullptr)
    {
        return false;
    }
    char buf[256] = {};
    // "<folder>\<folder><slot+1>.pal" - slots are 0-based here, 1-based on disk.
    std::snprintf(buf, sizeof(buf), "%s\\%s%u.pal",
                  folder, folder, static_cast<unsigned>(slot) + 1u);
    *out = buf;
    return true;
}

bool LoadRawPalFile(const char* absPath, std::uint8_t* out120)
{
    if (absPath == nullptr || out120 == nullptr)
    {
        return false;
    }
    // Win32 file API to match the codebase convention (no CRT fopen).
    //
    // EFZ .pal files are the 40-colour (120-byte) BGR body, optionally preceded
    // by exactly ONE header byte (the palette-script output). The body is always
    // the 120 bytes immediately after that header; anything past it is trailing
    // junk to ignore. So: a file bigger than the body has a 1-byte header, a file
    // exactly the body size has none. Reading the header as colour data would
    // shift every colour by one byte.
    //
    // NOTE: this deliberately replaces the older `size % 3 == 1` test, which only
    // recognised the header when the TOTAL size happened to be 1-mod-3 (e.g. the
    // canonical 121). A file that is "1 header + 120 data + N trailing bytes"
    // defeats it: e.g. a 123-byte nanase3.pal (1 header + 120 data + 2 trailing
    // zeros) is 0-mod-3, so the old test skipped no header and read `[00][119
    // colour bytes]`, shifting every colour and corrupting the palette - while
    // an identical-colour 121-byte nanase4.pal loaded fine. Re-exporting through
    // a palette editor "fixed" nanase3 only because it rewrote the canonical
    // 121-byte layout, dropping the 2 stray bytes.
    HANDLE h = CreateFileA(absPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const DWORD fileSize = GetFileSize(h, nullptr);
    const DWORD headerSize =
        (fileSize != INVALID_FILE_SIZE && fileSize > protocol::kPaletteRawBytes)
            ? 1u
            : 0u;
    bool ok = fileSize != INVALID_FILE_SIZE
        && fileSize >= headerSize + protocol::kPaletteRawBytes;
    if (ok && headerSize != 0)
    {
        ok = SetFilePointer(h, static_cast<LONG>(headerSize), nullptr, FILE_BEGIN)
            != INVALID_SET_FILE_POINTER;
    }
    DWORD read = 0;
    if (ok)
    {
        ok = ReadFile(h, out120,
                      static_cast<DWORD>(protocol::kPaletteRawBytes),
                      &read, nullptr) != FALSE
            && read == protocol::kPaletteRawBytes;
    }
    CloseHandle(h);
    return ok;
}

protocol::PaletteBlobBody AssembleRow(std::uint8_t side, std::uint8_t charId,
                                      std::uint8_t slot, bool custom,
                                      const std::uint8_t* raw120)
{
    protocol::PaletteBlobBody b{};
    b.side = side;
    b.charId = charId;
    b.colorSlot = slot;
    b.sourceFlag = custom ? 1u : 0u;
    b.slotByte = custom ? 1u : 0u;
    b.rawLen = protocol::kPaletteRawBytes;
    b.seq = 0;
    b.keyDword = PackKeyDword(charId, b.sourceFlag, slot);
    if (custom && raw120 != nullptr)
    {
        std::memcpy(b.rawBgr, raw120, protocol::kPaletteRawBytes);
    }
    // else: rawBgr already zeroed by value-init (the CLEAR row).
    return b;
}

bool LoadLocalRow(const std::string& baseDir, std::uint8_t side,
                  std::uint8_t charId, std::uint8_t slot,
                  protocol::PaletteBlobBody* out)
{
    if (out == nullptr)
    {
        return false;
    }
    std::string rel;
    std::uint8_t raw[protocol::kPaletteRawBytes] = {};
    bool custom = false;
    if (BuildPalRelPath(charId, slot, &rel))
    {
        std::string abs = baseDir;
        if (!abs.empty() && abs.back() != '\\' && abs.back() != '/')
        {
            abs.push_back('\\');
        }
        abs += rel;
        custom = LoadRawPalFile(abs.c_str(), raw);
    }
    *out = AssembleRow(side, charId, slot, custom, custom ? raw : nullptr);
    return custom;
}
} // namespace netplay::interop::source
