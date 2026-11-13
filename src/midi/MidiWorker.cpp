#include "midi/MidiWorker.hpp"
#include <libremidi/libremidi.hpp>
#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace manatee {
namespace {
libremidi::observer makeObserver() {
    return libremidi::observer{
        libremidi::observer_configuration{.track_virtual = true, .track_any = true},
        libremidi::winmm_observer_configuration{}
    };
}

std::string errorText(const stdx::error &error) {
    auto text = error.message();
    return {text.data(), text.size()};
}
} // namespace

std::vector<std::string> MidiWorker::Inputs() {
    auto observer = makeObserver();
    std::vector<std::string> names;
    for (const auto &port : observer.get_input_ports())
        names.push_back(port.display_name);
    return names;
}

MidiWorker::~MidiWorker() {
    if (thread.joinable())
        std::terminate();
}

MidiWorker::Failure MidiWorker::GetFailure() const {
    std::lock_guard lock(mutex);
    return lastFailure;
}

std::string MidiWorker::Stage() const {
    std::lock_guard lock(mutex);
    return progress;
}

void MidiWorker::RequestStop() {
    {
        std::lock_guard lock(mutex);
        stopRequested = true;
    }
    wake.notify_all();
}

bool MidiWorker::JoinIfDone() {
    if (!thread.joinable())
        return true;
    if (!Done())
        return false;
    thread.join();
    return true;
}

void MidiWorker::Start(MidiQueue &queue, std::string inputName, Notify notify, Trace trace) {
    if (hasStarted)
        throw std::logic_error("MIDI worker can only be started once.");
    hasStarted = true;

    // MIDI runs on own thread
    thread = std::thread([this, &queue, inputName = std::move(inputName), notify = std::move(notify), trace = std::move(trace)] {
        // callbacks may outlive thread function if cleanup fails so they need their own helper copies - can't use reference
        // catching exceptions to prevent interrupting MIDI setup
        
        // sends diagnostic text
        auto log = [trace](const std::string &text) noexcept {
            try {
                trace(text);
            } catch (...) {
            }
        };
        // sends a Notice
        auto notifyApp = [notify](Notice notice) noexcept {
            try {
                notify(notice);
            } catch (...) {
            }
        };
        // saves stage, logs, sends a progress notice
        auto setStage = [this, log, notifyApp](const std::string &text) {
            {
                std::lock_guard lock(mutex);
                progress = text;
            }
            log("Stage: " + text);
            notifyApp(Notice::Progress);
        };
        // saves, flags, logs, sends an Error notice
        auto reportFailure = [this, log, notifyApp](std::string_view text) noexcept {
            try {
                std::lock_guard lock(mutex);
                lastFailure = {progress, std::string(text)};
                hasFailed = true;
            } catch (...) {
                hasFailed = true;
            }
            wake.notify_all();
            try {
                log("MIDI input failure: " + std::string(text));
            } catch (...) {
            }
            notifyApp(Notice::Error);
        };
        
        std::unique_ptr<libremidi::midi_in> input;

        try {
            setStage("finding MIDI input: " + inputName);

            auto observer = makeObserver();
            auto ports = observer.get_input_ports();

            // does any port have a display name equal to input?
            const bool matchDisplayName = std::any_of(ports.begin(), ports.end(), [&](const auto &port) {
                return port.display_name == inputName;
            });

            auto matches = [&](const auto &port) {
                return (matchDisplayName ? port.display_name : port.port_name) == inputName;
            };
            
            // we need one match
            auto count = std::count_if(ports.begin(), ports.end(), matches);
            if (count == 0)
                throw std::runtime_error("MIDI input \"" + inputName + "\" not found. Start loopMIDI or set input to an available port with --port.");
            if (count > 1)
                throw std::runtime_error("More than one MIDI input port with this name. Set loopMIDI to a unique name and use --list-ports to check.");
            
            // just one, safe to deref and copy
            auto selected = *std::find_if(ports.begin(), ports.end(), matches);

            // callbacks
            libremidi::input_configuration config;
            config.on_message = [&queue, reportFailure](libremidi::message &&message) noexcept {
                try {
                    queue.PushMidi(message.bytes);
                } catch (...) {
                    reportFailure("MIDI packet reception failed.");
                }
            };
            config.on_error = [reportFailure](std::string_view text, const libremidi::source_location &) noexcept {
                reportFailure(text);
            };
            config.on_warning = [this, log](std::string_view text, const libremidi::source_location &) noexcept {
                inputWarning = true;
                try {
                    log("MIDI warning: " + std::string(text));
                } catch (...) {
                }
            };

            input = std::make_unique<libremidi::midi_in>(std::move(config), libremidi::winmm_input_configuration{});
            
            setStage("opening MIDI input: " + selected.display_name);
            const auto error = input->open_port(selected);
            if (error.is_set())
                throw std::runtime_error("Cannot open MIDI input: " + errorText(error));

            if (Failed())
                throw std::runtime_error("MIDI input failed while opening.");
            
            
            // woop woop
            // report success
            isReady = true;
            setStage("receiving MIDI from " + selected.display_name);
            notifyApp(Notice::Ready);

            // wait until stop requested or failure
            for (;;) {
                std::unique_lock lock(mutex);
                if (wake.wait_for(
                        lock,
                        std::chrono::milliseconds(500),
                        [this] { return stopRequested || hasFailed.load(); }))
                    break;

                // timeout - throw if selected port name no longer listed
                lock.unlock();
                auto current = observer.get_input_ports();
                if (std::none_of(current.begin(), current.end(), [&](const auto &port) { return port.display_name == selected.display_name; }))
                    throw std::runtime_error("MIDI input disconnected: " + selected.display_name);
            }
        } catch (const std::exception &error) {
            if (!Failed()) // not already reported
                reportFailure(error.what());
        } catch (...) {
            if (!Failed())
                reportFailure("Unknown exception in the MIDI worker.");
        }

        if (input) {
            log("Closing MIDI input");
            try {
                if (input->is_port_open()) {
                    const auto error = input->close_port();
                    if (error.is_set())
                        throw std::runtime_error("Cannot close MIDI input: " + errorText(error));
                } else if (inputWarning.load()) {
                    throw std::runtime_error("A warning occurred and the port isn't open.");
                }
                input.reset(); // destroy input
            } catch (const std::exception &error) {
                input.release(); // callbacks may still be alive - leaving input allocated till process exit
                reportFailure(error.what());
                return;
            } catch (...) {
                input.release();
                reportFailure("Cannot safely close MIDI input.");
                return;
            }
        }
        
        // complete - announce
        isDone = true;
        notifyApp(Notice::Finished);
    });
}
} // namespace manatee