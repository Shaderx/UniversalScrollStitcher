#include "universal_stitcher/DiagnosticLogger.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool require(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "logger test failure: " << message << '\n';
    return false;
}

std::size_t sessionFileCount(const std::filesystem::path& directory) {
    std::size_t count = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (!error && entry.is_regular_file(error) && entry.path().extension() == L".log" &&
            entry.path().filename().wstring().rfind(L"session-", 0) == 0) ++count;
    }
    return count;
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("UniversalScrollStitcher-logger-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);

    bool passed = true;
    {
        universal_stitcher::DiagnosticLogger logger(root);
        passed &= require(!logger.enabled(), "logger must be disabled by default");
        passed &= require(logger.ensureLogsDirectory(), "logger directory should be creatable");
        passed &= require(std::filesystem::is_directory(root), "logger directory should exist");

        // Seed old session files to exercise bounded retention during enable.
        for (int index = 0; index < 12; ++index) {
            const auto path = root / ("session-old-" + std::to_string(index) + ".log");
            std::ofstream(path) << "old\n";
            std::filesystem::last_write_time(path,
                std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 + index), error);
        }

        passed &= require(logger.enable(), "logger should enable in an explicit writable directory");
        passed &= require(logger.enabled(), "logger should report enabled");
        const auto activePath = logger.sessionPath();
        passed &= require(!activePath.empty() && std::filesystem::exists(activePath),
                          "active session path should exist");
        passed &= require(sessionFileCount(root) <= 10, "retention should keep at most ten session files");

        logger.info("test", "metadata entry written");
        logger.warning("test", "warning entry written");
        logger.disable();
        passed &= require(!logger.enabled(), "logger should disable cleanly");

        std::ifstream input(activePath, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        passed &= require(content.find("diagnostic logging enabled") != std::string::npos,
                          "session header should be present");
        passed &= require(content.find("[INFO] [test]") != std::string::npos,
                          "info category should be present");
        passed &= require(content.find("[WARN] [test]") != std::string::npos,
                          "warning category should be present");
    }

    std::filesystem::remove_all(root, error);
    return passed ? 0 : 1;
}
