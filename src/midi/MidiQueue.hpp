#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
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

        // takes one MIDI 1.0 channel message and adds it to queue
        void PushMidi(std::span<const std::uint8_t> bytes) {
            // reject empty
            if (bytes.empty())
                return;
            
            // get message kind from upper 4 bits
            auto kind = bytes[0] & 0xf0;

            // expected length
            auto size = (kind == 0xc0 || kind == 0xd0) ? 2u : 3u;

            // reject if
            if (bytes.size() != size || // wrong size
                bytes[0] < 0x80 || bytes[0] >= 0xf0 || // status isn't a channel message
                bytes[1] > 127 || // out of range
                (size == 3 && bytes[2] > 127)) // out of range
                return;
                
            Push({bytes[0], bytes[1], static_cast<std::uint8_t>(size == 3 ? bytes[2] : 0)});
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