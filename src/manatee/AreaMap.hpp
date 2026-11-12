#pragma once
#include <cstdint>

// manatee area map in AICA RAM, based on manatools and aitdtnn-pc-overhaul
namespace manatee::areaMap {
constexpr std::uint32_t base = 0x14000;
constexpr unsigned entrySize = 8;
constexpr unsigned size = 0x2a0; // range cleared up to / incl the second MDB entry
constexpr unsigned entryCount = size / entrySize;

constexpr std::uint32_t msb = base;
constexpr std::uint32_t mpb = 0x14080;
constexpr std::uint32_t osb = 0x14100;
constexpr std::uint32_t psr = 0x14180;
constexpr std::uint32_t fpb = 0x14200;
constexpr std::uint32_t fob = 0x14280;
constexpr std::uint32_t fpw = 0x14288;
constexpr std::uint32_t mdb = 0x14290;
} // namespace manatee::areaMap