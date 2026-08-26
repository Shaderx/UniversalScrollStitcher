#pragma once

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdint>

namespace universal_stitcher {

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
    [[nodiscard]] int right() const noexcept { return x + width; }
    [[nodiscard]] int bottom() const noexcept { return y + height; }

    [[nodiscard]] Rect clampTo(int imageWidth, int imageHeight) const noexcept {
        const int left = std::clamp(x, 0, std::max(0, imageWidth));
        const int top = std::clamp(y, 0, std::max(0, imageHeight));
        const int rightEdge = std::clamp(right(), left, std::max(left, imageWidth));
        const int bottomEdge = std::clamp(bottom(), top, std::max(top, imageHeight));
        return {left, top, rightEdge - left, bottomEdge - top};
    }
};

// Captures and stitched images are BGRA (8-bit, four channels). Keeping the
// alpha channel makes the capture path zero-copy-friendly; WIC converts it
// when writing JPEG.
struct CaptureFrame {
    cv::Mat bgra;

    [[nodiscard]] bool valid() const noexcept {
        return !bgra.empty() && bgra.type() == CV_8UC4;
    }

    [[nodiscard]] int width() const noexcept { return bgra.cols; }
    [[nodiscard]] int height() const noexcept { return bgra.rows; }

    [[nodiscard]] CaptureFrame crop(const Rect& requested) const {
        const Rect clipped = requested.clampTo(width(), height());
        if (!valid() || !clipped.valid()) return {};
        return {bgra(cv::Rect(clipped.x, clipped.y, clipped.width, clipped.height)).clone()};
    }
};

} // namespace universal_stitcher
