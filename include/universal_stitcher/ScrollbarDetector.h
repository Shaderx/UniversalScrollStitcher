#pragma once

#include "universal_stitcher/Frame.h"

#include <optional>
#include <vector>

namespace universal_stitcher {

enum class ScrollbarSide { Left, Right };

struct ScrollbarConfig {
    Rect track;
    ScrollbarSide side = ScrollbarSide::Right;
    bool enabled = true;
};

struct ScrollbarObservation {
    bool detected = false;
    Rect thumb;
    float confidence = 0.0F;

    [[nodiscard]] int thumbTop() const noexcept { return thumb.y; }
    [[nodiscard]] int thumbBottom() const noexcept { return thumb.y + thumb.height; }
};

struct ScrollbarCandidate {
    ScrollbarConfig config;
    ScrollbarObservation observation;
};

// This detector deliberately has no theme, game, or color template. It looks
// for a compact, persistent contrast run in a user-supplied track rectangle.
class ScrollbarDetector final {
public:
    // Returns distinct candidates ordered by confidence. Multiple candidates
    // are kept so the calibration UI can show them all and let the user pick
    // the correct scrollbar for nested or side-by-side scrolling views.
    [[nodiscard]] static std::vector<ScrollbarCandidate> autoDetectAll(const cv::Mat& bgra);
    [[nodiscard]] static std::optional<ScrollbarConfig> autoDetect(const cv::Mat& bgra);
    [[nodiscard]] static ScrollbarObservation detect(const cv::Mat& bgra,
                                                     const ScrollbarConfig& config);
};

struct ScrollbarState {
    bool valid = false;
    int previousTop = 0;
    int top = 0;
    int thumbHeight = 0;
    int trackTop = 0;
    int trackBottom = 0;
    int delta = 0;
    float confidence = 0.0F;
    bool atTop = false;
    bool atBottom = false;

    // The initial prior is intentionally conservative. It is refined from
    // accepted visual registrations in StitchSession.
    float learnedContentPerThumbPixel = 0.0F;

    [[nodiscard]] int expectedContentShift(int viewportHeight) const noexcept;
};

} // namespace universal_stitcher
