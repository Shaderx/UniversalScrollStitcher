#include "universal_stitcher/CalibrationProfile.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <string_view>

namespace universal_stitcher {
namespace {

constexpr int kMaximumDimension = 1'000'000;

bool rectFits(const Rect& rect, int width, int height) noexcept {
    return rect.valid() && rect.x >= 0 && rect.y >= 0 &&
           rect.right() <= width && rect.bottom() <= height;
}

bool parseInteger(std::string_view text, int& value) {
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

Rect scaledRect(const Rect& source, int oldWidth, int oldHeight,
                int newWidth, int newHeight) noexcept {
    const auto scaleX = [oldWidth, newWidth](int value) {
        return static_cast<int>(std::lround(static_cast<double>(value) * newWidth / oldWidth));
    };
    const auto scaleY = [oldHeight, newHeight](int value) {
        return static_cast<int>(std::lround(static_cast<double>(value) * newHeight / oldHeight));
    };
    const int left = std::clamp(scaleX(source.x), 0, newWidth);
    const int top = std::clamp(scaleY(source.y), 0, newHeight);
    const int right = std::clamp(scaleX(source.right()), left, newWidth);
    const int bottom = std::clamp(scaleY(source.bottom()), top, newHeight);
    return {left, top, right - left, bottom - top};
}

} // namespace

bool CalibrationProfile::valid() const noexcept {
    return frameWidth > 0 && frameHeight > 0 &&
           frameWidth <= kMaximumDimension && frameHeight <= kMaximumDimension &&
           rectFits(content, frameWidth, frameHeight) &&
           rectFits(scrollbar.track, frameWidth, frameHeight);
}

CalibrationProfile CalibrationProfile::scaledTo(int newWidth, int newHeight) const noexcept {
    if (!valid() || newWidth <= 0 || newHeight <= 0 ||
        newWidth > kMaximumDimension || newHeight > kMaximumDimension) {
        return {};
    }
    CalibrationProfile scaled = *this;
    scaled.content = scaledRect(content, frameWidth, frameHeight, newWidth, newHeight);
    scaled.scrollbar.track = scaledRect(scrollbar.track, frameWidth, frameHeight,
                                        newWidth, newHeight);
    scaled.frameWidth = newWidth;
    scaled.frameHeight = newHeight;
    return scaled;
}

bool saveCalibrationProfile(const std::filesystem::path& path,
                            const CalibrationProfile& profile,
                            std::string& error) {
    error.clear();
    if (!profile.valid()) {
        error = "calibration rectangles are invalid or outside the reference frame";
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to open the calibration file for writing";
        return false;
    }
    output << "universal_scroll_stitcher_template=" << CalibrationProfile::kCurrentVersion << '\n'
           << "frame_width=" << profile.frameWidth << '\n'
           << "frame_height=" << profile.frameHeight << '\n'
           << "content_x=" << profile.content.x << '\n'
           << "content_y=" << profile.content.y << '\n'
           << "content_width=" << profile.content.width << '\n'
           << "content_height=" << profile.content.height << '\n'
           << "scrollbar_x=" << profile.scrollbar.track.x << '\n'
           << "scrollbar_y=" << profile.scrollbar.track.y << '\n'
           << "scrollbar_width=" << profile.scrollbar.track.width << '\n'
           << "scrollbar_height=" << profile.scrollbar.track.height << '\n'
           << "scrollbar_side="
           << (profile.scrollbar.side == ScrollbarSide::Left ? "left" : "right") << '\n';
    output.flush();
    if (!output) {
        error = "unable to finish writing the calibration file";
        return false;
    }
    return true;
}

std::optional<CalibrationProfile> loadCalibrationProfile(const std::filesystem::path& path,
                                                         std::string& error) {
    error.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open the calibration file";
        return std::nullopt;
    }

    std::map<std::string, std::string> values;
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size()) {
            error = "invalid calibration entry on line " + std::to_string(lineNumber);
            return std::nullopt;
        }
        const std::string key = line.substr(0, separator);
        if (!values.emplace(key, line.substr(separator + 1)).second) {
            error = "duplicate calibration entry: " + key;
            return std::nullopt;
        }
    }
    if (!input.eof()) {
        error = "unable to read the complete calibration file";
        return std::nullopt;
    }

    auto requiredInteger = [&](const char* key, int& destination) {
        const auto found = values.find(key);
        if (found == values.end() || !parseInteger(found->second, destination)) {
            error = std::string("missing or invalid calibration entry: ") + key;
            return false;
        }
        return true;
    };

    int version = 0;
    CalibrationProfile profile;
    if (!requiredInteger("universal_scroll_stitcher_template", version) ||
        !requiredInteger("frame_width", profile.frameWidth) ||
        !requiredInteger("frame_height", profile.frameHeight) ||
        !requiredInteger("content_x", profile.content.x) ||
        !requiredInteger("content_y", profile.content.y) ||
        !requiredInteger("content_width", profile.content.width) ||
        !requiredInteger("content_height", profile.content.height) ||
        !requiredInteger("scrollbar_x", profile.scrollbar.track.x) ||
        !requiredInteger("scrollbar_y", profile.scrollbar.track.y) ||
        !requiredInteger("scrollbar_width", profile.scrollbar.track.width) ||
        !requiredInteger("scrollbar_height", profile.scrollbar.track.height)) {
        return std::nullopt;
    }
    if (version != CalibrationProfile::kCurrentVersion) {
        error = "unsupported calibration template version: " + std::to_string(version);
        return std::nullopt;
    }
    const auto side = values.find("scrollbar_side");
    if (side == values.end() || (side->second != "left" && side->second != "right")) {
        error = "missing or invalid calibration entry: scrollbar_side";
        return std::nullopt;
    }
    profile.scrollbar.side = side->second == "left" ? ScrollbarSide::Left : ScrollbarSide::Right;
    profile.scrollbar.enabled = true;
    if (!profile.valid()) {
        error = "calibration rectangles are invalid or outside the reference frame";
        return std::nullopt;
    }
    return profile;
}

} // namespace universal_stitcher
