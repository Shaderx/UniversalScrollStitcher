#pragma once

#include "universal_stitcher/ScrollbarDetector.h"
#include "universal_stitcher/SeamFinder.h"
#include "universal_stitcher/StripStore.h"
#include "universal_stitcher/VerticalShiftEstimator.h"

#include <opencv2/core.hpp>

#include <string>

namespace universal_stitcher {

enum class SessionState { Idle, Capturing, Paused, Finalized, Failed };

struct StitchOptions {
    Rect viewport;
    ScrollbarConfig scrollbar;
    ShiftEstimatorOptions estimator;
    // Fixed headers and footers must not participate in vertical
    // registration. When the scrollbar track covers only a middle band of a
    // broader viewport, use that vertical band as the effective stitch area.
    bool maskStaticOutsideScrollbar = true;
};

struct StitchUpdate {
    bool accepted = false;
    // A frame that could not be registered against the pending frame. The
    // session keeps the pending frame and stays live, so scrolling back up a
    // little restores the overlap and capture continues without a gap.
    bool rejected = false;
    bool paused = false;
    bool atTop = false;
    bool atBottom = false;
    bool scrollbarVisible = false;
    int shift = 0;
    int expectedShift = 0;
    int consecutiveRejections = 0;
    float confidence = 0.0F;
    float margin = 0.0F;
    float scrollbarConfidence = 0.0F;
    std::string message;
};

class StitchSession final {
public:
    StitchSession() = default;
    ~StitchSession() = default;

    StitchSession(const StitchSession&) = delete;
    StitchSession& operator=(const StitchSession&) = delete;

    [[nodiscard]] bool start(const cv::Mat& firstFrame, const StitchOptions& options);
    [[nodiscard]] StitchUpdate process(const cv::Mat& frame);
    [[nodiscard]] bool finish();
    void reset();

    [[nodiscard]] SessionState state() const noexcept { return state_; }
    // The finalized image remains in the disk-backed store. Use outputStore()
    // for streaming export; finalImage() is retained only as a compatibility
    // accessor and is intentionally empty for long-capture safety.
    [[nodiscard]] const cv::Mat& finalImage() const noexcept { return finalImage_; }
    [[nodiscard]] const StripStore& outputStore() const noexcept { return store_; }
    [[nodiscard]] bool hasOutput() const noexcept {
        return state_ == SessionState::Finalized && store_.isOpen() && store_.rows() > 0;
    }
    [[nodiscard]] const std::string& lastMessage() const noexcept { return lastMessage_; }
    [[nodiscard]] int acceptedFrames() const noexcept { return acceptedFrames_; }
    [[nodiscard]] int outputRows() const noexcept { return outputRows_; }
    [[nodiscard]] const Rect& viewport() const noexcept { return options_.viewport; }

private:
    // Transient conditions must not end a long capture. A thumb that fades
    // out, a duplicate frame, or a scroll that briefly outran the overlap all
    // recover on their own; only a sustained failure escalates to Paused.
    static constexpr int kInitialScrollbarMissPauseThreshold = 30;
    static constexpr int kRejectionPauseThreshold = 50;
    static constexpr int kThumbNoiseTolerance = 2;

    StitchOptions options_;
    ScrollbarState scrollbar_;
    cv::Mat pending_;
    cv::Mat finalImage_;
    int pendingStart_ = 0;
    int acceptedFrames_ = 0;
    int outputRows_ = 0;
    int consecutiveRejections_ = 0;
    int consecutiveScrollbarMisses_ = 0;
    SessionState state_ = SessionState::Idle;
    std::string lastMessage_;
    StripStore store_;
};

} // namespace universal_stitcher
