#include "universal_stitcher/DiagnosticLogger.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <utility>
#include <vector>

namespace universal_stitcher {
namespace {

std::wstring environmentValue(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(static_cast<std::size_t>(required), L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    return value;
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::ostringstream value;
    value << std::put_time(&local, "%Y-%m-%dT%H:%M:%S") << '.'
          << std::setfill('0') << std::setw(3) << millis.count();
    return value.str();
}

std::string severityName(DiagnosticLogger::Severity severity) {
    switch (severity) {
    case DiagnosticLogger::Severity::Warning: return "WARN";
    case DiagnosticLogger::Severity::Error: return "ERROR";
    default: return "INFO";
    }
}

std::string timeStem() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_s(&local, &time);
    std::ostringstream value;
    value << "session-" << std::put_time(&local, "%Y%m%d-%H%M%S")
          << "-pid" << GetCurrentProcessId();
    return value.str();
}

} // namespace

DiagnosticLogger::DiagnosticLogger(std::filesystem::path logsDirectoryOverride)
    : logsDirectory_(std::move(logsDirectoryOverride)) {}

DiagnosticLogger::~DiagnosticLogger() {
    disable();
}

bool DiagnosticLogger::ensureLogsDirectoryLocked() {
    if (!logsDirectory_.empty()) {
        std::error_code error;
        if (std::filesystem::exists(logsDirectory_, error) &&
            std::filesystem::is_directory(logsDirectory_, error)) return true;
        error.clear();
        std::filesystem::create_directories(logsDirectory_, error);
        if (!error && std::filesystem::is_directory(logsDirectory_, error)) return true;
        // An explicit test/embedding override was not usable; continue with
        // the normal profile/temp selection below.
        logsDirectory_.clear();
    }

    const std::wstring localAppData = environmentValue(L"LOCALAPPDATA");
    if (!localAppData.empty()) logsDirectory_ = std::filesystem::path(localAppData) /
        L"UniversalScrollStitcher" / L"logs";
    else {
        wchar_t temporary[MAX_PATH]{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
        if (length == 0 || length >= std::size(temporary)) {
            setErrorLocked("LOCALAPPDATA and the temporary-directory fallback are unavailable");
            return false;
        }
        logsDirectory_ = std::filesystem::path(temporary) / L"UniversalScrollStitcher" / L"logs";
    }

    std::error_code error;
    std::filesystem::create_directories(logsDirectory_, error);
    if (!error && std::filesystem::is_directory(logsDirectory_, error)) return true;

    // A machine can expose LOCALAPPDATA but deny writes (roaming profiles,
    // kiosk policies, or a read-only redirected profile).  Fall back to temp.
    wchar_t temporary[MAX_PATH]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    if (length == 0 || length >= std::size(temporary)) {
        setErrorLocked("unable to create the diagnostic log directory");
        return false;
    }
    logsDirectory_ = std::filesystem::path(temporary) / L"UniversalScrollStitcher" / L"logs";
    error.clear();
    std::filesystem::create_directories(logsDirectory_, error);
    if (error || !std::filesystem::is_directory(logsDirectory_, error)) {
        setErrorLocked("unable to create the diagnostic log directory or temp fallback");
        return false;
    }
    return true;
}

bool DiagnosticLogger::ensureLogsDirectory() {
    std::lock_guard lock(mutex_);
    return ensureLogsDirectoryLocked();
}

bool DiagnosticLogger::openPartLocked(unsigned part) {
    if (!ensureLogsDirectoryLocked()) return false;
    if (sessionStem_.empty()) sessionStem_ = timeStem();
    const std::filesystem::path path = logsDirectory_ /
        (sessionStem_ + (part == 1 ? std::string{} : "-part" + std::to_string(part)) + ".log");
    stream_.clear();
    stream_.open(path, std::ios::binary | std::ios::app);
    if (!stream_) {
        setErrorLocked("unable to open the diagnostic log file");
        return false;
    }
    sessionPath_ = path;
    std::error_code error;
    bytesWritten_ = std::filesystem::file_size(path, error);
    if (error) bytesWritten_ = 0;
    part_ = part;
    return true;
}

bool DiagnosticLogger::enable() {
    std::lock_guard lock(mutex_);
    if (enabled_) return true;
    lastError_.clear();
    if (!ensureLogsDirectoryLocked()) return false;
    sessionStem_ = timeStem();
    part_ = 1;
    bytesWritten_ = 0;
    if (!openPartLocked(part_)) {
        // The profile directory may exist but be read-only.  Try a local
        // temporary directory before giving up so diagnostics remain useful
        // on locked-down third-party machines.
        wchar_t temporary[MAX_PATH]{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
        if (length == 0 || length >= std::size(temporary)) return false;
        closeLocked();
        logsDirectory_ = std::filesystem::path(temporary) / L"UniversalScrollStitcher" / L"logs";
        std::error_code error;
        std::filesystem::create_directories(logsDirectory_, error);
        if (error || !openPartLocked(part_)) {
            setErrorLocked("unable to open a diagnostic log in LOCALAPPDATA or the temp fallback");
            return false;
        }
        // The fallback opened successfully; the earlier profile-open error
        // was transient and must not leak into the UI as a false warning.
        lastError_.clear();
    }
    enabled_ = true;
    enforceRetentionLocked();

    sessionId_ = sessionStem_;
    const std::string header = "diagnostic logging enabled; privacy=metadata-only; "
        "screenshots/pixel contents are never written";
    const std::string line = "[" + timestamp() + "] [INFO] [app] [pid=" +
        std::to_string(GetCurrentProcessId()) + "] [session=" + sessionId_ + "] " + header + "\r\n";
    stream_ << line;
    stream_.flush();
    bytesWritten_ += static_cast<std::uintmax_t>(line.size());
    if (!stream_) setErrorLocked("diagnostic log write failed");
    return true;
}

void DiagnosticLogger::closeLocked() noexcept {
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
    stream_.clear();
}

void DiagnosticLogger::disable() noexcept {
    std::lock_guard lock(mutex_);
    if (!enabled_) {
        closeLocked();
        return;
    }
    try {
        const std::string line = "[" + timestamp() + "] [INFO] [app] [pid=" +
            std::to_string(GetCurrentProcessId()) + "] [session=" + sessionId_ + "] diagnostic logging disabled\r\n";
        stream_ << line;
        stream_.flush();
        bytesWritten_ += static_cast<std::uintmax_t>(line.size());
    } catch (...) {
    }
    closeLocked();
    enabled_ = false;
}

bool DiagnosticLogger::enabled() const noexcept {
    std::lock_guard lock(mutex_);
    return enabled_;
}

void DiagnosticLogger::setErrorLocked(const std::string& error) noexcept {
    try { lastError_ = error; } catch (...) {}
}

void DiagnosticLogger::enforceRetentionLocked() noexcept {
    try {
        std::vector<std::filesystem::directory_entry> files;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(logsDirectory_, error)) {
            if (error) break;
            if (entry.is_regular_file(error) && entry.path().extension() == L".log" &&
                entry.path().filename().wstring().rfind(L"session-", 0) == 0) files.push_back(entry);
        }
        std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
            std::error_code leftError, rightError;
            const auto leftTime = left.last_write_time(leftError);
            const auto rightTime = right.last_write_time(rightError);
            return leftError ? false : rightError || leftTime > rightTime;
        });
        for (std::size_t index = kMaximumSessionFiles; index < files.size(); ++index) {
            if (files[index].path() == sessionPath_) continue;
            std::filesystem::remove(files[index].path(), error);
        }
    } catch (...) {
        // Retention is best-effort and must never make logging fatal.
    }
}

