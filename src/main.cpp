#include "app/App.hpp"
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
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // normal launch has 1 argument (the exe name). headless launch has 4 args (flag, driver and bank)
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    
    // if can't get args, something weird happened
    if (!args)
        return 2;
    
    // create the app object, which owns MidiWorker and other state
    manatee::App app;
    
    // if no args, run the GUI version
    if (count == 1) {
        return app.Run(instance, show);
    }

    // use the parent console for errors unless stderr was redirected to a file or pipe
    const auto outputType = GetFileType(GetStdHandle(STD_ERROR_HANDLE));
    if (AttachConsole(ATTACH_PARENT_PROCESS) && outputType != FILE_TYPE_DISK &&
        outputType != FILE_TYPE_PIPE) {
        FILE *stream;
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
    // if not headless, or missing driver or bank, print usage and exit
    if (count != 4 || std::wstring_view(args[1]) != L"--headless" || !args[2][0] || !args[3][0]) {
        std::fprintf(stderr, "Usage: ManateeHost --headless manatee.drv bank.mpb|bundle.mlt\n");
        return 2;
    }
    // construct paths from the arguments
    std::filesystem::path driver(args[2]), bank(args[3]);

    consoleThread = GetCurrentThreadId();

    // create message queue so Ctrl+C can be handled
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);

    // register Ctrl+C handler
    if (!SetConsoleCtrlHandler(stopFromConsole, TRUE)) {
        std::fprintf(stderr, "Headless mode requires a terminal with Ctrl+C support.\n");
        return 2;
    }
    // run the app headless
    const auto result = app.Run(instance, show, driver, bank);
    // unregister handler Ctrl+C handler before exiting
    SetConsoleCtrlHandler(stopFromConsole, FALSE);
    return result;
}