#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "netplay/interop/overlay_protocol.h"

namespace netplay::interop::source
{
const char* CharFolderName(int charId);

// Pack the engine palette key: slot | (sourceFlag<<16) | (charId<<24) (matches PaletteRow.keyDword / stream_flags_pack_dword).
std::uint32_t PackKeyDword(std::uint8_t charId, std::uint8_t sourceFlag,
                           std::uint8_t slot);

// .pal relative path format: "<folder>\<folder><slot+1>.pal" (e.g. "nanase\nanase1.pal"); false for unknown charId.
bool BuildPalRelPath(std::uint8_t charId, std::uint8_t slot, std::string* out);

bool LoadRawPalFile(const char* absPath, std::uint8_t* out120);

protocol::PaletteBlobBody AssembleRow(std::uint8_t side, std::uint8_t charId,
                                      std::uint8_t slot, bool custom,
                                      const std::uint8_t* raw120);

bool LoadLocalRow(const std::string& baseDir, std::uint8_t side,
                  std::uint8_t charId, std::uint8_t slot,
                  protocol::PaletteBlobBody* out);
} // namespace netplay::interop::source
