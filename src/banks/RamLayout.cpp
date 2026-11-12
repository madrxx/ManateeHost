#include "banks/RamLayout.hpp"
#include "banks/Mpb.hpp"
#include "manatee/AreaMap.hpp"
#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace manatee {
namespace {
constexpr std::uint32_t ramSize = 0x200000;

// checks FPB program table and calculates RAM needed for the largest program
std::uint32_t fpbWorkSize(const Bytes &fpb) {
    if (fpb.size() < 16 || !signature(fpb, "SFPB"))
        throw std::runtime_error("Expected an SFPB.");
    
    // read and check program count
    auto count = u32(fpb.data() + 0x0C);
    if (!count || count > (fpb.size() - 16) / 4)
        throw std::runtime_error("Invalid FPB program table.");
    
    // workspace sizes from mltmake
    constexpr std::uint32_t sizes[] = {0x4040, 0x8040, 0x10040, 0x20040, 0};
    std::uint32_t work = 0;
    
    // for each program
    for (unsigned i = 0; i < count; ++i) {
        auto offset = u32(fpb.data() + 0x10 + i * 4);
        // check that program doesn't start before header end,
        // or start after EOF,
        // or start too close to EOF for program structure (sum of manatools FPB pattern)
        if (offset < 16 + count * 4 || offset > fpb.size() || fpb.size() - offset < 0xc40)
            throw std::runtime_error("Invalid or truncated FPB program.");
        
        // check workspace requirement index and update max
        auto index = fpb[offset + 0x20];
        if (index >= std::size(sizes))
            throw std::runtime_error("Unknown FPB workspace index.");
        work = std::max(work, sizes[index]);
    }
    return work;
}

// check FOB has basic structure (manatools hexpat)
void validateFob(const Bytes &fob) {
    if (fob.size() < 16 || !signature(fob, "SFOB"))
        throw std::runtime_error("Expected an SFOB.");
    
    auto count = u32(fob.data() + 0x0C); // get count from offset 12
    if (!count || count > (fob.size() - 16) / 4)
        throw std::runtime_error("Invalid FOB mixer table.");
    
    for (unsigned i = 0; i < count; ++i) {
        auto offset = u32(fob.data() + 0x10 + i * 4);
        
        // check that program doesn't start before header end,
        // or start after EOF,
        // or start too close to EOF for mixer data (sum of manatools FOB pattern)
        if (offset < 16 + count * 4 || offset > fob.size() || fob.size() - offset < 0x20)
            throw std::runtime_error("Invalid FOB mixer data.");
    }
}

// MLT parser based on manatools and aitdtnn-pc-overhaul
std::vector<LayoutEntry> parseMlt(const Bytes &b) {
    if (b.size() < 32 || !signature(b, "SMLT"))
        throw std::runtime_error("Expected an SMLT.");

    // number of entries
    const auto count = u32(b.data() + 8);
    if (count > 256 || count > (b.size() - 32) / 32)
        throw std::runtime_error("Invalid MLT entry count.");
    
    const auto headerEnd = 32 + count * 32;

    std::vector<LayoutEntry> entries;
    std::array<bool, areaMap::entryCount> used{}; // initialise all empty
    
    for (unsigned i = 0; i < count; ++i) {
        const auto *p = b.data() + 32 + i * 32; // for each entry, p points to first byte
        std::string_view type(reinterpret_cast<const char *>(p), 4); // get the entry type tag
        
        // select driver mapping area and set bank slot limit
        // map locations from aitdtnn-pc-overhaul audio_renderer.cpp
        std::uint32_t map = 0;
        unsigned max = 16;
        ResourceType resourceType;
        if (type == "SMSB") {
            resourceType = ResourceType::MSB;
            map = areaMap::msb;
        } else if (type == "SMPB") {
            resourceType = ResourceType::MPB;
            map = areaMap::mpb;
        } else if (type == "SOSB") {
            resourceType = ResourceType::OSB;
            map = areaMap::osb;
        } else if (type == "SPSR") {
            resourceType = ResourceType::PSR;
            map = areaMap::psr;
        } else if (type == "SFPB") {
            resourceType = ResourceType::FPB;
            map = areaMap::fpb;
            max = 1;
        } else if (type == "SFOB") {
            resourceType = ResourceType::FOB;
            map = areaMap::fob;
            max = 1;
        } else if (type == "SFPW") {
            resourceType = ResourceType::FPW;
            map = areaMap::fpw;
            max = 1;
        } else if (type == "SMDB") {
            resourceType = ResourceType::MDB;
            map = areaMap::mdb;
            max = 2;
        } else
            throw std::runtime_error("Unknown MLT entry type: " + std::string(type));

        auto bank = std::uint32_t(p[4]); // slot number
        auto address = u32(p + 8); // loc of allocation in AICA RAM
        auto allocation = u32(p + 12); // size of allocation
        auto offset = u32(p + 16); // offset in MLT
        auto size = u32(p + 20); // size of payload in MLT

        // exceeds number of slots allowed for this type
        if (bank >= max)
            throw std::runtime_error("MLT bank index out of range.");
        
        map += bank * areaMap::entrySize; // area map slot for this bank type and slot
        auto index = (map - areaMap::base) / areaMap::entrySize; // index in used array
        if (used[index])
            throw std::runtime_error("Duplicate MLT bank slot.");
        used[index] = true;

        if (address < 0x15000 || address >= ramSize || // outside allowed range
            (address & 3) != 0 || allocation == 0 || // unaligned or empty
            allocation > ramSize - address) // exceeds available space
            throw std::runtime_error("MLT allocation outside bank RAM or unaligned.");
        
        // overlap if starts before existing range ends or existing range starts before new range ends
        for (const auto &e : entries)
            if (address < e.allocation.address + e.allocation.size && e.allocation.address < address + allocation)
                throw std::runtime_error("Overlapping MLT RAM allocations.");
        
        // check for embedded payload with types that need one
        bool stored = offset != 0xffffffff && type != "SFPW" && type != "SPSR";
        if (!stored && type != "SMPB" && type != "SFPW" && type != "SPSR")
            throw std::runtime_error(
                "MLT entry has no embedded data.");

        // check that data fits
        if (stored && (offset < headerEnd || offset > b.size() || size > b.size() - offset || size > allocation))
            throw std::runtime_error("Invalid MLT payload range.");
        
        // check that MPB is actually valid
        if (stored && type == "SMPB" && !isMpb(std::span(b).subspan(offset, size)))
            throw std::runtime_error("Invalid embedded MPB.");

        std::optional<Bytes> payload;
        if (stored)
            payload.emplace(b.begin() + offset, b.begin() + offset + size);
        entries.push_back({{resourceType, bank}, map, {address, allocation}, std::move(payload)});
    }
    return entries;
}

// add a resource for loose bank layout
void addBank(std::vector<LayoutEntry> &entries, std::uint32_t &nextAddress, ResourceId id, std::uint32_t mapAddress, const Bytes &data, std::uint32_t reserved = 0) {
    if (data.size() > ramSize - nextAddress || reserved > ramSize - nextAddress)
        throw std::runtime_error("Banks do not fit 2MiB AICA RAM.");
    
    auto size = static_cast<std::uint32_t>(data.size());
    auto allocation = ((reserved ? reserved : size) + 31u) & ~31u;
    if (!allocation || allocation > ramSize - nextAddress)
        throw std::runtime_error("Bank allocation exceeds 2MiB AICA RAM after rounding.");
    
    std::optional<Bytes> payload;
    if (!data.empty())
        payload = data;
    entries.push_back({id, mapAddress, {nextAddress, allocation}, std::move(payload)});
    nextAddress += allocation;
}

} // namespace

