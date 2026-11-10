// Adapted from Microsoft MIDI main_simple_app_to_app sample.
// MIT licence: vendor/MICROSOFT-MIDI-LICENSE (project root).

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <winrt/Windows.Devices.Midi2.h>
#include <winrt/Windows.Devices.Midi2.Enumeration.h>
#include <winrt/Windows.Devices.Midi2.Transports.Virtual.h>

#include "midi/MidiWorker.hpp"

#include <memory>
#include <utility>
#include <functional>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace manatee {
namespace {
namespace wm = winrt::Windows::Devices::Midi2;  // Core SDK
namespace we = wm::Enumeration;                 // Metadata, including function blocks, are here
namespace wv = wm::Transports::Virtual;         // For creating the app-to-app midi device

class MidiPort {
    wm::MidiSession session{nullptr};
    wm::MidiEndpointConnection endpoint{nullptr};
    wv::MidiVirtualDevice device{nullptr};
    winrt::event_token token{};

    public:
        MidiPort() = default;
        MidiPort(const MidiPort &) = delete;
        MidiPort &operator=(const MidiPort &) = delete;

        void Open(
                MidiQueue &queue,
                const std::function<void(const char *)> &stage,
                std::function<void()> failed,
                const std::function<void(const std::string &)> &trace = {}) {
            
            stage("checking Windows MIDI Services");
            if (!wm::MidiApi::EnsureServiceAvailable())
                throw std::runtime_error("Windows MIDI Services could not be started - you need Win11 25H2 or later with MIDI Services enabled.");
            
            stage("creating a Windows MIDI Services session");
            session = wm::MidiSession::Create(L"ManateeHost");
            if (!session)
                throw std::runtime_error("MIDI session creation returned no session.");
            
            stage("constructing virtual MIDI endpoint metadata");
            we::MidiDeclaredEndpointInfo info;
            info.Name(L"ManateeHost");
            info.ProductInstanceId(L"ManateeHost");
            info.HasStaticFunctionBlocks(true);
            info.SupportsMidi10Protocol(true);
            info.SupportsMidi20Protocol(true);
            info.SupportsReceivingJitterReductionTimestamps(false);
            info.SupportsSendingJitterReductionTimestamps(false);
            // UMP spec version
            info.SpecificationVersionMajor(1);
            info.SpecificationVersionMinor(1);

            stage("constructing virtual MIDI creation config");
            wv::MidiVirtualDeviceCreationConfig config(L"ManateeHost", L"ManateeHost virtual instrument", L"ManateeHost", info);
            config.CreateOnlyUmpEndpoints(false); // requesting compatibility for older applications

            stage("constructing the MIDI function block");
            we::MidiFunctionBlock block;
            block.Number(0);
            block.IsActive(true);
            block.Name(L"Synth");
            block.FirstGroup(wm::MidiGroup(static_cast<std::uint8_t>(0)));
            block.GroupCount(1);
            block.Direction(we::MidiFunctionBlockDirection::BlockInput);
            block.RepresentsMidi10Connection(we::MidiFunctionBlockRepresentsMidi10Connection::Not10);
            config.FunctionBlocks().Append(block);

            if (trace) {
                trace("Virtual MIDI ProductInstanceId: " + winrt::to_string(info.ProductInstanceId()));
                trace("Virtual MIDI AssociationId: " + winrt::to_string(winrt::to_hstring(config.AssociationId())));
            }

            stage("calling Windows CreateVirtualDevice");
            device = wv::MidiVirtualDeviceManager::CreateVirtualDevice(config);
            if (!device)
                throw std::runtime_error(
                    "CreateVirtualDevice returned no device.");
            stage("Windows CreateVirtualDevice returned");
            if (trace)
                trace("Device endpoint ID: " + winrt::to_string(device.DeviceEndpointDeviceId()));
            
            stage("connecting to the virtual MIDI device");
            endpoint = session.CreateEndpointConnection(device.DeviceEndpointDeviceId());
            if (!endpoint)
                throw std::runtime_error("CreateEndpointConnection returned no connection.");
            if (trace)
                trace("Connection ID: " + winrt::to_string(winrt::to_hstring(endpoint.ConnectionId())));
            
            stage("registering the MIDI receive callback");
            // called when MIDI arrives
            token = endpoint.MessageReceived(
                [&queue, failed = std::move(failed)](const auto &, const wm::MidiMessageReceivedEventArgs &args) {
                    try {
                        const auto first = args.PeekFirstWord();
                        if (((first >> 24) & 15) != 0 || // if group isn't 0
                            ((first >> 28) != 2 && (first >> 28) != 4)) // or if type isn't 2 (MIDI 1.0) or 4 (MIDI 2.0)
                            return;

                        // copy packet words to a local array
                        auto words = args.GetMessagePacket().GetAllWords();
                        std::uint32_t storage[4]{};
                        auto count = words.GetMany(0, winrt::array_view<std::uint32_t>{storage});
                        
                        // push to queue
                        queue.PushUmp({storage, count});
                    } catch (...) {
                        failed();
                    }
                });
            
            stage("attaching the virtual MIDI device to its connection");
            if (endpoint.AddMessageProcessingPlugin(device) !=
                wm::MidiMessageProcessingPluginAddResult::Succeeded)
                throw std::runtime_error("Could not attach virtual MIDI device to its connection.");
            
            stage("opening virtual MIDI device connection");
            if (!endpoint.Open())
                throw std::runtime_error("Virtual MIDI endpoint Open() failed.");
        }

