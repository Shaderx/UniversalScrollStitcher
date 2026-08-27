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
    // Preserve every vertical row for exact shift estimation, but cap the
    // horizontal working set. Full-width 1920px correlation blocked the UI
    // thread for 50-250 ms per frame while providing little extra vertical
    // registration information.
    constexpr int kMaximumRegistrationWidth = 480;
    if (gray.cols > kMaximumRegistrationWidth) {
        cv::resize(gray, gray, cv::Size(kMaximumRegistrationWidth, gray.rows),
                   0.0, 0.0, cv::INTER_AREA);
    }

    cv::Mat gx, gy, magnitude, edges;
    cv::Sobel(gray, gx, CV_32F, 1, 0, 3);
    cv::Sobel(gray, gy, CV_32F, 0, 1, 3);
    cv::magnitude(gx, gy, magnitude);
    magnitude.convertTo(edges, CV_8U, 0.25);

    cv::Mat result;
    cv::addWeighted(gray, 0.72, edges, 0.28, 0.0, result);
    return result;
}

cv::Mat motionMask(const cv::Mat& previous, const cv::Mat& current) {
    cv::Mat difference;
    cv::absdiff(previous, current, difference);
    cv::Mat mask;
    cv::threshold(difference, mask, 3.0, 255.0, cv::THRESH_BINARY);
    const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel);
    cv::dilate(mask, mask, kernel);
    const int minimumActive = std::max(64, static_cast<int>(mask.total() / 100));
    if (cv::countNonZero(mask) < minimumActive) return {};
    return mask;
}

float scoreBand(const cv::Mat& previous, const cv::Mat& current, int shift,
                const cv::Mat& activity, int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return -1.0F;
    const cv::Mat oldRoi = previous(cv::Rect(x, y + shift, width, height));
    const cv::Mat newRoi = current(cv::Rect(x, y, width, height));
    cv::Mat difference;
    cv::absdiff(oldRoi, newRoi, difference);
    double meanDifference = 0.0;
    if (!activity.empty()) {
        const cv::Mat oldActivity = activity(cv::Rect(x, y + shift, width, height));
        const cv::Mat newActivity = activity(cv::Rect(x, y, width, height));
        cv::Mat combinedActivity;
        cv::bitwise_and(oldActivity, newActivity, combinedActivity);
        const int activePixels = cv::countNonZero(combinedActivity);
        const int minimumActive = std::max(32, width * height / 100);
        if (activePixels < minimumActive) return -1.0F;
        meanDifference = cv::mean(difference, combinedActivity)[0];
    } else {
        meanDifference = cv::mean(difference)[0];
    }
    return std::clamp(1.0F - static_cast<float>(meanDifference / 255.0), 0.0F, 1.0F);
}

float scoreShift(const cv::Mat& previous, const cv::Mat& current, int shift,
                 const cv::Mat& activity, int marginX, int comparisonWidth,
                 int height, float minimumOverlapRatio) {
    const int overlap = height - shift;
    if (overlap < static_cast<int>(height * minimumOverlapRatio)) return -1.0F;

    const int bandHeight = std::max(8, overlap / 4);
    float scoreSum = 0.0F;
    float minimum = 1.0F;
    int bandCount = 0;
    for (int band = 0; band < 4; ++band) {
        const int y = band == 3 ? std::max(0, overlap - bandHeight) : band * overlap / 4;
        const int availableHeight = std::min(bandHeight, overlap - y);
        const float score = scoreBand(previous, current, shift, activity, marginX, y,
                                      comparisonWidth, availableHeight);
        if (score < 0.0F) continue;
        scoreSum += score;
        minimum = std::min(minimum, score);
        ++bandCount;
    }
    if (bandCount == 0) return -1.0F;
    const float mean = scoreSum / bandCount;
    return mean * 0.72F + minimum * 0.28F;
}

