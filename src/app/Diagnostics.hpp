#pragma once
#include <filesystem>
#include <mutex>
#include <string>
namespace manatee {
class Diagnostics {
    public:
        void Open();
        void Log(const std::string &message);
        void Stage(const char *name);
        std::string CurrentStage();

    private:
        std::mutex mutex;
        std::string startupStage = "initialising Windows Runtime";
        std::filesystem::path logPath;
};
} // namespace manatee