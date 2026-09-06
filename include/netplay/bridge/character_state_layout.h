#pragma once

#include <cstddef>
#include <cstdint>

// Character-state layout shared by the in-mod diagnostics and the observe-only
// stock probe.  EfzRevival 1.02h indexes its savestate copy-size table with
// character[141].  Keep this table explicit: using the smallest entry hides
// the character-specific tail bytes (including Nayuki's final 0x10 bytes).
namespace netplay::bridge::character_state_layout
{
inline constexpr std::size_t kCharacterIdOffset = 141u;
inline constexpr std::uint8_t kCharacterCount = 25u;
inline constexpr std::uint8_t kNayukiCharacterId = 17u;
inline constexpr std::size_t kMinimumSavestateBytes = 0x3448u;
inline constexpr std::size_t kMaximumSavestateBytes = 0x34B0u;

inline constexpr std::size_t kSavestateBytesByCharacterId[kCharacterCount] = {
    0x3470u, //  0 Nanase
    0x3450u, //  1 Ayu
    0x3458u, //  2 Mai
    0x3450u, //  3 Makoto
    0x3450u, //  4 Akane
    0x3448u, //  5 Mayu
    0x34B0u, //  6 Mizuka Nagamori
    0x3448u, //  7 Misaki
    0x3458u, //  8 Shiori
    0x3458u, //  9 Sayuri
    0x3460u, // 10 Neyuki
    0x3458u, // 11 Mio
    0x3468u, // 12 Doppel Nanase
    0x3450u, // 13 Kaori
    0x3478u, // 14 Ikumi
    0x3478u, // 15 Mishio
    0x3468u, // 16 Akiko
    0x3458u, // 17 Nayuki (Sleepy)
    0x3460u, // 18 UNKNOWN
    0x3458u, // 19 Kanna
    0x3478u, // 20 Kano
    0x3458u, // 21 Minagi
    0x3460u, // 22 Neyuki (alternate)
    0x3488u, // 23 Misuzu
    0x3460u, // 24 UNKNOWN(boss?)
};

// Zero is deliberately the invalid-ID result.  Callers must fail closed and
// skip the region rather than falling back to a minimum or maximum size.
constexpr std::size_t SavestateBytesForCharacterId(std::uint8_t id) noexcept
{
    return id < kCharacterCount ? kSavestateBytesByCharacterId[id] : 0u;
}

static_assert(
    SavestateBytesForCharacterId(kNayukiCharacterId) == 0x3458u,
    "Nayuki savestate coverage must include the final 0x10 bytes");
static_assert(
    SavestateBytesForCharacterId(6u) == kMaximumSavestateBytes,
    "maximum character-state dump slot is too small");
static_assert(
    SavestateBytesForCharacterId(25u) == 0u,
    "invalid character IDs must fail closed");
}