std::vector<Candidate> collectCandidates(const cv::Mat& previous, const cv::Mat& current,
                                         int minimumShift, int maximumShift, int marginX,
                                         int comparisonWidth, int height, const cv::Mat& activity,
                                         float minimumOverlapRatio, int preferredShift = 0) {
    std::vector<Candidate> candidates;
    if (maximumShift < minimumShift) return candidates;

    // Coarse stride keeps broad mouse-wheel searches responsive. Always also
    // probe the scrollbar prior so a better distant alias cannot hide the
    // true nearby shift on the coarse grid.
    const int range = maximumShift - minimumShift;
    const int stride = range > 120 ? 4 : (range > 64 ? 2 : 1);
    candidates.reserve(static_cast<std::size_t>(range / stride + 16));

    auto consider = [&](int shift) {
        if (shift < minimumShift || shift > maximumShift) return;
        const float score = scoreShift(previous, current, shift, activity, marginX, comparisonWidth,
                                       height, minimumOverlapRatio);
        if (score < 0.0F) return;
        candidates.push_back({shift, score, score});
    };

    for (int shift = minimumShift; shift <= maximumShift; shift += stride) consider(shift);
    if (preferredShift > 0) {
        for (int shift = preferredShift - stride; shift <= preferredShift + stride; ++shift) {
            consider(shift);
        }
    }
    if (stride == 1 || candidates.empty()) return candidates;

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.score > right.score;
    });
    // Deduplicate identical coarse hits before refining.
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                 [](const Candidate& left, const Candidate& right) {
                                     return left.shift == right.shift;
                                 }),
                     candidates.end());
    const int refineCount = std::min<int>(4, static_cast<int>(candidates.size()));
    std::vector<Candidate> refined;
    refined.reserve(static_cast<std::size_t>(refineCount * (stride * 2 + 1) + 8));
    auto refineAround = [&](int center) {
        for (int shift = std::max(minimumShift, center - stride);
             shift <= std::min(maximumShift, center + stride); ++shift) {
            const float score = scoreShift(previous, current, shift, activity, marginX, comparisonWidth,
                                           height, minimumOverlapRatio);
            if (score < 0.0F) continue;
            refined.push_back({shift, score, score});
        }
    };
    for (int index = 0; index < refineCount; ++index) {
        refineAround(candidates[static_cast<std::size_t>(index)].shift);
    }
    if (preferredShift > 0) refineAround(preferredShift);
    return refined;
}

