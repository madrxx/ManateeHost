#pragma once
#include "support/binaryFile.hpp"
#include <optional>
#include <utility>

namespace manatee {
enum class ResourceType { MSB, MPB, OSB, PSR, FPB, FOB, FPW, MDB };

struct ResourceId {
    ResourceType type;
    unsigned slot;
};

struct RamAllocation {
    std::uint32_t address;
    std::uint32_t size;
};

struct LayoutEntry {
    ResourceId id;
    std::uint32_t mapAddress;
    RamAllocation allocation;
    std::optional<Bytes> payload;
};

// resource allocations for upload - not actual AICA RAM
class RamLayout {
    public:
        static RamLayout FromMlt(const Bytes &bytes);
        static RamLayout FromBanks(const std::vector<Bytes> &banks, const Bytes &fpb = {}, const Bytes &fob = {});
        const std::vector<LayoutEntry> &Entries() const { return entries; }

    private:
        explicit RamLayout(std::vector<LayoutEntry> entries) : entries(std::move(entries)) {}
        std::vector<LayoutEntry> entries;

        friend class Engine;
};
} // namespace manatee