void DiagnosticLogger::log(Severity severity, const std::string& category,
                            const std::string& message) noexcept {
    std::lock_guard lock(mutex_);
    if (!enabled_ || !stream_.is_open()) return;
    try {
        std::string safeMessage = message;
        std::replace(safeMessage.begin(), safeMessage.end(), '\r', ' ');
        std::replace(safeMessage.begin(), safeMessage.end(), '\n', ' ');
        const std::string line = "[" + timestamp() + "] [" + severityName(severity) + "] [" +
            category + "] [pid=" + std::to_string(GetCurrentProcessId()) + "] [session=" +
            sessionId_ + "] " + safeMessage + "\r\n";
        if (bytesWritten_ + line.size() > kMaximumFileBytes) {
            closeLocked();
            if (!openPartLocked(part_ + 1)) {
                enabled_ = false;
                return;
            }
            enforceRetentionLocked();
        }
        stream_ << line;
        stream_.flush();
        if (!stream_) {
            setErrorLocked("diagnostic log write failed");
            return;
        }
        lastError_.clear();
        bytesWritten_ += static_cast<std::uintmax_t>(line.size());
    } catch (...) {
        setErrorLocked("unexpected diagnostic logger failure");
    }
}

bool DiagnosticLogger::openLogsFolder() {
    std::lock_guard lock(mutex_);
    if (!ensureLogsDirectoryLocked()) return false;
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", logsDirectory_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        setErrorLocked("Windows could not open the diagnostic log directory");
        return false;
    }
    lastError_.clear();
    return true;
}

std::filesystem::path DiagnosticLogger::logsDirectory() const {
    std::lock_guard lock(mutex_);
    return logsDirectory_;
}

std::filesystem::path DiagnosticLogger::sessionPath() const {
    std::lock_guard lock(mutex_);
    return sessionPath_;
}

std::string DiagnosticLogger::lastError() const {
    std::lock_guard lock(mutex_);
    return lastError_;
}

} // namespace universal_stitcher