ShiftEstimate finalizeEstimate(std::vector<Candidate> candidates, int expectedShift,
                               const ShiftEstimatorOptions& options, bool enforcePrior) {
    ShiftEstimate result;
    result.expectedShift = expectedShift;
    if (candidates.empty()) {
        result.reason = "scroll displacement leaves insufficient overlap";
        return result;
    }
    std::sort(candidates.begin(), candidates.end(), [expectedShift](const Candidate& left,
                                                                    const Candidate& right) {
        if (std::fabs(left.score - right.score) > 0.0001F) return left.score > right.score;
        if (expectedShift > 0) return std::abs(left.shift - expectedShift) < std::abs(right.shift - expectedShift);
        return left.shift < right.shift;
    });

    Candidate best = candidates.front();
    // When several shifts score almost equally, prefer the one nearest the
    // scrollbar prior. Distant aliases of repeating chrome otherwise win.
    if (expectedShift > 0) {
        const float topScore = best.score;
        Candidate nearest = best;
        for (const Candidate& candidate : candidates) {
            if (topScore - candidate.score > 0.02F) break;
            if (std::abs(candidate.shift - expectedShift) < std::abs(nearest.shift - expectedShift)) {
                nearest = candidate;
            }
        }
        best = nearest;
    }

    float adjacentSecond = 0.0F;
    bool hasAdjacentCompetitor = false;
    for (const Candidate& candidate : candidates) {
        if (candidate.shift == best.shift) continue;
        adjacentSecond = candidate.score;
        hasAdjacentCompetitor = true;
        break;
    }

    // Adjacent integer shifts describe the same correlation peak. They can be
    // ignored only when the high-confidence peak also closely agrees with the
    // scrollbar; broad visual-only searches retain the stricter ambiguity
    // check so a distant alias cannot become a false stitch.
    constexpr int kSamePeakRadius = 2;
    float distantSecond = 0.0F;
    bool hasDistantCompetitor = false;
    for (const Candidate& candidate : candidates) {
        if (std::abs(candidate.shift - best.shift) <= kSamePeakRadius) continue;
        distantSecond = candidate.score;
        hasDistantCompetitor = true;
        break;
    }
    result.shift = best.shift;
    result.confidence = best.score;
    if (best.score < options.minimumConfidence) {
        result.reason = "visual overlap confidence is too low";
        return result;
    }
    const int strongPriorTolerance = expectedShift > 0
        ? std::max(3, static_cast<int>(std::lround(expectedShift * 0.20F)))
        : 0;
    const bool strongPriorAgreement = enforcePrior && expectedShift > 0 &&
        best.score >= 0.94F && std::abs(best.shift - expectedShift) <= strongPriorTolerance;
    const float second = strongPriorAgreement ? distantSecond : adjacentSecond;
    const bool hasCompetingPeak = strongPriorAgreement
        ? hasDistantCompetitor : hasAdjacentCompetitor;
    result.margin = best.score - second;
    if (result.margin < options.minimumMargin && hasCompetingPeak && !strongPriorAgreement) {
        result.reason = "visual overlap is ambiguous";
        return result;
    }
    if (enforcePrior && expectedShift > 0) {
        const int tolerance = std::max(options.minimumPriorTolerance,
                                       static_cast<int>(std::lround(expectedShift * options.priorToleranceRatio)));
        if (std::abs(result.shift - expectedShift) > tolerance) {
            result.reason = "visual displacement disagrees with scrollbar movement";
            return result;
        }
    }
    result.accepted = true;
    return result;
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

    const int inputWidth = previousBgra.cols;
    const int height = previousBgra.rows;
    if (inputWidth < 16 || height < 32) {
        result.reason = "viewport is too small for registration";
        return result;
    }
    result.expectedShift = std::clamp(expectedShift, 0, height - 1);

    const cv::Mat previous = registrationImage(previousBgra);
    const cv::Mat current = registrationImage(currentBgra);
    const cv::Mat activity = motionMask(previous, current);
    const int width = previous.cols;
    const int marginX = std::clamp(width / 20, 2, width / 4);
    const int comparisonWidth = width - marginX * 2;
    // Mouse-wheel notches often move most of a viewport between samples.
    // Keep a thin overlap band so registration still has something to lock onto.
    const int broadMaximum = std::max(1, static_cast<int>(height * (1.0F - options.minimumOverlapRatio)));

    const Candidate duplicate{0, scoreBand(previous, current, 0, {}, marginX, 0,
                                           comparisonWidth, height), 0.0F};
    if (duplicate.score >= 0.985F && activity.empty()) {
        result.duplicate = true;
        result.confidence = duplicate.score;
        result.reason = "frames are duplicates";
        return result;
    }

    auto runSearch = [&](int minimumShift, int maximumShift, bool enforcePrior) {
        auto candidates = collectCandidates(previous, current, minimumShift, maximumShift,
                                            marginX, comparisonWidth, height, activity,
                                            options.minimumOverlapRatio, result.expectedShift);
        ShiftEstimate estimate = finalizeEstimate(std::move(candidates), result.expectedShift,
                                                  options, enforcePrior);
        if (estimate.accepted) estimate.overlap = height - estimate.shift;
        return estimate;
    };

    if (result.expectedShift > 0) {
        const int tolerance = std::max(options.minimumPriorTolerance,
                                       static_cast<int>(std::lround(result.expectedShift * options.priorToleranceRatio)));
        const int minimumShift = std::max(1, result.expectedShift - tolerance);
        const int maximumShift = std::min(broadMaximum, result.expectedShift + tolerance);
        ShiftEstimate prior = runSearch(minimumShift, maximumShift, true);
        if (prior.accepted) return prior;
        // Wheel acceleration and delayed thumb motion often put the true shift
        // outside the scrollbar prior. Fall back to a full-range visual search
        // with a stricter ambiguity bar so periodic UI chrome cannot invent a
        // false lock far from the prior.
        ShiftEstimatorOptions broadOptions = options;
        broadOptions.minimumMargin = std::max(options.minimumMargin, 0.035F);
        broadOptions.minimumConfidence = std::max(options.minimumConfidence, 0.86F);
        auto candidates = collectCandidates(previous, current, 1, broadMaximum,
                                            marginX, comparisonWidth, height, activity,
                                            options.minimumOverlapRatio, result.expectedShift);
        ShiftEstimate broad = finalizeEstimate(std::move(candidates), result.expectedShift,
                                               broadOptions, false);
        if (broad.accepted) {
            broad.overlap = height - broad.shift;
            return broad;
        }
        if (broad.reason.empty()) broad.reason = prior.reason.empty()
            ? "scroll displacement leaves insufficient overlap" : prior.reason;
        // Prefer the more specific prior failure when the broad search also fails.
        if (!prior.reason.empty() && broad.reason.find("ambiguous") == std::string::npos &&
            broad.reason.find("confidence") == std::string::npos) {
            return prior;
        }
        return broad;
    }

    ShiftEstimate broad = runSearch(1, broadMaximum, false);
    if (!broad.accepted && broad.reason.empty()) {
        broad.reason = "scroll displacement leaves insufficient overlap";
    }
    return broad;
}

} // namespace universal_stitcher
