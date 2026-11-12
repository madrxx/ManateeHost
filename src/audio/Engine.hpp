#pragma once
#include "banks/RamLayout.hpp"
#include <memory>

namespace manatee {
// owns the ARM / AICA emulator and loaded bank map
class Engine {
    public:
        Engine(const Bytes &driver, RamLayout layout);
        void SelectBank(unsigned bank);
        void AllNotesOff();
        void SelectProgram(unsigned program);
        unsigned ProgramCount(unsigned bank);
        void ReplaceBank(unsigned bank, const Bytes &mpb);

        void InputMidi(std::uint8_t status, std::uint8_t d1, std::uint8_t d2); // filtering out midi CC bank select from external sources (since they're not likely to line up with ours)
        void Midi(std::uint8_t status, std::uint8_t d1, std::uint8_t d2);
        bool Render(std::int16_t *output, unsigned frames);
        const std::vector<LayoutEntry> &Entries() const { return layout.Entries(); }

    private:
        std::vector<std::uint64_t> stateStorage;
        RamLayout layout;
        void *State() { return stateStorage.data(); }
        void Upload(std::uint32_t address, const std::uint8_t *data, std::uint32_t size);
        void Command(unsigned slot, std::uint8_t op, std::uint8_t value);
        void Settle(unsigned frames);
};
} // namespace manatee