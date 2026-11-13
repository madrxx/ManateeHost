#include "ui/MainWindow.hpp"
#include <stdexcept>
#include <commdlg.h>
#include <algorithm>
#include <cwchar>
#include <utility>

namespace manatee {
namespace {
void checkWindow(bool success, const char *operation) {
    if (!success) {
        auto code = GetLastError();
        throw std::runtime_error(std::string(operation) + " failed (Win32 error " + std::to_string(code) + ").");
    }
}

} // namespace
MainWindow::~MainWindow() {
    onMessage = {};
    Destroy();
}
void MainWindow::Status(const wchar_t *text) {
    SetWindowTextW(statusLabel, text);
}

std::filesystem::path MainWindow::Choose(const wchar_t *filter) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window;
    dialog.lpstrFile = path;
    dialog.nMaxFile = 32768;
    dialog.lpstrFilter = filter;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&dialog) ? std::filesystem::path(path) : std::filesystem::path();
}

std::vector<std::filesystem::path> MainWindow::ChooseMpbs() {
    std::vector<wchar_t> buffer(65536);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrFilter = L"MIDI Program Banks (*.mpb)\0*.mpb\0All files\0*.*\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER |
                   OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError() != 0)
            throw std::runtime_error(
                "Could not open the MPB selection dialog or its selection was too large.");
        return {};
    }
    std::filesystem::path first(buffer.data());
    auto *next = buffer.data() + wcslen(buffer.data()) + 1;
    if (!*next)
        return {first};
    std::vector<std::filesystem::path> paths;
    while (*next) {
        paths.push_back(first / next);
        next += wcslen(next) + 1;
    }
    if (paths.size() > 16)
        throw std::runtime_error("Select at most 16 MPBs.");
    return paths;
}

unsigned MainWindow::SelectedSlot() const {
    auto index = SendMessageW(banks, CB_GETCURSEL, 0, 0);
    if (index < 0 || static_cast<std::size_t>(index) >= slots.size())
        throw std::runtime_error("Select an MPB slot first.");
    return slots[index];
}

void MainWindow::UpdateBanks(const std::vector<LayoutEntry> &entries, const BankSource &source) {
    SendMessageW(banks, CB_RESETCONTENT, 0, 0);
    slots.clear();
    for (const auto &e : entries)
        if (e.id.type == ResourceType::MPB) {
            auto label = L"MPB " + std::to_wstring(e.id.slot) + (e.payload ? L"" : L" (empty)");
            if (const auto *path = source.MpbPath(e.id.slot))
                label += L" - " + path->filename().wstring();
            SendMessageW(banks, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            slots.push_back(e.id.slot);
        }
}

void MainWindow::RefreshSource(const BankSource &source, bool ready, bool hasEngine, bool hasDriver) {
    std::wstring description = L"No banks loaded. Choose MLT/MPB(s).";
    if (source.IsLoose()) {
        description = L"MPBs: " + std::to_wstring(source.Mpbs().size()) + L" selected.";
    } else if (source.GetMode() == BankSource::Mode::MltBundle) {
        description = L"MLT: " + source.Mlt().filename().wstring();
    }
    SetWindowTextW(sourceLabel, description.c_str());

    for (int id : {Driver, OpenMpbs, OpenMlt})
        EnableWindow(GetDlgItem(window, id), ready);
    for (int id : {OpenFpb, OpenFob, ClearEffects})
        EnableWindow(GetDlgItem(window, id), ready && source.IsLoose());
    for (int id : {ActiveBank, ReplaceMpb, AllNotesOff})
        EnableWindow(GetDlgItem(window, id), ready && hasEngine);
    EnableWindow(GetDlgItem(window, Reload), ready && !source.Empty() && hasDriver);
}

void MainWindow::ShowSelectedSlot(unsigned slot) {
    auto it = std::find(slots.begin(), slots.end(), slot);
    SendMessageW(banks, CB_SETCURSEL, it == slots.end() ? -1 : it - slots.begin(), 0);
}

HWND MainWindow::Widget(const wchar_t *type, const wchar_t *label, DWORD style, int x, int y, int w,
                        int h, int id) {
    auto handle =
        CreateWindowW(type, label, WS_CHILD | WS_VISIBLE | style, x, y, w, h, window,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    checkWindow(handle != nullptr, "Creating a window control");
    SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                 TRUE);
    return handle;
}

LRESULT CALLBACK MainWindow::WindowProcedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    auto *view = reinterpret_cast<MainWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        view = static_cast<MainWindow *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
        view->window = hwnd;
    }
    if (!view || !view->onMessage)
        return DefWindowProcW(hwnd, message, w, l);
    return view->onMessage(hwnd, message, w, l);
}

void MainWindow::Create(HINSTANCE instance, int show, MessageHandler handler) {
    onMessage = std::move(handler);

    // describe window
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = WindowProcedure;
    cls.lpszClassName = L"ManateeHost";
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    checkWindow(RegisterClassW(&cls) != 0, "Registering main window");


    const DWORD windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

    // adjust window size to accommodate this client area
    RECT windowRect{0, 0, 340, 268};
    checkWindow(AdjustWindowRectEx(&windowRect, windowStyle, FALSE, 0) != 0, "Calculating main window size");

    window = CreateWindowW(cls.lpszClassName, L"ManateeHost",
                           windowStyle,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
                           nullptr, nullptr, instance, this);
    checkWindow(window != nullptr, "Creating the main window");

    Widget(L"BUTTON", L"manatee.drv", WS_TABSTOP, 10, 10, 100, 30, Driver);
    Widget(L"BUTTON", L"Open MLT", WS_TABSTOP, 120, 10, 100, 30, OpenMlt);
    Widget(L"BUTTON", L"Open MPB(s)", WS_TABSTOP, 230, 10, 100, 30, OpenMpbs);

    sourceLabel = Widget(L"STATIC", L"", 0, 10, 50, 320, 20);

    Widget(L"STATIC", L"Active bank", 0, 10, 80, 90, 24);
    banks = Widget(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 110, 77, 170, 220,
                   ActiveBank);
    Widget(L"BUTTON", L"Swap", WS_TABSTOP, 290, 76, 40, 22, ReplaceMpb);

    Widget(L"BUTTON", L"Load FPB", WS_TABSTOP, 10, 108, 100, 30, OpenFpb);
    Widget(L"BUTTON", L"Load FOB", WS_TABSTOP, 120, 108, 100, 30, OpenFob);
    Widget(L"BUTTON", L"Clear FPB/FOB", WS_TABSTOP, 230, 108, 100, 30, ClearEffects);


    Widget(L"BUTTON", L"All notes off", WS_TABSTOP, 10, 148, 155, 30, AllNotesOff);
    Widget(L"BUTTON", L"Reload files", WS_TABSTOP, 175, 148, 155, 30, Reload);

    statusLabel = Widget(L"STATIC", L"Starting...", SS_LEFT, 10, 188, 320, 70);

    ShowWindow(window, show == SW_HIDE ? SW_SHOWNORMAL : show);
    UpdateWindow(window);
}

void MainWindow::Destroy() {
    if (window && IsWindow(window))
        DestroyWindow(window);
}
} // namespace manatee