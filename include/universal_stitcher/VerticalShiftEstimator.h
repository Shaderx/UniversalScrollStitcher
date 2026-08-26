#pragma once

#include <opencv2/core.hpp>

#include <string>

namespace universal_stitcher {

struct ShiftEstimate {
    bool accepted = false;
    bool duplicate = false;
    int shift = 0;
    int expectedShift = 0;
    int overlap = 0;
    float confidence = 0.0F;
    float margin = 0.0F;
    std::string reason;
};

struct ShiftEstimatorOptions {
    float minimumConfidence = 0.78F;
    float minimumMargin = 0.012F;
    float priorToleranceRatio = 0.75F;
    int minimumPriorTolerance = 18;
    float minimumOverlapRatio = 0.25F;
};

// Estimates the amount by which current content moved downward relative to
// previous content. OpenCV is used for grayscale/edge registration; no fixed
// content template is involved.
class VerticalShiftEstimator final {
public:
    [[nodiscard]] static ShiftEstimate estimate(const cv::Mat& previousBgra,
                                                 const cv::Mat& currentBgra,
                                                 int expectedShift,
                                                 const ShiftEstimatorOptions& options = {});
};

} // namespace universal_stitcher

