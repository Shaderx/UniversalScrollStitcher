#include "universal_stitcher/ScrollbarDetector.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace universal_stitcher {
namespace {

struct RunResult {
    int top = 0;
    int bottom = 0;
    float confidence = 0.0F;
};

float luma(const cv::Vec4b& pixel) {
    return 0.114F * static_cast<float>(pixel[0]) +
           0.587F * static_cast<float>(pixel[1]) +
           0.299F * static_cast<float>(pixel[2]);
}

float median(std::vector<float> values) {
    if (values.empty()) return 0.0F;
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

RunResult findRun(const cv::Mat& bgra, const Rect& requested) {
    const Rect track = requested.clampTo(bgra.cols, bgra.rows);
    if (!track.valid()) return {};

    std::vector<float> rows(static_cast<std::size_t>(track.height), 0.0F);
    std::vector<float> deviations(rows.size(), 0.0F);
    for (int y = 0; y < track.height; ++y) {
        float sum = 0.0F;
        const auto* row = bgra.ptr<cv::Vec4b>(track.y + y);
        for (int x = track.x; x < track.right(); ++x) sum += luma(row[x]);
        rows[static_cast<std::size_t>(y)] = sum / static_cast<float>(track.width);
    }

    std::vector<float> smoothed(rows.size(), 0.0F);
    for (int y = 0; y < track.height; ++y) {
        const int first = std::max(0, y - 2);
        const int last = std::min(track.height - 1, y + 2);
        float sum = 0.0F;
        for (int i = first; i <= last; ++i) sum += rows[static_cast<std::size_t>(i)];
        smoothed[static_cast<std::size_t>(y)] = sum / static_cast<float>(last - first + 1);
    }

    const float baseline = median(smoothed);
    for (std::size_t i = 0; i < smoothed.size(); ++i) {
        deviations[i] = std::fabs(smoothed[i] - baseline);
    }

    std::vector<float> sorted = deviations;
    const float highPercentile = [&] {
        if (sorted.empty()) return 0.0F;
        const auto index = static_cast<std::size_t>(std::floor(sorted.size() * 0.75));
        std::nth_element(sorted.begin(), sorted.begin() + std::min(index, sorted.size() - 1), sorted.end());
        return sorted[std::min(index, sorted.size() - 1)];
    }();
    const float threshold = std::max(7.0F, highPercentile * 0.55F);
    const int minimumRun = std::max(4, std::min(24, track.height / 100));

    RunResult best;
    int runStart = -1;
    for (int i = 0; i <= track.height; ++i) {
        const bool active = i < track.height && deviations[static_cast<std::size_t>(i)] >= threshold;
        if (active && runStart < 0) runStart = i;
        if ((!active || i == track.height) && runStart >= 0) {
            const int runEnd = i;
            runStart = std::exchange(runStart, -1);
            const int length = runEnd - runStart;
            if (length >= minimumRun && length < static_cast<int>(track.height * 0.80F)) {
                float peak = 0.0F;
                for (int row = runStart; row < runEnd; ++row) peak = std::max(peak, deviations[static_cast<std::size_t>(row)]);
                const float compactness = 1.0F - std::fabs(static_cast<float>(length) - track.height * 0.12F) / std::max(1.0F, track.height * 0.88F);
                const float score = peak * (0.65F + 0.35F * std::max(0.0F, compactness));
                if (score > best.confidence) {
                    best = {runStart + track.y, runEnd + track.y,
                            std::clamp(score / 64.0F, 0.0F, 1.0F)};
                }
            }
        }
    }
    return best;
}

} // namespace

std::optional<ScrollbarConfig> ScrollbarDetector::autoDetect(const cv::Mat& bgra) {
    if (bgra.empty() || bgra.type() != CV_8UC4 || bgra.cols < 32 || bgra.rows < 64) return std::nullopt;

    const int candidateWidth = std::clamp(static_cast<int>(std::round(bgra.cols * 0.025)), 8, 28);
    const int searchWidth = std::max(candidateWidth + 2, static_cast<int>(std::round(bgra.cols * 0.12)));
    RunResult best;
    Rect bestTrack;
    ScrollbarSide bestSide = ScrollbarSide::Right;

    for (int x = 0; x <= searchWidth - candidateWidth; ++x) {
        const Rect candidate{x, 0, candidateWidth, bgra.rows};
        const RunResult result = findRun(bgra, candidate);
        if (result.confidence > best.confidence) {
            best = result;
            bestTrack = candidate;
            bestSide = ScrollbarSide::Left;
        }
    }
    for (int x = bgra.cols - searchWidth; x <= bgra.cols - candidateWidth; ++x) {
        const Rect candidate{x, 0, candidateWidth, bgra.rows};
        const RunResult result = findRun(bgra, candidate);
        if (result.confidence > best.confidence) {
            best = result;
            bestTrack = candidate;
            bestSide = ScrollbarSide::Right;
        }
    }

    if (!bestTrack.valid() || best.confidence < 0.18F) return std::nullopt;
    return ScrollbarConfig{bestTrack, bestSide, true};
}

ScrollbarObservation ScrollbarDetector::detect(const cv::Mat& bgra, const ScrollbarConfig& config) {
    if (!config.enabled || bgra.empty() || bgra.type() != CV_8UC4) return {};
    const Rect track = config.track.clampTo(bgra.cols, bgra.rows);
    if (!track.valid()) return {};
    const RunResult run = findRun(bgra, track);
    if (run.bottom <= run.top) return {};
    return {true, {track.x, run.top, track.width, run.bottom - run.top}, run.confidence};
}

int ScrollbarState::expectedContentShift(int viewportHeight) const noexcept {
    if (!valid || delta <= 0 || viewportHeight <= 0) return 0;
    const float geometricScale = thumbHeight > 0
        ? static_cast<float>(std::max(1, viewportHeight)) / static_cast<float>(thumbHeight)
        : 1.0F;
    const float scale = learnedContentPerThumbPixel > 0.0F ? learnedContentPerThumbPixel : geometricScale;
    const float estimated = static_cast<float>(delta) * std::max(1.0F, scale);
    return std::clamp(static_cast<int>(std::lround(estimated)), 1, std::max(1, viewportHeight - 1));
}

} // namespace universal_stitcher
