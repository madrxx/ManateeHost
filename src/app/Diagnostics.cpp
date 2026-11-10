#include "app/Diagnostics.hpp"
#include <windows.h>
#include <winrt/base.h>
#include <fstream>
#include <sstream>
#include <iomanip>
namespace manatee {
void Diagnostics::Open() {
    wchar_t temp[MAX_PATH]{};
    if (auto n = GetTempPathW(MAX_PATH, temp); n > 0 && n < MAX_PATH)
        logPath = std::filesystem::path(temp) / L"ManateeHost-startup.log";
    if (!logPath.empty()) {
        std::ofstream clear(logPath);
        clear << "ManateeHost log\n";
    }
    wchar_t executable[32768]{};
    if (GetModuleFileNameW(nullptr, executable, 32768))
        Log("Executable: " + winrt::to_string(executable));
    Log("Process ID: " + std::to_string(GetCurrentProcessId()));
}
void Diagnostics::Log(const std::string &message) {
    std::lock_guard lock(mutex);
    if (!logPath.empty()) {
        std::ofstream file(logPath, std::ios::app);

        SYSTEMTIME now{};
        GetSystemTime(&now);

        std::ostringstream line;
        line << std::setfill('0');
        
        line << now.wYear << '-'
             << std::setw(2) << now.wMonth << '-'
             << std::setw(2) << now.wDay;
             
        line << 'T'
             << std::setw(2) << now.wHour << ':'
             << std::setw(2) << now.wMinute << ':'
             << std::setw(2) << now.wSecond << '.'
             << std::setw(3) << now.wMilliseconds << "Z";
    
        line << "[thread " << GetCurrentThreadId() << "] " << message;
        file << line.str() << "\n";
        
        // also send to debugger output
        OutputDebugStringA((line.str() + "\n").c_str());
    }
}
std::string Diagnostics::CurrentStage() {
    std::lock_guard lock(mutex);
    return startupStage;
}
void Diagnostics::Stage(const char *name) {
    {
        std::lock_guard lock(mutex);
        startupStage = name;
    }
    Log("Stage: " + std::string(name));
}
} // namespace manatee