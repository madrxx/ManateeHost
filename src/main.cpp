#include "app/App.hpp"
#include "support/windowsEncoding.hpp"
#include <shellapi.h>
#include <cstdio>
#include <filesystem>
#include <string_view>

namespace {
DWORD consoleThread;

// forward Ctrl+C and Ctrl+Break to the main thread so it can close the app
BOOL WINAPI stopFromConsole(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    return PostThreadMessageW(consoleThread, WM_CLOSE, 0, 0);
}

void usage() {
    std::fprintf(stderr,
        "Usage: ManateeHost --headless manatee.drv bundle.mlt|bank.mpb (start without GUI)\n"
        "       ManateeHost --port \"port name\" (override MIDI input port)\n"
        "       ManateeHost --list-ports (list available ports)\n"
        "Default MIDI input: ManateeHost (create in loopMIDI).\n");
}

// UTF-16 for console, UTF-8 for file/pipe
void printInput(const std::string &name) {
    auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;

    if (GetConsoleMode(handle, &mode)) {
        auto text = manatee::fromUtf8(name + "\n");
        DWORD written;
        if (!WriteConsoleW(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
            throw std::runtime_error("Can't write MIDI input list.");
    } else {
        std::printf("%s\n", name.c_str());
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args)
        return 2;

    // attach to parent console, preserve stdout/stderr redirect
    if (count > 1) {
        const auto outputType = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
        const auto stdoutType = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE *stream;
            if (outputType != FILE_TYPE_DISK && outputType != FILE_TYPE_PIPE)
                freopen_s(&stream, "CONOUT$", "w", stderr);
            if (stdoutType != FILE_TYPE_DISK && stdoutType != FILE_TYPE_PIPE)
                freopen_s(&stream, "CONOUT$", "w", stdout);
        }
    }


    std::filesystem::path driver, bank;
    std::wstring inputName = L"ManateeHost";
    
    bool valid = true, headless = false, inputSpecified = false, listInputs = false, help = false;

    for (int i = 1; i < count && valid; ++i) {
        const std::wstring_view arg(args[i]);
        if (arg == L"--port" && // next param is port name
                !inputSpecified &&
                i + 1 < count &&
                args[i + 1][0]) {
            inputName = args[++i];
            inputSpecified = true;
        } else if (arg == L"--headless" && // next two params are driver and bank
                !headless &&
                i + 2 < count &&
                args[i + 1][0] &&
                args[i + 2][0]) {
            driver = args[++i];
            bank = args[++i];
            headless = true;
        } else if (arg == L"--list-ports" && count == 2) {
            listInputs = true;
        } else if (arg == L"--help" && count == 2) {
            help = true;
        } else {
            valid = false;
        }
    }
    LocalFree(args);

    if (help) {
        usage();
        return 0;
    }
    if (!valid) {
        usage();
        return 2;
    }

    try {
        if (listInputs) {
            const auto names = manatee::MidiWorker::Inputs();
            for (const auto &name : names)
                printInput(name);
            if (names.empty())
                std::fprintf(stderr, "No MIDI inputs found. Start loopMIDI and create a port.\n");
            return 0;
        }

        // owns midi worker/queue, bank source and window
        manatee::App app;

        const auto input = manatee::toUtf8(inputName);
        if (!headless)
            return app.Run(instance, show, {}, {}, input); // run GUI app

        consoleThread = GetCurrentThreadId();
        MSG message{};
        PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);

        // register Ctrl+C handler
        if (!SetConsoleCtrlHandler(stopFromConsole, TRUE)) {
            std::fprintf(stderr, "Headless mode requires a terminal with Ctrl+C support.\n");
            return 2;
        }

        const auto result = app.Run(instance, show, driver, bank, input); // run headless app

        // unregister Ctrl+C handler before exiting
        SetConsoleCtrlHandler(stopFromConsole, FALSE);
        return result;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}