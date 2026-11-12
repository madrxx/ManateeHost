#pragma once
#include <windows.h>
#include "app/BankSource.hpp"
#include "banks/RamLayout.hpp"
#include <functional>

namespace manatee {
enum ControlId {
    Driver = 1,
    OpenMlt,
    ActiveBank,
    ReplaceMpb,
    AllNotesOff,
    Reload,
    OpenMpbs,
    OpenFpb,
    OpenFob,
    ClearEffects
};
class MainWindow {
  public:
    using MessageHandler = std::function<LRESULT(HWND, UINT, WPARAM, LPARAM)>;
    MainWindow() = default;
    ~MainWindow();
    MainWindow(const MainWindow &) = delete;
    MainWindow &operator=(const MainWindow &) = delete;
    void Create(HINSTANCE instance, int show, MessageHandler handler);
    void Destroy();
    HWND Handle() const { return window; }
    void Status(const wchar_t *text);
    std::filesystem::path Choose(const wchar_t *filter);
    std::vector<std::filesystem::path> ChooseMpbs();
    unsigned SelectedSlot() const;
    bool HasSlots() const { return !slots.empty(); }
    unsigned FirstSlot() const { return slots.front(); }
    bool IsBankControl(HWND control) const { return control == banks; }
    void UpdateBanks(const std::vector<LayoutEntry> &entries, const BankSource &source);
    void ShowSelectedSlot(unsigned slot);
    void RefreshSource(const BankSource &source, bool ready, bool hasEngine, bool hasDriver);

  private:
    static LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM w, LPARAM l);
    HWND Widget(const wchar_t *type, const wchar_t *label, DWORD style, int x, int y, int width,
                int height, int id = 0);
    HWND window{}, statusLabel{}, driverLabel{}, sourceLabel{}, banks{};
    std::vector<unsigned> slots;
    MessageHandler onMessage;
};
} // namespace manatee