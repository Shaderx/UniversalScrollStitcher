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
    // Ranking combines motion-thumb contrast, neutral scrollbar color, edge
    // proximity, and the detector's run confidence. The GUI uses it to order
    // every distinct candidate in its manual selection list.
    float autoDetectionScore = 0.0F;
};

// The detector looks for a compact contrast run in a user-supplied track
// rectangle. Automatic ranking also favors the low-chroma gray-on-light
// appearance used by standard and game scrollbars without requiring an exact
// hard-coded color.
class ScrollbarDetector final {
public:
    // Returns distinct candidates across the full window, ordered by automatic
    // selection quality. Searching the interior is important for games and
    // split-pane applications whose scrollable panel occupies only part of a
    // larger top-level window.
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
