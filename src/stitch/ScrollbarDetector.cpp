#include "universal_stitcher/ScrollbarDetector.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <vector>

namespace universal_stitcher {
namespace {

struct RunResult {
    int top = 0;
    int bottom = 0;
    float confidence = 0.0F;
    // The vertical extent of the scrollbar track itself, which is usually a
    // subrange of the searched strip. Window chrome above and below the track
    // deviates from the track baseline and bounds the expansion.
    int trackTop = 0;
    int trackBottom = 0;
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
        // Use the smoothed signal only to establish the track baseline. Use
        // unsmoothed row luminance for the run itself so the detector reports
        // the actual thumb edges instead of expanding them by the smoothing
        // kernel radius.
        deviations[i] = std::fabs(rows[i] - baseline);
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
    int bestLocalTop = -1;
    int bestLocalBottom = -1;
    int runStart = -1;
    for (int i = 0; i <= track.height; ++i) {
        const bool active = i < track.height && deviations[static_cast<std::size_t>(i)] >= threshold;
        if (active && runStart < 0) runStart = i;
        if ((!active || i == track.height) && runStart >= 0) {
            const int runEnd = i;
            // Keep the start before resetting it.  Assigning std::exchange's
            // return value back to runStart restores the old value and makes
            // the run consume every later row (the old audit bug).
            const int completedStart = runStart;
            runStart = -1;
            const int length = runEnd - completedStart;
            if (length >= minimumRun && length < static_cast<int>(track.height * 0.80F)) {
                float peak = 0.0F;
                for (int row = completedStart; row < runEnd; ++row) peak = std::max(peak, deviations[static_cast<std::size_t>(row)]);
                const float compactness = 1.0F - std::fabs(static_cast<float>(length) - track.height * 0.12F) / std::max(1.0F, track.height * 0.88F);
                const float score = peak * (0.65F + 0.35F * std::max(0.0F, compactness));
                if (score > best.confidence) {
                    best.top = completedStart + track.y;
                    best.bottom = runEnd + track.y;
                    best.confidence = std::clamp(score / 64.0F, 0.0F, 1.0F);
                    bestLocalTop = completedStart;
                    bestLocalBottom = runEnd;
                }
            }
        }
    }
    if (bestLocalTop < 0) return {};

    // Grow away from the thumb across rows that still match the track
    // baseline. Window chrome above or below the track (title bars, tool
    // strips, status bars) deviates from that baseline and stops the growth,
    // which yields the real track bounds instead of the whole searched strip.
    int extentTop = bestLocalTop;
    int extentBottom = bestLocalBottom;
    while (extentTop > 0 && deviations[static_cast<std::size_t>(extentTop - 1)] < threshold) --extentTop;
    while (extentBottom < track.height &&
           deviations[static_cast<std::size_t>(extentBottom)] < threshold) ++extentBottom;

    // A track must be able to hold the thumb with room to travel. Anything
    // smaller is more likely a misread than a genuine bound, so fall back to
    // the searched strip rather than inventing a tight, wrong track.
    const int extentHeight = extentBottom - extentTop;
    const int thumbHeight = bestLocalBottom - bestLocalTop;
    if (extentHeight < std::max(minimumRun * 2, thumbHeight + 2)) {
        best.trackTop = track.y;
        best.trackBottom = track.bottom();
        return best;
    }
    best.trackTop = extentTop + track.y;
    best.trackBottom = extentBottom + track.y;
    return best;
}

} // namespace

std::optional<ScrollbarConfig> ScrollbarDetector::autoDetect(const cv::Mat& bgra) {
    const auto candidates = autoDetectAll(bgra);
    if (candidates.empty()) return std::nullopt;
    return candidates.front().config;
}

std::vector<ScrollbarCandidate> ScrollbarDetector::autoDetectAll(const cv::Mat& bgra) {
    std::vector<ScrollbarCandidate> candidates;
    if (bgra.empty() || bgra.type() != CV_8UC4 || bgra.cols < 32 || bgra.rows < 64) return candidates;

    const int candidateWidth = std::clamp(static_cast<int>(std::round(bgra.cols * 0.025)), 8, 28);
    const int searchWidth = std::max(candidateWidth + 2, static_cast<int>(std::round(bgra.cols * 0.12)));

    auto collect = [&](int firstX, int lastX, ScrollbarSide side) {
        for (int x = firstX; x <= lastX; ++x) {
            const Rect strip{x, 0, candidateWidth, bgra.rows};
            const RunResult run = findRun(bgra, strip);
            if (run.confidence < 0.18F || run.bottom <= run.top) continue;
            // Publish the measured track extent, not the full-height search
            // strip. StitchSession derives top-of-document and end-of-document
            // state from track.y and track.bottom(), so a strip that spans the
            // title bar makes a document that is at the top look scrolled.
            const Rect track{strip.x, run.trackTop, strip.width, run.trackBottom - run.trackTop};
            if (!track.valid()) continue;
            candidates.push_back({
                {track, side, true},
                {true, {track.x, run.top, track.width, run.bottom - run.top}, run.confidence}});
        }
    };

    collect(0, searchWidth - candidateWidth, ScrollbarSide::Left);
    collect(bgra.cols - searchWidth, bgra.cols - candidateWidth, ScrollbarSide::Right);

    std::sort(candidates.begin(), candidates.end(), [](const ScrollbarCandidate& left,
                                                       const ScrollbarCandidate& right) {
        if (std::fabs(left.observation.confidence - right.observation.confidence) > 0.001F) {
            return left.observation.confidence > right.observation.confidence;
        }
        if (left.config.side != right.config.side) {
            return left.config.side == ScrollbarSide::Right;
        }
        // Saturated confidence scores are common for high-contrast thumbs.
        // Prefer the strip closest to its declared window edge so the chosen
        // cluster representative covers the complete scrollbar rather than a
        // partial overlap with neighboring content.
        return left.config.side == ScrollbarSide::Left
            ? left.config.track.x < right.config.track.x
            : left.config.track.x > right.config.track.x;
    });

    // Sliding a narrow detector across one scrollbar creates several nearly
    // identical hits. Keep only the strongest member of each horizontal
    // cluster while preserving genuinely separate scrollbars.
    std::vector<ScrollbarCandidate> distinct;
    constexpr std::size_t kMaximumCandidates = 12;
    for (const auto& candidate : candidates) {
        const int center = candidate.config.track.x + candidate.config.track.width / 2;
        const bool duplicate = std::any_of(distinct.begin(), distinct.end(), [&](const ScrollbarCandidate& existing) {
            const int existingCenter = existing.config.track.x + existing.config.track.width / 2;
            return std::abs(center - existingCenter) <= candidateWidth;
        });
        if (!duplicate) distinct.push_back(candidate);
        if (distinct.size() == kMaximumCandidates) break;
    }

    return distinct;
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
