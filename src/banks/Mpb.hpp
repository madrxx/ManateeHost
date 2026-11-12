#pragma once
#include "support/binaryFile.hpp"

namespace manatee {

constexpr unsigned mpbProgramCountOffset = 0x14;

inline bool isMpb(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 16 && signature(bytes, "SMPB");
}

} // namespace manatee