#include "audio/AudioPlayer.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace manatee {
AudioPlayer::AudioPlayer(MidiQueue &queue) : queue(queue) {}
AudioPlayer::~AudioPlayer() { Shutdown(); }

void AudioPlayer::Initialise(const std::atomic<bool> &failureFlag) {
    if (initialised)
        throw std::logic_error("Audio output is already initialised.");
    
    // store pointer to midi failure flag
    midiFailed = &failureFlag;

    // default playback device
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = 2;
    config.sampleRate = 44100;
    config.periodSizeInFrames = 256;

    // set audio source for ma
    config.dataCallback = AudioCallback;
    config.pUserData = this;

    if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS)
        throw std::runtime_error("Can't open default audio output.");
    initialised = true;
}

// start audio output and start accepting midi events
void AudioPlayer::Start() {
    if (!engine || !initialised)
        return;
    
    fault = false;
    if (ma_device_start(&device) != MA_SUCCESS)
        throw std::runtime_error("Can't start default audio output.");
    
    queue.accepting = true;
}

// stop accepting new midi events, stop audio, wait for callbacks, clear queue
void AudioPlayer::Stop() noexcept {
    queue.accepting = false;
    
    if (initialised)
        ma_device_stop(&device);
    
    queue.Clear();
}

void AudioPlayer::Shutdown() noexcept {
    Stop();
    if (initialised) {
        ma_device_uninit(&device);
        initialised = false;
    }
    midiFailed = nullptr;
}

void AudioPlayer::SetEngine(std::unique_ptr<Engine> replacement) {
    Stop();
    engine = std::move(replacement);
}

void AudioPlayer::SelectBank(unsigned bank) {
    Stop();
    if (engine)
        engine->SelectBank(bank);
}

void AudioPlayer::ReplaceBank(unsigned bank, const Bytes &mpb) {
    Stop();
    if (engine)
        engine->ReplaceBank(bank, mpb);
}

void AudioPlayer::AllNotesOff() {
    Stop();
    if (engine)
        engine->AllNotesOff();
}

const std::vector<MltEntry> &AudioPlayer::Entries() const {
    static const std::vector<MltEntry> empty;
    return engine ? engine->Entries() : empty;
}

unsigned AudioPlayer::ProgramCount(unsigned bank) {
    Stop();
    return engine ? engine->ProgramCount(bank) : 0;
}

void AudioPlayer::AudioCallback(ma_device *device, void *output, const void *, ma_uint32 frames) {
    static_cast<AudioPlayer *>(device->pUserData)->Render(output, frames);
}

void AudioPlayer::Render(
    void *output, // miniaudio buffer to fill
    ma_uint32 frames) { // number of stereo frames requested
    
    auto *out = static_cast<std::int16_t *>(output);

    // fill with silence in case of early return or in case we only render part of the buffer
    std::fill_n(out, frames * 2, 0);

    if (!engine || fault.load() || midiFailed->load())
        return;
    
    while (frames) {
        // get a 64 frame max chunk of frames
        auto count = std::min<unsigned>(frames, 64);

        // fault if there's midi queue overflow
        if (queue.overflow.load()) {
            fault = true;
            return;
        }

        Event event;
        // pop events from the midi queue and feed them to the engine
        // loop ends when either the queue is empty or 32 events sent
        for (unsigned i = 0; i < 32 && queue.Pop(event); ++i)
            engine->InputMidi(event.status, event.data1, event.data2);
        
        // engine renders chunk of frames to current position in buffer
        if (!engine->Render(out, count)) {
            fault = true;
            std::fill_n(out, frames * 2, 0);
            return;
        }

        out += count * 2;
        frames -= count;
    }
}
} // namespace manatee