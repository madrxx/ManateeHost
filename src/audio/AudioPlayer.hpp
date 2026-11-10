#pragma once
#include "audio/Engine.hpp"
#include "midi/MidiQueue.hpp"
#include "miniaudio.h"
#include <atomic>
#include <memory>

namespace manatee {
// owns the audio device and emulator - takes MIDI queue, gives events to engine, gives rendered samples to miniaudio
class AudioPlayer {
    public:
        explicit AudioPlayer(MidiQueue &queue);
        ~AudioPlayer();

        // miniaudio callback borrows this object's address so it mustn't move
        AudioPlayer(const AudioPlayer &) = delete;
        AudioPlayer &operator=(const AudioPlayer &) = delete;
        AudioPlayer(AudioPlayer &&) = delete;
        AudioPlayer &operator=(AudioPlayer &&) = delete;

        void Initialise(const std::atomic<bool> &midiFailed);
        void Start();
        void Stop() noexcept;
        void Shutdown() noexcept;
        bool Faulted() const noexcept { return fault.load(); }
        bool HasEngine() const noexcept { return engine != nullptr; }

        // these operations stop audio - caller decides when to restart playback
        void SetEngine(std::unique_ptr<Engine> replacement);
        void SelectBank(unsigned bank);
        void ReplaceBank(unsigned bank, const Bytes &mpb);
        void AllNotesOff();
        const std::vector<MltEntry> &Entries() const;
        unsigned ProgramCount(unsigned bank);

    private:
        static void AudioCallback(ma_device *device, void *output, const void *, ma_uint32 frames);
        void Render(void *output, ma_uint32 frames);

        MidiQueue &queue;
        const std::atomic<bool> *midiFailed = nullptr;
        std::unique_ptr<Engine> engine;
        ma_device device{};
        bool initialised = false;
        std::atomic<bool> fault{false};
};
} // namespace manatee