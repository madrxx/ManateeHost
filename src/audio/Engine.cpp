// Adapted from blackopsrepl/aitdtnn-pc-overhaul's audio-restoration/src/audio_renderer.cpp (GPLv3)
#include "audio/Engine.hpp"
#include "banks/Mpb.hpp"
#include "manatee/AreaMap.hpp"
#include <array>
#include <stdexcept>
#include <algorithm>
#include <utility>

// Highly Theoretical
extern "C" {
#include "arm.h"
#include "dcsound.h"
#include "yam.h"
}

namespace manatee {
namespace {
// pack MIDI packet for manatee
std::uint32_t packMidi(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    return ((0xf8u | ((status >> 4) & 7u)) << 24) | ((status & 15u) << 16) |
           (std::uint32_t(d1) << 8) | d2;
}

// SDRV file handling
constexpr unsigned driverHeaderSize = 32; // skipped in upload, per AITD
constexpr std::uint32_t maxDriverPayload = 0x15000; // host limit so driver can't bleed into bank allocations

// manatee MIDI ring, based on AITD
constexpr std::uint32_t midiRingBase = 0xb000;
constexpr std::uint32_t midiRingMask = 0xfff;
constexpr std::uint32_t midiProducer = 0x12000;

// manatee command input, based on AITD
constexpr std::uint32_t commandBase = 0x12200;
constexpr std::uint32_t commandReady = 0x12400;
constexpr unsigned commandSize = 16;
constexpr std::uint8_t totalVolumeCommand = 0x81;
constexpr std::uint8_t mixerChangeCommand = 0x82;
constexpr std::uint8_t effectChangeCommand = 0x84;

// emulator
constexpr unsigned cyclesPerFrame = 128;
constexpr unsigned renderChunk = 64;
constexpr unsigned bootFrames = 22050;
constexpr unsigned commandSettleFrames = 512;

constexpr unsigned midiChannels = 16;
} // namespace

void Engine::Upload(std::uint32_t address, const std::uint8_t *data, std::uint32_t size) {
    dcsound_upload_to_ram(State(), address, const_cast<std::uint8_t *>(data), size);
}

// creates and uploads a fixed size command record
void Engine::Command(unsigned slot, std::uint8_t op, std::uint8_t value) {
    std::array<std::uint8_t, commandSize> bytes{op, 0, value};
    Upload(commandBase + slot * commandSize, bytes.data(), commandSize);
}

// calls HT to get next samples to buffer
bool Engine::Render(std::int16_t *out, unsigned frames) {
    while (frames) {
        // inner check to chunk requests we send to HT
        auto count = std::min(frames, renderChunk);
        auto produced = count;
        if (dcsound_execute(State(), static_cast<sint32>(count * cyclesPerFrame), out, &produced) < 0 || // emu error
            produced != count) { // or returned without generating full chunk
            return false;
        }
        out += count * 2;
        frames -= count;
    }
    return true;
}

// lets emulated driver run for x frames, discards sound (this is to let initialisation settle)
void Engine::Settle(unsigned frames) {
    std::array<std::int16_t, renderChunk * 2> scratch{};
    while (frames) {
        auto count = std::min(frames, renderChunk);
        if (!Render(scratch.data(), count)) {
            throw std::runtime_error("Manatee ARM execution failed.");
        }
        frames -= count;
    }
}

Engine::Engine(const Bytes &driver, RamLayout initialLayout) : layout(std::move(initialLayout)) {
    if (driver.size() < driverHeaderSize + 4 ||
        driver.size() > maxDriverPayload + driverHeaderSize || !signature(driver, "SDRV")) {
        throw std::runtime_error("Expected a SDRV manatee.drv file.");
    }
    
    if (driver[4] != 2) {
        throw std::runtime_error("Unsupported Manatee driver version - this only supports V2.");
    }

    // initialise Highly Theoretical
    if (arm_init() < 0 || yam_init() < 0 || dcsound_init() < 0) {
        throw std::runtime_error("Emulator initialisation failed.");
    }

    stateStorage.resize((dcsound_get_state_size() + 7) / 8);

    dcsound_clear_state(State());
    yam_enable_dsp_dynarec(dcsound_get_yam_state(State()), 0); // disabling dynarec (HT dynarec only supports 32-bit x86)
    yam_enable_dsp(dcsound_get_yam_state(State()), 1);

    // upload driver to AICA RAM
    Upload(0, driver.data() + driverHeaderSize, static_cast<std::uint32_t>(driver.size() - driverHeaderSize));
    // run driver to allow it to init - no actual confirmation that it's ready, just a startup allowance so manatee doesn't clear what we're about to install
    Settle(bootFrames);

    // clearing map range before we install our layout
    const std::array<std::uint8_t, areaMap::size> emptyMap{};
    Upload(areaMap::base, emptyMap.data(), static_cast<std::uint32_t>(emptyMap.size()));

    // install each layout entry
    for (const auto &entry : layout.entries) {
        dcsound_setword(State(), entry.mapAddress, entry.allocation.address);
        dcsound_setword(State(), entry.mapAddress + 4, entry.allocation.size);
        if (entry.payload)
            Upload(entry.allocation.address, entry.payload->data(), static_cast<std::uint32_t>(entry.payload->size()));
    }

    // check if there's a stored FPB / FOB and set initial prog/mixer
    for (const auto &entry : layout.entries) {
        if (!entry.payload)
            continue;
        if (entry.id.type == ResourceType::FPB && entry.id.slot == 0)
            Command(1, effectChangeCommand, 0);
        else if (entry.id.type == ResourceType::FOB && entry.id.slot == 0)
            Command(0, mixerChangeCommand, 0);
    }
    
    Command(2, totalVolumeCommand, 0xff);
    
    // tell manatee to process the commands
    const std::uint8_t ready = 1;
    Upload(commandReady, &ready, 1);

    // no confirmation that it's complete, again, just an allowance to let the driver process these
    Settle(commandSettleFrames);
}

void Engine::Midi(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    if (status < 0x80 || // not a MIDI status byte
        status >= 0xf0 || // system message - currently unhandled
        d1 > 127 || // outside MIDI 1.0 range
        d2 > 127) { // outside MIDI 1.0 range
            return;
    }
    
    // find the next write position in MIDI ring
    auto offset = dcsound_getword(State(), midiProducer) & midiRingMask;
    
    // write message
    dcsound_setword(State(), midiRingBase + offset, packMidi(status, d1, d2));
    
    // advance write position
    dcsound_setword(State(), midiProducer, (offset + 4) & midiRingMask);
}

// messages that come in from external sources
void Engine::InputMidi(std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    // check if it's a CC Bank Select message and ignore it - we handle bank changing in-app and a MIDI file might not line up with it
    if ((status & 0xf0) == 0xb0 && (d1 == 0 || d1 == 32))
        return;
    Midi(status, d1, d2);
}

// returns how many programs are in the MPB at the given slot
unsigned Engine::ProgramCount(unsigned bank) {
    for (const auto &entry : layout.entries) {
        if (entry.id.type == ResourceType::MPB && entry.id.slot == bank && entry.payload) {
            if (entry.payload->size() < mpbProgramCountOffset + 4)
                return 0;

            auto count = dcsound_getword(State(), entry.allocation.address + mpbProgramCountOffset);
            return std::min(count, 128u);
        }
    }
    return 0;
}

// sets program on all channels
void Engine::SelectProgram(unsigned program) {
    if (program > 127)
        throw std::runtime_error("MIDI programs range from 0 to 127.");
    for (unsigned channel = 0; channel < midiChannels; ++channel) {
        Midi(0xc0 | channel, static_cast<std::uint8_t>(program), 0);
    }
    Settle(commandSettleFrames);
}

// panic button - doesn't reset bank/prog state
void Engine::AllNotesOff() {
    for (unsigned channel = 0; channel < midiChannels; ++channel) {
        Midi(0xb0 | channel, 64, 0);
        Midi(0xb0 | channel, 120, 0);
        Midi(0xb0 | channel, 123, 0);
    }
    Settle(commandSettleFrames);
}

// switches engine to a loaded MPB slot and then sets every channel program to 0
void Engine::SelectBank(unsigned bank) {
    bool found = false;
    for (const auto &entry : layout.entries) {
        if (entry.id.type == ResourceType::MPB && entry.id.slot == bank && entry.payload) {
            found = true;
            break;
        }
    }
    if (!found) {
        throw std::runtime_error("This MPB slot is empty; load an MPB into it first.");
    }
    AllNotesOff();
    for (unsigned channel = 0; channel < midiChannels; ++channel) {
        Midi(0xb0 | channel, 32, static_cast<std::uint8_t>(bank));
    }
    Settle(commandSettleFrames);
    
    SelectProgram(0);
}

// swaps MPB into existing slot in AICA RAM
void Engine::ReplaceBank(unsigned bank, const Bytes &mpb) {
    LayoutEntry *entry = nullptr;
    for (auto &candidate : layout.entries) {
        if (candidate.id.type == ResourceType::MPB && candidate.id.slot == bank) {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
        throw std::runtime_error("No MPB slot exists with that bank number.");
    if (!isMpb(mpb))
        throw std::runtime_error("The replacement file is not a recognised MPB.");
    if (mpb.size() > entry->allocation.size)
        throw std::runtime_error("The replacement MPB is too large for this slot's allocation.");
    Bytes replacement = mpb;
    
    // stop voices before overwriting memory they may be reading
    for (unsigned channel = 0; channel < midiChannels; ++channel) {
        Midi(0xb0 | channel, 64, 0);
        Midi(0xb0 | channel, 120, 0);
    }
    Settle(commandSettleFrames);

    // upload and select new mpb
    Upload(entry->allocation.address, replacement.data(), static_cast<std::uint32_t>(replacement.size()));
    entry->payload = std::move(replacement);
    SelectBank(bank);
}
} // namespace manatee