#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "netplay/interop/overlay_protocol.h"

namespace netplay::interop::source
{
const char* CharFolderName(int charId);

std::uint32_t PackKeyDword(std::uint8_t charId, std::uint8_t sourceFlag,
                           std::uint8_t slot);

bool BuildPalRelPath(std::uint8_t charId, std::uint8_t slot, std::string* out);

bool LoadRawPalFile(const char* absPath, std::uint8_t* out120);

protocol::PaletteBlobBody AssembleRow(std::uint8_t side, std::uint8_t charId,
                                      std::uint8_t slot, bool custom,
                                      const std::uint8_t* raw120);

bool LoadLocalRow(const std::string& baseDir, std::uint8_t side,
                  std::uint8_t charId, std::uint8_t slot,
                  protocol::PaletteBlobBody* out);
}