        void Close(const std::function<void(const char *)> &report = {}) noexcept {
            // report one line safely
            auto reportStage = [&](const char *text) {
                try {
                    if (report)
                        report(text);
                } catch (...) {
                }
            };
            
            // run and report one cleanup op
            auto runCleanup = [&](const char *name, auto &&operation) {
                reportStage(name);
                try {
                    operation();
                    reportStage((std::string(name) + " returned").c_str());
                } catch (const winrt::hresult_error &error) {
                    try {
                        std::ostringstream message;
                        message << name << " failed: HRESULT 0x"
                                << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
                                << static_cast<std::uint32_t>(error.code().value) << ": "
                                << winrt::to_string(error.message());
                        reportStage(message.str().c_str());
                    } catch (...) {
                        reportStage("MIDI cleanup: could not format WinRT error");
                    }
                } catch (const std::exception &error) {
                    reportStage(error.what());
                } catch (...) {
                    reportStage("MIDI cleanup: unknown exception");
                }
            };

            // cleanup based on order from Microsoft sample
            if (endpoint && token.value) {
                runCleanup("MIDI cleanup: removing MessageReceived callback", [&] {
                    endpoint.MessageReceived(token);
                    token = {};
                });
            }
            if (endpoint && device) {
                runCleanup("MIDI cleanup: RemoveMessageProcessingPlugin", [&] {
                    endpoint.RemoveMessageProcessingPlugin(device.as<wm::IMidiEndpointMessageProcessingPlugin>().PluginId());
                });
            }
            if (session && endpoint) {
                runCleanup("MIDI cleanup: DisconnectEndpointConnection", [&] { session.DisconnectEndpointConnection(endpoint.ConnectionId()); });
            }
            if (session) {
                runCleanup("MIDI cleanup: session.Close", [&] { session.Close(); });
            }
            reportStage("MIDI cleanup: releasing endpoint");
            endpoint = nullptr;
            reportStage("MIDI cleanup: releasing virtual device");
            device = nullptr;
            reportStage("MIDI cleanup: releasing session");
            session = nullptr;

            reportStage("MIDI cleanup: completed");
        }
        ~MidiPort() { Close(); }
};
} // namespace

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

void MidiWorker::Start(MidiQueue &queue, Notify notify, Trace trace) {
    if (hasStarted)
        throw std::logic_error("MIDI worker can only be started once.");
    hasStarted = true;
    // MIDI runs on own thread
    thread = std::thread([this, &queue, notify = std::move(notify), trace = std::move(trace)] {
        // sends diagnostic text - exceptions caught to prevent interrupting MIDI setup
        auto log = [&](const std::string &text) noexcept {
            try {
                trace(text);
            } catch (...) {
            }
        };
        // sends a Notice
        auto notifyApp = [&](Notice notice) noexcept {
            try {
                notify(notice);
            } catch (...) {
            }
        };
        auto setStage = [&](const char *text) {
            {
                std::lock_guard lock(mutex);
                progress = text;
            }
            log("Stage: " + std::string(text));
            notifyApp(Notice::Progress);
        };
        auto reportFailure = [&](std::string text) {
            {
                std::lock_guard lock(mutex);
                lastFailure = {progress, std::move(text)};
            }
            hasFailed = true;
            auto saved = GetFailure();
            log("MIDI startup failure at " + saved.stage + ": " + saved.message);
            notifyApp(Notice::Error);
        };

        bool apartment = false; // tracks if apartment gets initialised successfully
        std::unique_ptr<MidiPort> port; // owns Windows MIDI device

        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartment = true;

            // create and open MIDI port
            port = std::make_unique<MidiPort>();
            port->Open(
                queue, setStage,
                [this, trace] {
                    hasFailed = true;
                    try {
                        trace("MIDI packet reception failed.");
                    } catch (...) {
                    }
                },
                log);
            
            // report success
            isReady = true;
            setStage("running");
            notifyApp(Notice::Ready);
            
            // wait until stop requested
            std::unique_lock lock(mutex);
            wake.wait(lock, [this] { return stopRequested; });
        } catch (const winrt::hresult_error &error) {
            std::ostringstream text;
            text << "HRESULT 0x";
            text << std::hex << std::uppercase << std::setfill('0');
            text << std::setw(8) << static_cast<std::uint32_t>(error.code().value);
            text << ": " << winrt::to_string(error.message());
            reportFailure(text.str());
        } catch (const std::exception &error) {
            reportFailure(error.what());
        } catch (...) {
            reportFailure("Unknown exception in the MIDI worker.");
        }

        log("MIDI worker: releasing session and endpoint");
        if (port)
            port->Close(setStage);
        port.reset();
        
        if (apartment)
            winrt::uninit_apartment();
        
        // complete
        isDone = true;
        notifyApp(Notice::Finished);
    });
}
} // namespace manatee