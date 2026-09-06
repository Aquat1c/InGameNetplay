#pragma once
// Local palette source for the overlay channel: resolve the engine char id +
// color slot to its .pal file, load the raw bytes, and assemble the wire row we
// broadcast. This is the "what am I wearing" side; the peer's row is applied by
// the (live, game-memory) apply stage which lives elsewhere.
//
// Pure/testable except LoadRawPalFile (plain fopen/fread - exercised in tests
// with a temp file). The char-id -> folder table is the ONE piece of game data
// here; it is centralized and flagged for on-disk verification.

#include <cstddef>
#include <cstdint>
#include <string>

#include "netplay/interop/overlay_protocol.h"

namespace netplay::interop::source
{
// Engine select-id -> character resource folder name. Mirrors EfzRevival's
// GetCharacterResourceName (EFZ Memorial roster). NOTE: verify each entry
// against the actual on-disk character folders before enabling custom sends -
// a wrong name silently loads the stock palette. Returns nullptr for ids with
// no resource folder.
const char* CharFolderName(int charId);

// Pack the engine palette key: slot | (sourceFlag<<16) | (charId<<24)
// (matches PaletteRow.keyDword / stream_flags_pack_dword).
std::uint32_t PackKeyDword(std::uint8_t charId, std::uint8_t sourceFlag,
                           std::uint8_t slot);

// Build the .pal relative path for a slot: "<folder>\<folder><slot+1>.pal".
// Returns false for an unknown charId. `out` receives e.g. "nanase\nanase1.pal".
bool BuildPalRelPath(std::uint8_t charId, std::uint8_t slot, std::string* out);

// Read exactly kPaletteRawBytes (120) from `absPath` into out120. Returns false
// if the file is missing, shorter than 120 bytes, or on any IO error. Extra
// bytes beyond 120 are ignored.
bool LoadRawPalFile(const char* absPath, std::uint8_t* out120);

// Assemble the wire row for our side. When a custom .pal loaded, sourceFlag=1 /
// slotByte=1 and rawBgr holds the file bytes. `raw120` may be null for a stock
// row (sourceFlag=0 / slotByte=0, rawBgr zeroed) - this is the CLEAR that tells
// the peer to revert to the default palette. seq is left 0 (assigned by the
// channel's local-row bookkeeping).
protocol::PaletteBlobBody AssembleRow(std::uint8_t side, std::uint8_t charId,
                                      std::uint8_t slot, bool custom,
                                      const std::uint8_t* raw120);

// Convenience: resolve + load our side's current row from a base directory
// (the EFZ resource root). If "<base>\<folder>\<folder><slot+1>.pal" exists and
// loads, returns a custom row; otherwise returns a stock CLEAR row. Always
// fills *out; returns true if a CUSTOM palette was loaded.
bool LoadLocalRow(const std::string& baseDir, std::uint8_t side,
                  std::uint8_t charId, std::uint8_t slot,
                  protocol::PaletteBlobBody* out);
} // namespace netplay::interop::source
