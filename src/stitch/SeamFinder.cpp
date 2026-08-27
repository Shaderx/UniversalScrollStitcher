#include "universal_stitcher/SeamFinder.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>

namespace universal_stitcher {

Seam SeamFinder::find(const cv::Mat& previousBgra, const cv::Mat& currentBgra,
                      int shift, int guardRows, int minimumCurrentRow) {
    if (previousBgra.empty() || currentBgra.empty() ||
        previousBgra.type() != CV_8UC4 || currentBgra.type() != CV_8UC4 ||
        previousBgra.size() != currentBgra.size() || shift <= 0 || shift >= previousBgra.rows) {
        return {};
    }

    const int overlap = previousBgra.rows - shift;
    const int requestedFirst = std::max(guardRows, minimumCurrentRow);
    int firstRow = std::clamp(requestedFirst, 1, std::max(1, overlap - 1));
    int lastRow = std::clamp(overlap - guardRows - 1, firstRow, overlap - 1);
    if (lastRow < firstRow) return {};

    cv::Mat previousGray, currentGray;
    cv::cvtColor(previousBgra, previousGray, cv::COLOR_BGRA2GRAY);
    cv::cvtColor(currentBgra, currentGray, cv::COLOR_BGRA2GRAY);
    constexpr int kMaximumSeamWidth = 480;
    if (previousGray.cols > kMaximumSeamWidth) {
        cv::resize(previousGray, previousGray,
                   cv::Size(kMaximumSeamWidth, previousGray.rows), 0.0, 0.0, cv::INTER_AREA);
        cv::resize(currentGray, currentGray,
                   cv::Size(kMaximumSeamWidth, currentGray.rows), 0.0, 0.0, cv::INTER_AREA);
    }
    const int marginX = std::clamp(previousGray.cols / 20, 2, previousGray.cols / 4);
    const int width = previousGray.cols - marginX * 2;
    if (width <= 0) return {};

    cv::Mat samePositionDifference;
    cv::absdiff(previousGray, currentGray, samePositionDifference);
    cv::Mat activity;
    cv::threshold(samePositionDifference, activity, 3.0, 255.0, cv::THRESH_BINARY);
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(activity, activity, cv::MORPH_CLOSE, kernel);
    cv::dilate(activity, activity, kernel);
    if (cv::countNonZero(activity) < std::max(64, static_cast<int>(activity.total() / 100))) {
        activity.release();
    }

    Seam best;
    best.cost = 2.0F;
    const int window = std::max(1, std::min(3, overlap / 20));
    for (int row = firstRow; row <= lastRow; ++row) {
        const int top = std::max(firstRow, row - window);
        const int bottom = std::min(lastRow, row + window);
        float difference = 0.0F;
        int comparedRows = 0;
        for (int y = top; y <= bottom; ++y) {
            cv::Mat oldRoi = previousGray(cv::Rect(marginX, y + shift, width, 1));
            cv::Mat newRoi = currentGray(cv::Rect(marginX, y, width, 1));
            cv::Mat absDifference;
            cv::absdiff(oldRoi, newRoi, absDifference);
            if (!activity.empty()) {
                const cv::Mat oldActivity = activity(cv::Rect(marginX, y + shift, width, 1));
                const cv::Mat newActivity = activity(cv::Rect(marginX, y, width, 1));
                cv::Mat combinedActivity;
                cv::bitwise_and(oldActivity, newActivity, combinedActivity);
                if (cv::countNonZero(combinedActivity) < std::max(4, width / 100)) continue;
                difference += static_cast<float>(cv::mean(absDifference, combinedActivity)[0] / 255.0);
            } else {
                difference += static_cast<float>(cv::mean(absDifference)[0] / 255.0);
            }
            ++comparedRows;
        }
        if (comparedRows == 0) continue;
        difference /= static_cast<float>(comparedRows);
        if (difference < best.cost) best = {true, row, difference};
    }
    return best;
}

} // namespace universal_stitcher
