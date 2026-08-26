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
};

struct StitchUpdate {
    bool accepted = false;
    bool paused = false;
    bool atTop = false;
    bool atBottom = false;
    int shift = 0;
    int expectedShift = 0;
    float confidence = 0.0F;
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
    StitchOptions options_;
    ScrollbarState scrollbar_;
    cv::Mat pending_;
    cv::Mat finalImage_;
    int pendingStart_ = 0;
    int acceptedFrames_ = 0;
    int outputRows_ = 0;
    SessionState state_ = SessionState::Idle;
    std::string lastMessage_;
    StripStore store_;
};

} // namespace universal_stitcher
