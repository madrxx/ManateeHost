#pragma once
#include "ui/MainWindow.hpp"
#include "app/Diagnostics.hpp"
#include "audio/AudioPlayer.hpp"
#include "midi/MidiWorker.hpp"

namespace manatee {
class App {
    public:
        int Run(HINSTANCE instance, int show, const std::filesystem::path &driver = {},
            const std::filesystem::path &bank = {});

    private:
        // lifecycle
        void StartMidiWorker();
        void FinishClose();
        
        // message and control
        LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM w, LPARAM l);
        void Control(int id, HWND item, int event);

        // loading and playback
        void LoadSource(unsigned preferredSlot = 16);
        void ReplaceSelectedMpb();
        void StartAudio();

        // UI feedback
        void RefreshSourceUi();
        void ShowSelectedSlot(unsigned slot);
        void Status(const wchar_t *text);
        void StartupError(const std::string &message, const std::string &savedStage = {});

        
        Diagnostics diagnostics;
        MidiQueue queue;
        AudioPlayer player{queue};
        MainWindow window;
        BankSource source;
        std::filesystem::path driverPath;
        bool closing = false;
        bool headless = false;
        int exitCode = 0;
        static constexpr UINT stageMessage = WM_APP + 1, readyMessage = WM_APP + 2,
                              errorMessage = WM_APP + 3;
        MidiWorker midiWorker;
};
} // namespace manatee