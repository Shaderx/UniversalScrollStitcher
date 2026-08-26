#include "universal_stitcher/VerticalShiftEstimator.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace universal_stitcher {
namespace {

struct Candidate {
    int shift = 0;
    float score = -1.0F;
    float agreement = 0.0F;
};

cv::Mat registrationImage(const cv::Mat& bgra) {
    cv::Mat gray;
    cv::cvtColor(bgra, gray, cv::COLOR_BGRA2GRAY);

    cv::Mat gx, gy, magnitude, edges;
    cv::Sobel(gray, gx, CV_32F, 1, 0, 3);
    cv::Sobel(gray, gy, CV_32F, 0, 1, 3);
    cv::magnitude(gx, gy, magnitude);
    magnitude.convertTo(edges, CV_8U, 0.25);

    cv::Mat result;
    cv::addWeighted(gray, 0.72, edges, 0.28, 0.0, result);
    return result;
}

float scoreBand(const cv::Mat& previous, const cv::Mat& current, int shift,
                int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return 0.0F;
    const cv::Mat oldRoi = previous(cv::Rect(x, y + shift, width, height));
    const cv::Mat newRoi = current(cv::Rect(x, y, width, height));
    cv::Mat difference;
    cv::absdiff(oldRoi, newRoi, difference);
    const double meanDifference = cv::mean(difference)[0];
    return std::clamp(1.0F - static_cast<float>(meanDifference / 255.0), 0.0F, 1.0F);
}

} // namespace

ShiftEstimate VerticalShiftEstimator::estimate(const cv::Mat& previousBgra,
                                                const cv::Mat& currentBgra,
                                                int expectedShift,
                                                const ShiftEstimatorOptions& options) {
    ShiftEstimate result;
    if (previousBgra.empty() || currentBgra.empty() ||
        previousBgra.type() != CV_8UC4 || currentBgra.type() != CV_8UC4 ||
        previousBgra.size() != currentBgra.size()) {
        result.reason = "frames have different sizes or formats";
        return result;
    }

    const int width = previousBgra.cols;
    const int height = previousBgra.rows;
    if (width < 16 || height < 32) {
        result.reason = "viewport is too small for registration";
        return result;
    }
    result.expectedShift = std::clamp(expectedShift, 0, height - 1);

    const cv::Mat previous = registrationImage(previousBgra);
    const cv::Mat current = registrationImage(currentBgra);
    const int marginX = std::clamp(width / 20, 2, width / 4);
    const int comparisonWidth = width - marginX * 2;
    const int broadMaximum = std::max(1, static_cast<int>(height * 0.74F));

    int minimumShift = 1;
    int maximumShift = broadMaximum;
    if (result.expectedShift > 0) {
        const int tolerance = std::max(options.minimumPriorTolerance,
                                       static_cast<int>(std::lround(result.expectedShift * options.priorToleranceRatio)));
        minimumShift = std::max(1, result.expectedShift - tolerance);
        maximumShift = std::min(broadMaximum, result.expectedShift + tolerance);
    }

    std::vector<Candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(maximumShift - minimumShift + 1));
    for (int shift = minimumShift; shift <= maximumShift; ++shift) {
        const int overlap = height - shift;
        if (overlap < static_cast<int>(height * options.minimumOverlapRatio)) continue;

        const int bandHeight = std::max(8, overlap / 4);
        float scores[4]{};
        int bandCount = 0;
        for (int band = 0; band < 4; ++band) {
            const int y = band == 3 ? std::max(0, overlap - bandHeight) : band * overlap / 4;
            const int availableHeight = std::min(bandHeight, overlap - y);
            scores[band] = scoreBand(previous, current, shift, marginX, y, comparisonWidth, availableHeight);
            if (availableHeight > 0) ++bandCount;
        }
        if (bandCount == 0) continue;
        float mean = 0.0F;
        float minimum = 1.0F;
        for (float score : scores) {
            mean += score;
            minimum = std::min(minimum, score);
        }
        mean /= 4.0F;
        candidates.push_back({shift, mean * 0.72F + minimum * 0.28F, minimum});
    }

    if (candidates.empty()) {
        result.reason = "scroll displacement leaves insufficient overlap";
        return result;
    }
    std::sort(candidates.begin(), candidates.end(), [expected = result.expectedShift](const Candidate& left, const Candidate& right) {
        if (std::fabs(left.score - right.score) > 0.0001F) return left.score > right.score;
        if (expected > 0) return std::abs(left.shift - expected) < std::abs(right.shift - expected);
        return left.shift < right.shift;
    });

    // A duplicate frame is useful information to the session even though it
    // is not a stitchable movement.
    const Candidate duplicate{0, scoreBand(previous, current, 0, marginX, 0,
                                           comparisonWidth, height), 0.0F};
    if (duplicate.score >= 0.985F) {
        result.duplicate = true;
        result.confidence = duplicate.score;
        result.reason = "frames are duplicates";
        return result;
    }

    const Candidate best = candidates.front();
    const float second = candidates.size() > 1 ? candidates[1].score : 0.0F;
    result.shift = best.shift;
    result.overlap = height - best.shift;
    result.confidence = best.score;
    result.margin = best.score - second;
    if (best.score < options.minimumConfidence) {
        result.reason = "visual overlap confidence is too low";
        return result;
    }
    if (result.margin < options.minimumMargin && candidates.size() > 1) {
        result.reason = "visual overlap is ambiguous";
        return result;
    }
    if (result.expectedShift > 0) {
        const int tolerance = std::max(options.minimumPriorTolerance,
                                       static_cast<int>(std::lround(result.expectedShift * options.priorToleranceRatio)));
        if (std::abs(result.shift - result.expectedShift) > tolerance) {
            result.reason = "visual displacement disagrees with scrollbar movement";
            return result;
        }
    }
    result.accepted = true;
    return result;
}

} // namespace universal_stitcher
