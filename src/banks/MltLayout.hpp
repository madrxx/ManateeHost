#pragma once
#include "support/binaryFile.hpp"

namespace manatee {
constexpr std::uint32_t ramSize = 0x200000;

struct MltEntry {
    std::uint32_t map, address, allocation, offset, size;
    bool stored; // is there data in this? else, it's just reserved
    bool mpb; // is this an MPB?
    unsigned bank; // bank slot number
};

constexpr unsigned mpbProgramCountOffset = 0x14;
bool isMpb(std::span<const std::uint8_t> bytes);

class MltLayout {
    public:
        explicit MltLayout(Bytes bytes);
        static MltLayout FromMpbs(const std::vector<Bytes> &banks, const Bytes &fpb = {}, const Bytes &fob = {});

        const Bytes &Data() const { return bytes; }
        const std::vector<MltEntry> &Entries() const { return entries; }

    private:
        Bytes bytes;
        std::vector<MltEntry> entries;
};
} // namespace manatee