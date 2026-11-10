#pragma once
#include "midi/MidiQueue.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#include <string>

namespace manatee {

// owns the MIDI apartment and every MIDI object
class MidiWorker {
    public:
        enum class Notice { Progress, Ready, Error, Finished };
        struct Failure {
            std::string stage;
            std::string message;
        };
        using Notify = std::function<void(Notice)>;
        using Trace = std::function<void(const std::string &)>;

        MidiWorker() = default;
        MidiWorker(const MidiWorker &) = delete;
        MidiWorker &operator=(const MidiWorker &) = delete;
        ~MidiWorker();
        
        bool Ready() const noexcept { return isReady.load(); }
        bool Failed() const noexcept { return hasFailed.load(); }
        const std::atomic<bool> &FailureFlag() const noexcept { return hasFailed; }
        bool Done() const noexcept { return isDone.load(); }
        bool Started() const noexcept { return thread.joinable(); }
        Failure GetFailure() const;
        std::string Stage() const;
        void RequestStop();
        bool JoinIfDone();
        void Start(MidiQueue &queue, Notify notify, Trace trace);

    private:
        std::thread thread;
        mutable std::mutex mutex;
        std::condition_variable wake;
        bool stopRequested = false;
        bool hasStarted = false;
        std::atomic<bool> isReady{false}, hasFailed{false}, isDone{false};
        std::string progress = "starting MIDI worker";
        Failure lastFailure;
};
} // namespace manatee