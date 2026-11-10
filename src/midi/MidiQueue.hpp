#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <libremidi/cmidi2.hpp>
namespace manatee {
// one MIDI 1.0 channel message
struct Event {
    std::uint8_t status{}, data1{}, data2{};
};

class MidiQueue {
    static constexpr unsigned capacity = 2048; // number of positions in queue + 1
    std::array<Event, capacity> events{}; // storage for queue
    std::atomic<unsigned> write{}, read{}; // atomic uint - midi thread advances write, audio thread advances read
    std::mutex producer; // producers have to take turns

    public:
        std::atomic<bool> accepting{false}; // controls whether new events should be added or discarded
        std::atomic<bool> overflow{false}; // set when an event is dropped due to a full queue

        // takes UMP (MIDI 1 or 2) packets and converts them to MIDI 1 bytes for our queue
        void PushUmp(std::span<const std::uint32_t> words) {
            if (words.empty())
                return;
            
            // read message type from top 4 bits
            const auto type = words[0] >> 28;

            // we only want group 0 and we only want type 2 (MIDI 1.0 channel msg) or type 4 (MIDI 2.0 channel msg)
            if (((words[0] >> 24) & 15) != 0 || (type != 2 && type != 4))
                return;
            
            // should be 1 32-bit UMP word if type 2, or 2 32-bit UMP words if type 4
            if (words.size() != (type == 2 ? 1u : 2u))
                return;
            
            // prep for conversion
            std::uint8_t bytes[32]{}; // converted MIDI 1 buffer
            cmidi2_midi_conversion_context context;
            cmidi2_midi_conversion_context_initialize(&context);

            // output buffer for cmidi2
            context.midi1 = bytes;
            context.midi1_num_bytes = sizeof(bytes);

            // input UMP packet for cmidi2 (which doesn't actually mutate this const)
            context.ump = const_cast<std::uint32_t *>(words.data());
            context.ump_num_bytes = words.size_bytes();
            context.skip_delta_time = true; // this isn't a MIDI file, it's a realtime stream, so we don't need delta-time info

            // convert, or discard packet if cmidi2 reports a conversion error
            if (cmidi2_convert_ump_to_midi1(&context) != CMIDI2_CONVERSION_RESULT_OK)
                return;
            
            // advance through bytes - looping because 1 UMP message can convert to multiple MIDI 1.0 messages
            for (std::size_t i = 0; i < context.midi1_proceeded_bytes;) {
                auto status = bytes[i];
                auto kind = status & 0xf0;
                auto size = (kind == 0xc0 || kind == 0xd0) ? 2u : 3u;
                
                // stop if
                if (status < 0x80 || status >= 0xf0 || // stop if status isn't a channel message
                    i + size > context.midi1_proceeded_bytes) // or if there's not enough output bytes for the whole message
                    return;
                Push({
                    status,
                    bytes[i + 1], // data 1
                    static_cast<std::uint8_t>(size == 3 ? bytes[i + 2] : 0)}); // data 2 (or 0 if N/A)
                i += size;
            }
        }

        void Push(Event e) {
            std::lock_guard lock(producer); // only one producer may update the queue at a time

            if (!accepting.load())
                return;

            auto w = write.load(std::memory_order_relaxed); // where we intend to put the event
            auto next = (w + 1) % capacity; // position write will move to afterwards

            // check for space or overflow if there's none
            if (next == read.load(std::memory_order_acquire)) {
                overflow = true;
                return;
            }

            // publish the index only after the payload is fully written
            events[w] = e;
            write.store(next, std::memory_order_release); // creating a link between !!this!! and
        }

        bool Pop(Event &e) {
            auto r = read.load(std::memory_order_relaxed);
            if (r == write.load(std::memory_order_acquire)) // !!this!! - so that we can safely access the event array across threads
                return false;
            e = events[r];

            // finished with this slot, advancing read and using memory order to ensure safe reuse
            read.store((r + 1) % capacity, std::memory_order_release);
            return true;
        }

        // reset queue so it's empty and ready to use again
        void Clear() {
            std::lock_guard lock(producer);
            read = 0;
            write = 0;
            overflow = false;
        }
};
} // namespace manatee