#include "universal_stitcher/SeamFinder.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace universal_stitcher {

Seam SeamFinder::find(const cv::Mat& previousBgra, const cv::Mat& currentBgra,
                      int shift, int guardRows) {
    if (previousBgra.empty() || currentBgra.empty() ||
        previousBgra.type() != CV_8UC4 || currentBgra.type() != CV_8UC4 ||
        previousBgra.size() != currentBgra.size() || shift <= 0 || shift >= previousBgra.rows) {
        return {};
    }

    const int overlap = previousBgra.rows - shift;
    const int firstRow = std::clamp(guardRows, 1, std::max(1, overlap - 2));
    const int lastRow = std::clamp(overlap - guardRows - 1, firstRow, overlap - 1);
    const int marginX = std::clamp(previousBgra.cols / 20, 2, previousBgra.cols / 4);
    const int width = previousBgra.cols - marginX * 2;
    if (width <= 0 || lastRow < firstRow) return {};

    cv::Mat previousGray, currentGray;
    cv::cvtColor(previousBgra, previousGray, cv::COLOR_BGRA2GRAY);
    cv::cvtColor(currentBgra, currentGray, cv::COLOR_BGRA2GRAY);

    Seam best;
    best.cost = 2.0F;
    const int window = std::max(1, std::min(3, overlap / 20));
    for (int row = firstRow; row <= lastRow; ++row) {
        const int top = std::max(firstRow, row - window);
        const int bottom = std::min(lastRow, row + window);
        float difference = 0.0F;
        for (int y = top; y <= bottom; ++y) {
            cv::Mat oldRoi = previousGray(cv::Rect(marginX, y + shift, width, 1));
            cv::Mat newRoi = currentGray(cv::Rect(marginX, y, width, 1));
            cv::Mat absDifference;
            cv::absdiff(oldRoi, newRoi, absDifference);
            difference += static_cast<float>(cv::mean(absDifference)[0] / 255.0);
        }
        difference /= static_cast<float>(bottom - top + 1);
        if (difference < best.cost) best = {true, row, difference};
    }
    return best;
}

} // namespace universal_stitcher

