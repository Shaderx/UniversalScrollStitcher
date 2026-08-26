#pragma once

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <mutex>
#include <string>

namespace universal_stitcher {

// Diagnostic logging is deliberately opt-in.  The logger writes only textual
// session metadata and never stores captured frames or screenshot pixels.
class DiagnosticLogger final {
public:
    enum class Severity { Info, Warning, Error };

    explicit DiagnosticLogger(std::filesystem::path logsDirectoryOverride = {});
    ~DiagnosticLogger();

    DiagnosticLogger(const DiagnosticLogger&) = delete;
    DiagnosticLogger& operator=(const DiagnosticLogger&) = delete;

    [[nodiscard]] bool enable();
    void disable() noexcept;
    [[nodiscard]] bool enabled() const noexcept;

    // Creates %LOCALAPPDATA%\\UniversalScrollStitcher\\logs, or a writable
    // temporary-directory fallback, without enabling logging.
    [[nodiscard]] bool ensureLogsDirectory();
    [[nodiscard]] bool openLogsFolder();

    void log(Severity severity, const std::string& category, const std::string& message) noexcept;
    void info(const std::string& category, const std::string& message) noexcept {
        log(Severity::Info, category, message);
    }
    void warning(const std::string& category, const std::string& message) noexcept {
        log(Severity::Warning, category, message);
    }
    void error(const std::string& category, const std::string& message) noexcept {
        log(Severity::Error, category, message);
    }

    [[nodiscard]] std::filesystem::path logsDirectory() const;
    [[nodiscard]] std::filesystem::path sessionPath() const;
    [[nodiscard]] std::string lastError() const;

private:
    static constexpr std::uintmax_t kMaximumFileBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t kMaximumSessionFiles = 10;

    mutable std::mutex mutex_;
    std::ofstream stream_;
    std::filesystem::path logsDirectory_;
    std::filesystem::path sessionPath_;
    std::string sessionStem_;
    std::string sessionId_;
    std::string lastError_;
    std::uintmax_t bytesWritten_ = 0;
    unsigned part_ = 1;
    bool enabled_ = false;

    [[nodiscard]] bool ensureLogsDirectoryLocked();
    [[nodiscard]] bool openPartLocked(unsigned part);
    void enforceRetentionLocked() noexcept;
    void closeLocked() noexcept;
    void setErrorLocked(const std::string& error) noexcept;
};

} // namespace universal_stitcher
