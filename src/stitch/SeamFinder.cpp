#include "universal_stitcher/SeamFinder.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

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
    constexpr int kMaximumSeamWidth = 256;
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

    // Compute every aligned row difference in one vectorized pass, then use
    // prefix sums for the small smoothing window. This replaces hundreds of
    // per-row cv::Mat allocations and is important for keeping up with a 60 Hz
    // capture stream.
    cv::Mat alignedDifference;
    cv::absdiff(previousGray(cv::Rect(marginX, shift, width, overlap)),
                currentGray(cv::Rect(marginX, 0, width, overlap)), alignedDifference);
    std::vector<float> rowCosts(static_cast<std::size_t>(overlap), -1.0F);
    if (!activity.empty()) {
        cv::Mat combinedActivity;
        cv::bitwise_and(activity(cv::Rect(marginX, shift, width, overlap)),
                        activity(cv::Rect(marginX, 0, width, overlap)), combinedActivity);
        cv::Mat maskedDifference = cv::Mat::zeros(alignedDifference.size(), alignedDifference.type());
        alignedDifference.copyTo(maskedDifference, combinedActivity);
        cv::Mat differenceSums;
        cv::Mat activitySums;
        cv::reduce(maskedDifference, differenceSums, 1, cv::REDUCE_SUM, CV_64F);
        cv::reduce(combinedActivity, activitySums, 1, cv::REDUCE_SUM, CV_64F);
        const int minimumActive = std::max(4, width / 100);
        for (int row = 0; row < overlap; ++row) {
            const double activePixels = activitySums.at<double>(row, 0) / 255.0;
            if (activePixels >= minimumActive) {
                rowCosts[static_cast<std::size_t>(row)] = static_cast<float>(
                    differenceSums.at<double>(row, 0) / (activePixels * 255.0));
            }
        }
    } else {
        cv::Mat averages;
        cv::reduce(alignedDifference, averages, 1, cv::REDUCE_AVG, CV_32F);
        for (int row = 0; row < overlap; ++row) {
            rowCosts[static_cast<std::size_t>(row)] = averages.at<float>(row, 0) / 255.0F;
        }
    }

    std::vector<double> costPrefix(static_cast<std::size_t>(overlap + 1), 0.0);
    std::vector<int> countPrefix(static_cast<std::size_t>(overlap + 1), 0);
    for (int row = 0; row < overlap; ++row) {
        const float cost = rowCosts[static_cast<std::size_t>(row)];
        costPrefix[static_cast<std::size_t>(row + 1)] =
            costPrefix[static_cast<std::size_t>(row)] + (cost >= 0.0F ? cost : 0.0F);
        countPrefix[static_cast<std::size_t>(row + 1)] =
            countPrefix[static_cast<std::size_t>(row)] + (cost >= 0.0F ? 1 : 0);
    }

    Seam best;
    best.cost = 2.0F;
    const int window = std::max(1, std::min(3, overlap / 20));
    for (int row = firstRow; row <= lastRow; ++row) {
        const int top = std::max(firstRow, row - window);
        const int bottom = std::min(lastRow, row + window);
        const int comparedRows = countPrefix[static_cast<std::size_t>(bottom + 1)] -
                                 countPrefix[static_cast<std::size_t>(top)];
        if (comparedRows == 0) continue;
        const float difference = static_cast<float>(
            (costPrefix[static_cast<std::size_t>(bottom + 1)] -
             costPrefix[static_cast<std::size_t>(top)]) / comparedRows);
        if (difference < best.cost) best = {true, row, difference};
    }
    return best;
}

} // namespace universal_stitcher
