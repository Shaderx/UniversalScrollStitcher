#pragma once

#include "universal_stitcher/ScrollbarDetector.h"

#include <filesystem>
#include <optional>
#include <string>

namespace universal_stitcher {

// A portable calibration file records both user-editable regions together
// with the capture dimensions they were measured against. Profiles are
// scaled on load when the target window has changed size.
struct CalibrationProfile {
    static constexpr int kCurrentVersion = 1;

    int frameWidth = 0;
    int frameHeight = 0;
    Rect content;
    ScrollbarConfig scrollbar;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] CalibrationProfile scaledTo(int newWidth, int newHeight) const noexcept;
};

[[nodiscard]] bool saveCalibrationProfile(const std::filesystem::path& path,
                                          const CalibrationProfile& profile,
                                          std::string& error);

[[nodiscard]] std::optional<CalibrationProfile> loadCalibrationProfile(
    const std::filesystem::path& path, std::string& error);

} // namespace universal_stitcher
