#pragma once

#include <opencv2/core.hpp>

namespace universal_stitcher {

struct Seam {
    bool valid = false;
    int currentRow = 0;
    float cost = 1.0F;
};

class SeamFinder final {
public:
    // currentRow is a row in currentBgra's overlap. The corresponding row in
    // previousBgra is currentRow + shift.
    [[nodiscard]] static Seam find(const cv::Mat& previousBgra,
                                   const cv::Mat& currentBgra,
                                   int shift,
                                   int guardRows = 3);
};

} // namespace universal_stitcher

