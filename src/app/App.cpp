#include "app/App.hpp"
#include "support/binaryFile.hpp"
#include <winrt/base.h>
#include <memory>
#include <sstream>
#include <iomanip>
#include <cstdio>

namespace manatee {

// takes a WinRT error and returns a readable string with the HRESULT code and message
namespace {
std::string winrtError(const winrt::hresult_error &e) {
    std::ostringstream out;
    out << "HRESULT 0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
        << static_cast<std::uint32_t>(e.code().value) << ": " << winrt::to_string(e.message());
    return out.str();
}
} // namespace

int App::Run(HINSTANCE instance, int show, const std::filesystem::path &driver, const std::filesystem::path &bank) {
    headless = !driver.empty();

    // state needed for cleanup
    bool mainApartment = false;
    HANDLE singleInstance = nullptr;
    try {
        // single instance guard
        singleInstance = CreateMutexW(nullptr, FALSE, L"Local\\ManateeHost.SingleInstance");
        auto mutexError = GetLastError();
        if (!singleInstance) {
            throw std::runtime_error("Cannot create mutex (" + std::to_string(mutexError) + ").");
        }
        if (mutexError == ERROR_ALREADY_EXISTS) {
            if (headless) {
                throw std::runtime_error("ManateeHost is already running.");
            }
            MessageBoxW(nullptr,
                        L"ManateeHost is already running. Please force close it if it is stuck.",
                        L"ManateeHost already running", MB_OK | MB_ICONINFORMATION);
            CloseHandle(singleInstance);
            // it's a GUI app and we told the user, so returning 0
            return 0;
        }

        diagnostics.Open();

        diagnostics.Stage("initialising winrt");
        // prepare main thread to use WinRT APIs
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        mainApartment = true;

        if (headless) { // save driver path to class, open bank
            diagnostics.Stage("loading headless source");
            driverPath = driver;
            source.OpenFile(bank);
        } else { // GUI mode, user can choose driver and bank later
            window.Create(instance, show, [this](HWND hwnd, UINT message, WPARAM w, LPARAM l) {
                return HandleMessage(hwnd, message, w, l);
            });
            RefreshSourceUi();
            diagnostics.Stage("main window displayed");
        }

        // prepare main thread message queue for MIDI worker
        MSG msg{};
        PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);

        // initialise audio
        diagnostics.Stage("opening the default audio output");
        player.Initialise(midiWorker.FailureFlag());
        
        StartMidiWorker();

        // periodic status check
        const auto timer = SetTimer(window.Handle(), 1, 500, nullptr);
        
        // main loop
        BOOL messageResult;
        while ((messageResult = GetMessageW(&msg, nullptr, 0, 0)) > 0) { // continues until WM_QUIT (0) or error (-1)
            if (headless)
                HandleMessage(nullptr, msg.message, msg.wParam, msg.lParam);
            else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        // cleanup
        KillTimer(window.Handle(), timer);

        // if GetMessage failed, terminate the process to avoid freeing a potentially live MIDI worker
        if (messageResult == -1) {
            diagnostics.Log(
                "GetMessage failed; terminating");
            TerminateProcess(GetCurrentProcess(), 1);
        }
    } catch (const winrt::hresult_error &e) { // WinRT error
        StartupError(winrtError(e));
        exitCode = 1;
    } catch (const std::exception &e) { // standard C++ error
        StartupError(e.what());
        exitCode = 1;
    } catch (...) { // catch-all
        StartupError("Unknown exception.");
        exitCode = 1;
    }

    // stop audio, request MIDI shutdown
    player.Stop();
    midiWorker.RequestStop();
    if (!midiWorker.JoinIfDone()) { // not a graceful shutdown - live MIDI worker
        diagnostics.Log("exited message loop with a live MIDI worker; terminating");
        TerminateProcess(GetCurrentProcess(), 1);
        std::terminate();
    }
    // release resources
    player.Shutdown();
    window.Destroy();
    if (mainApartment)
        winrt::uninit_apartment();
    if (singleInstance)
        CloseHandle(singleInstance);
    return exitCode;
}

#pragma region Lifecycle

void App::StartMidiWorker() {
    const auto target = window.Handle();
    const auto thread = GetCurrentThreadId();
    midiWorker.Start(
        queue,
        [target, thread](MidiWorker::Notice notice) { // notification callback
            const auto message = notice == MidiWorker::Notice::Error   ? errorMessage
                               : notice == MidiWorker::Notice::Ready   ? readyMessage
                                                                       : stageMessage;
            if (target)
                PostMessageW(target, message, 0, 0);
            else
                PostThreadMessageW(thread, message, 0, 0);
        },
        [this](const std::string &text) { // logging callback
            diagnostics.Log(text);
        }
    );
}

void App::FinishClose() {
    if (headless)
        PostQuitMessage(exitCode);
    else
        window.Destroy();
}

#pragma endregion

#pragma region Message and Control

LRESULT App::HandleMessage(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == stageMessage) {
        if (closing) {
            auto text = "Closing: " + midiWorker.Stage() + (headless ? ". Ctrl+C again to force exit." : ". Close again to force exit.");
            std::wstring wide(text.begin(), text.end());
            Status(wide.c_str());
            if (midiWorker.Done())
                FinishClose();
        } else if (headless) {
            std::fprintf(stderr, "%s\n", midiWorker.Stage().c_str());
            return 0;
        } else if (midiWorker.Failed()) {
            Status(L"MIDI startup or reception failed. See the startup log.");
        } else if (midiWorker.Ready()) {
            Status(L"MIDI ready. Load manatee.drv, then MPBs or an MLT.");
        } else {
            auto text = "Starting: " + midiWorker.Stage();
            std::wstring wide(text.begin(), text.end());
            Status(wide.c_str());
        }
        RefreshSourceUi();
        return 0;
    }
    if (message == readyMessage) {
        if (headless && !closing) {
            try {
                LoadSource();
                Status(L"Ctrl+C to stop. Ctrl+C again to force exit if cleanup hangs.");
            } catch (const std::exception &error) {
                StartupError(error.what(), "loading driver and banks");
                exitCode = 1;
                HandleMessage(nullptr, WM_CLOSE, 0, 0);
            }
        } else if (!closing) {
            RefreshSourceUi();
            Status(L"MIDI ready. Load manatee.drv, then MPBs or an MLT.");
        }
        return 0;
    }
    if (message == errorMessage) {
        const auto failure = midiWorker.GetFailure();
        if (!closing) {
            Status(L"MIDI startup failed. See the startup log.");
            StartupError(failure.message, failure.stage);
            if (headless) {
                exitCode = 1;
                HandleMessage(nullptr, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        if (closing && !midiWorker.Done()) {
            // blocked SDK call - offer force exit as a last resort
            if (headless ||
                MessageBoxW(hwnd,
                            L"Windows MIDI Services has not returned. Force exit ManateeHost?",
                            L"Force exit", MB_YESNO | MB_ICONWARNING) == IDYES)
                TerminateProcess(GetCurrentProcess(), 1);
            return 0;
        }
        closing = true;
        player.Stop();
        midiWorker.RequestStop();
        if (midiWorker.Done() || !midiWorker.Started())
            FinishClose();
        else
            Status(headless
                       ? L"Closing: waiting for Windows MIDI Services. Ctrl+C again to force exit."
                       : L"Closing: waiting for Windows MIDI Services. Close again to force exit.");
        return 0;
    }
    if (message == WM_COMMAND && !headless) {
        try {
            Control(LOWORD(w), reinterpret_cast<HWND>(l), HIWORD(w));
        } catch (const std::exception &e) {
            player.Stop();
            Status(L"Stopped. Fix the selected files, then Reload.");
            MessageBoxA(window.Handle(), e.what(), "ManateeHost", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    if (message == WM_TIMER) {
        if (closing)
            return 0;
        if (!midiWorker.Ready())
            return 0;
        if (midiWorker.Failed()) {
            RefreshSourceUi();
            Status(L"MIDI connection failed. Close and reopen the app after checking Windows MIDI Services is running.");
        } else if (player.Faulted())
            Status(L"Stopped: emulator error or MIDI queue overflow. Use Reload.");
        if (headless && (midiWorker.Failed() || player.Faulted())) {
            exitCode = 1;
            HandleMessage(nullptr, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, w, l);
}

void App::Control(int id, HWND item, int event) {
    if (!midiWorker.Ready() || midiWorker.Failed() || closing)
        return;
    switch (id) {
    case Driver: {
        auto path = window.Choose(L"manatee.drv (*.drv)\0*.drv\0All files\0*.*\0");
        if (path.empty())
            return;
        driverPath = path;
        LoadSource();
        break;
    }
    case OpenMpbs: {
        auto paths = window.ChooseMpbs();
        if (paths.empty())
            return;
        source.OpenMpbs(std::move(paths));
        LoadSource();
        break;
    }
    case OpenMlt: {
        auto path = window.Choose(L"Multi-Unit (*.mlt)\0*.mlt\0All files\0*.*\0");
        if (path.empty())
            return;
        source.OpenMlt(path);
        LoadSource();
        break;
    }
    case ActiveBank:
        if (event == CBN_SELCHANGE && window.IsBankControl(item) && player.HasEngine()) {
            player.SelectBank(window.SelectedSlot());
            StartAudio();
        }
        break;
    case ReplaceMpb:
        if (player.HasEngine())
            ReplaceSelectedMpb();
        break;
    case AllNotesOff:
        if (player.HasEngine()) {
            player.AllNotesOff();
            StartAudio();
        }
        break;
    case Reload:
        LoadSource(player.HasEngine() && window.HasSlots() ? window.SelectedSlot() : 16);
        break;
    case OpenFpb:
    case OpenFob: {
        auto path =
            window.Choose(id == OpenFpb ? L"FX Program Bank (*.fpb)\0*.fpb\0All files\0*.*\0"
                                        : L"FX Output Bank (*.fob)\0*.fob\0All files\0*.*\0");
        if (path.empty())
            return;
        if (id == OpenFpb)
            source.SetFpb(path);
        else
            source.SetFob(path);
        LoadSource(player.HasEngine() && window.HasSlots() ? window.SelectedSlot() : 16);
        break;
    }
    case ClearEffects:
        source.ClearEffects();
        LoadSource(player.HasEngine() && window.HasSlots() ? window.SelectedSlot() : 16);
        break;
    }
}

#pragma endregion

#pragma region Loading and Playback

void App::LoadSource(unsigned preferredSlot) {
    RefreshSourceUi();
    if (driverPath.empty() || source.Empty()) {
        Status(L"Choose a driver and either individual MPBs or one MLT.");
        return;
    }
    // clear the old player before loading
    Status(L"Loading driver and banks...");
    player.SetEngine(nullptr);
    if (!headless)
        window.UpdateBanks(player.Entries(), source);
    RefreshSourceUi();
    auto replacement = std::make_unique<Engine>(readFile(driverPath), source.MakeLayout());
    unsigned selected = 16;
    for (const auto &entry : replacement->Entries()) {
        if (entry.id.type != ResourceType::MPB || !entry.payload)
            continue;
        if (selected == 16 || entry.id.slot == preferredSlot)
            selected = entry.id.slot;
    }
    if (selected < 16)
        replacement->SelectBank(selected);
    player.SetEngine(std::move(replacement));
    if (!headless)
        window.UpdateBanks(player.Entries(), source);
    RefreshSourceUi();
    if (selected < 16) {
        ShowSelectedSlot(selected);
        StartAudio();
    } else {
        if (headless)
            throw std::runtime_error("The layout has no loaded MPB to play.");
        if (window.HasSlots())
            ShowSelectedSlot(window.FirstSlot());
        Status(L"MLT has no loaded MPB. Select an empty slot and Replace MPB.");
    }
}

void App::ReplaceSelectedMpb() {
    auto path = window.Choose(L"MIDI Program Bank (*.mpb)\0*.mpb\0All files\0*.*\0");
    if (path.empty())
        return;
    const auto slot = window.SelectedSlot();
    if (source.IsLoose()) {
        source.ReplaceMpb(slot, path);
        LoadSource(slot);
    } else {
        auto data = readFile(path);
        player.ReplaceBank(slot, data);
        if (!headless)
            window.UpdateBanks(player.Entries(), source);
        ShowSelectedSlot(slot);
        StartAudio();
    }
}

void App::StartAudio() {
    if (!player.HasEngine())
        return;
    player.Start();
    Status(L"Running");
}

#pragma endregion

#pragma region UI Feedback

void App::RefreshSourceUi() {
    if (headless)
        return;
    const bool ready = midiWorker.Ready() && !midiWorker.Failed() && !closing;
    window.RefreshSource(source, ready, player.HasEngine(), !driverPath.empty());
}

void App::ShowSelectedSlot(unsigned slot) {
    if (!headless)
        window.ShowSelectedSlot(slot);
    diagnostics.Log("Selected MPB slot " + std::to_string(slot) +
                    ", programs: " + std::to_string(player.ProgramCount(slot)));
}

void App::Status(const wchar_t *text) {
    if (headless)
        std::fprintf(stderr, "%s\n", winrt::to_string(text).c_str());
    else
        window.Status(text);
}

void App::StartupError(const std::string &message, const std::string &savedStage) {
    auto failedStage = savedStage.empty() ? diagnostics.CurrentStage() : savedStage;
    auto text = "Startup failed while " + failedStage + ".\n\n" + message;
    if (failedStage.find("MIDI") != std::string::npos ||
        failedStage.find("virtual") != std::string::npos)
        text += "\n\nManateeHost requires Windows 11 25H2 or later with MIDI Services enabled. Keep the generated .dll and .pri beside the .exe.";
    diagnostics.Log(text);
    text += "\n\nDetails: %TEMP%\\ManateeHost-startup.log";
    if (headless)
        std::fprintf(stderr, "%s\n", text.c_str());
    else
        MessageBoxA(window.Handle(), text.c_str(), "ManateeHost", MB_OK | MB_ICONERROR);
}

#pragma endregion

} // namespace manatee