RamLayout RamLayout::FromMlt(const Bytes &bytes) {
    return RamLayout(parseMlt(bytes));
}

// construct a RAM layout from the chosen loose banks
RamLayout RamLayout::FromBanks(const std::vector<Bytes> &banks, const Bytes &fpb, const Bytes &fob) {
    if (banks.empty() || banks.size() > 16)
        throw std::runtime_error("Choose between 1 and 16 MPB files.");
    
    // if there's no FPB work, = 0, otherwise reserve memory
    auto work = fpb.empty() ? 0 : fpbWorkSize(fpb);
    
    if (!fob.empty())
        validateFob(fob);
    
    auto count = banks.size() + !fpb.empty() + !fob.empty() + (work != 0);
    std::vector<LayoutEntry> entries;
    entries.reserve(count);
    std::uint32_t nextAddress = 0x18000;
    
    // DSP work RAM header entry (per mltmake)
    if (work)
        addBank(entries, nextAddress, {ResourceType::FPW, 0}, areaMap::fpw, {}, work);
    
    // add each MPB
    for (unsigned i = 0; i < banks.size(); ++i) {
        if (!isMpb(banks[i]))
            throw std::runtime_error(
                "Choose an MPB here, not an FPB or other file.");
        addBank(entries, nextAddress, {ResourceType::MPB, i}, areaMap::mpb + i * areaMap::entrySize, banks[i]);
    }

    // add FPB/FOB if present
    if (!fpb.empty())
        addBank(entries, nextAddress, {ResourceType::FPB, 0}, areaMap::fpb, fpb);
    if (!fob.empty())
        addBank(entries, nextAddress, {ResourceType::FOB, 0}, areaMap::fob, fob);
    
    return RamLayout(std::move(entries));
}
} // namespace manatee