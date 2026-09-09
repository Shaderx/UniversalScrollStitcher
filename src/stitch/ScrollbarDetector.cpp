#include "universal_stitcher/ScrollbarDetector.h"
#include "ScrollbarCandidateOrder.h"

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
    float bestScore = -1.0F;
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
                if (score > bestScore) {
                    bestScore = score;
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

float candidateSelectionScore(const cv::Mat& bgra, const ScrollbarCandidate& candidate) {
    const Rect track = candidate.config.track.clampTo(bgra.cols, bgra.rows);
    const Rect thumb = candidate.observation.thumb.clampTo(bgra.cols, bgra.rows);
    if (!track.valid() || !thumb.valid()) return 0.0F;

    double thumbBlue = 0.0;
    double thumbGreen = 0.0;
    double thumbRed = 0.0;
    std::uint64_t thumbPixels = 0;
    double backgroundLuma = 0.0;
    std::uint64_t backgroundPixels = 0;
    double horizontalVariation = 0.0;
    for (int y = track.y; y < track.bottom(); ++y) {
        const auto* row = bgra.ptr<cv::Vec4b>(y);
        float rowMinimum = 255.0F, rowMaximum = 0.0F;
        for (int x = track.x; x < track.right(); ++x) {
            const float value = luma(row[x]);
            rowMinimum = std::min(rowMinimum, value);
            rowMaximum = std::max(rowMaximum, value);
            const bool insideThumb = x >= thumb.x && x < thumb.right() &&
                                     y >= thumb.y && y < thumb.bottom();
            if (insideThumb) {
                thumbBlue += row[x][0];
                thumbGreen += row[x][1];
                thumbRed += row[x][2];
                ++thumbPixels;
            } else {
                backgroundLuma += luma(row[x]);
                ++backgroundPixels;
            }
        }
        horizontalVariation += rowMaximum - rowMinimum;
    }
    if (thumbPixels == 0 || backgroundPixels == 0) return candidate.observation.confidence;

    const float blue = static_cast<float>(thumbBlue / thumbPixels);
    const float green = static_cast<float>(thumbGreen / thumbPixels);
    const float red = static_cast<float>(thumbRed / thumbPixels);
    const float thumbLuma = 0.114F * blue + 0.587F * green + 0.299F * red;
    const float chroma = std::max({blue, green, red}) - std::min({blue, green, red});
    // The supplied game uses a neutral medium-gray thumb on a very light
    // track. Low chroma is also a useful general scrollbar cue and rejects
    // saturated list cards near the screen edge.
    const float neutrality = 1.0F - std::clamp(chroma / 96.0F, 0.0F, 1.0F);
    const float contrast = std::clamp(
        std::fabs(static_cast<float>(backgroundLuma / backgroundPixels) - thumbLuma) / 128.0F,
        0.0F, 1.0F);
    const float averageTrackLuma = static_cast<float>(backgroundLuma / backgroundPixels);
    // A pure-white strip plus a dark window shadow is a common false positive
    // at the outer frame edge. The reference scrollbar has a distinct light
    // gray track, so discount backgrounds that are effectively pure white.
    const float whiteTrackPenalty =
        std::clamp((averageTrackLuma - 248.0F) / 7.0F, 0.0F, 1.0F) * 0.08F;
    // Soft color priors from the reference UI. They improve ordering without
    // excluding different themes because contrast, geometry, and neutrality
    // still carry most of the score.
    const float trackTone = 1.0F -
        std::clamp(std::fabs(averageTrackLuma - 214.0F) / 42.0F, 0.0F, 1.0F);
    const float thumbTone = 1.0F -
        std::clamp(std::fabs(thumbLuma - 128.0F) / 80.0F, 0.0F, 1.0F);
    // Distance to the top-level window edge says nothing about whether an
    // interior pane owns a scrollbar. Rewarding it promoted frame shadows
    // above real scrollbars in wide layouts.
    const float trackCoverage = std::clamp(
        static_cast<float>(track.height) / std::max(1.0F, bgra.rows * 0.45F), 0.0F, 1.0F);
    const int insetThreshold = std::max(2, bgra.rows / 100);
    const float interiorTrack = track.y >= insetThreshold &&
                                bgra.rows - track.bottom() >= insetThreshold ? 1.0F : 0.0F;
    const float conventionalSide = candidate.config.side == ScrollbarSide::Right ? 0.05F : 0.0F;
    // A strip straddling content and a scrollbar can fake a strong vertical
    // profile. Prefer coherent cross-sections over these mixed partial hits.
    const float mixedStripPenalty = 0.15F * std::clamp(
        static_cast<float>(horizontalVariation / track.height) / 64.0F, 0.0F, 1.0F);
    return std::clamp(candidate.observation.confidence * 0.28F + neutrality * 0.15F +
                      contrast * 0.12F + trackCoverage * 0.12F +
                      trackTone * 0.10F + thumbTone * 0.08F + conventionalSide -
                      whiteTrackPenalty + interiorTrack * 0.08F - mixedStripPenalty,
                      0.0F, 1.0F);
}

void collectNeutralGrayCandidates(const cv::Mat& bgra, int candidateWidth,
                                  int firstX, int lastX, int leftSideBoundary,
                                  std::vector<ScrollbarCandidate>& candidates) {
    const int minimumThumbHeight = std::max(4, std::min(24, bgra.rows / 100));
    const int maximumThumbHeight = bgra.rows - 8;
    for (int x = firstX; x <= lastX; ++x) {
        std::vector<float> rowLuma(static_cast<std::size_t>(bgra.rows));
        std::vector<float> rowChroma(static_cast<std::size_t>(bgra.rows));
        for (int y = 0; y < bgra.rows; ++y) {
            const auto* row = bgra.ptr<cv::Vec4b>(y);
            float blue = 0.0F;
            float green = 0.0F;
            float red = 0.0F;
            for (int column = x; column < x + candidateWidth; ++column) {
                blue += row[column][0];
                green += row[column][1];
                red += row[column][2];
            }
            blue /= candidateWidth;
            green /= candidateWidth;
            red /= candidateWidth;
            rowLuma[static_cast<std::size_t>(y)] =
                0.114F * blue + 0.587F * green + 0.299F * red;
            rowChroma[static_cast<std::size_t>(y)] =
                std::max({blue, green, red}) - std::min({blue, green, red});
        }

        auto isThumbRow = [&](int y) {
            const float value = rowLuma[static_cast<std::size_t>(y)];
            return rowChroma[static_cast<std::size_t>(y)] <= 36.0F &&
                   value >= 45.0F && value <= 195.0F;
        };
        int runStart = -1;
        for (int y = 0; y <= bgra.rows; ++y) {
            const bool active = y < bgra.rows && isThumbRow(y);
            if (active && runStart < 0) runStart = y;
            if (active || runStart < 0) continue;
            const int runEnd = y;
            const int completedStart = runStart;
            runStart = -1;
            const int thumbHeight = runEnd - completedStart;
            if (thumbHeight < minimumThumbHeight || thumbHeight > maximumThumbHeight) continue;

            float thumbLuma = 0.0F;
            for (int row = completedStart; row < runEnd; ++row) {
                thumbLuma += rowLuma[static_cast<std::size_t>(row)];
            }
            thumbLuma /= thumbHeight;

            std::vector<float> nearbyTrackRows;
            const int probe = std::clamp(bgra.rows / 40, 12, 32);
            const int probeTop = std::max(0, completedStart - probe);
            const int probeBottom = std::min(bgra.rows, runEnd + probe);
            for (int row = probeTop; row < probeBottom; ++row) {
                if (row >= completedStart && row < runEnd) continue;
                const float value = rowLuma[static_cast<std::size_t>(row)];
                if (rowChroma[static_cast<std::size_t>(row)] <= 30.0F &&
                    value >= thumbLuma + 25.0F && value <= 246.0F) {
                    nearbyTrackRows.push_back(value);
                }
            }
            if (nearbyTrackRows.size() < 3) continue;
            const float trackLuma = median(std::move(nearbyTrackRows));
            const float contrast = trackLuma - thumbLuma;
            if (contrast < 28.0F) continue;

            auto isTrackRow = [&](int row) {
                return rowChroma[static_cast<std::size_t>(row)] <= 38.0F &&
                       std::fabs(rowLuma[static_cast<std::size_t>(row)] - trackLuma) <= 22.0F;
            };
            int trackTop = completedStart;
            int trackBottom = runEnd;
            while (trackTop > 0 && (isThumbRow(trackTop - 1) || isTrackRow(trackTop - 1))) --trackTop;
            while (trackBottom < bgra.rows &&
                   (isThumbRow(trackBottom) || isTrackRow(trackBottom))) ++trackBottom;
            const int trackHeight = trackBottom - trackTop;
            if (trackHeight < std::max(64, thumbHeight + 8)) continue;

            ScrollbarCandidate candidate{
                {{x, trackTop, candidateWidth, trackHeight},
                 x + candidateWidth / 2 < leftSideBoundary
                     ? ScrollbarSide::Left : ScrollbarSide::Right,
                 true},
                {true, {x, completedStart, candidateWidth, thumbHeight},
                 std::clamp(0.55F + contrast / 220.0F, 0.0F, 1.0F)}};
            candidate.autoDetectionScore = candidateSelectionScore(bgra, candidate);
            candidates.push_back(candidate);
        }
    }
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

    // Modern overlay/game scrollbars are often only 5-8 physical pixels wide.
    // A strip around 2.5% of the window averaged the supplied gray thumb into
    // adjacent colorful content and promoted unrelated bottom-edge controls.
    const int scaledWidth = std::clamp(static_cast<int>(std::round(bgra.cols * 0.012)), 5, 16);
    std::vector<int> candidateWidths{5, 8, scaledWidth};
    std::sort(candidateWidths.begin(), candidateWidths.end());
    candidateWidths.erase(std::unique(candidateWidths.begin(), candidateWidths.end()),
                          candidateWidths.end());
    const int leftSideBoundary = std::max(
        scaledWidth + 2, static_cast<int>(std::round(bgra.cols * 0.12)));

    auto collect = [&](int candidateWidth, int firstX, int lastX) {
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
            const ScrollbarSide side = x + candidateWidth / 2 < leftSideBoundary
                ? ScrollbarSide::Left : ScrollbarSide::Right;
            ScrollbarCandidate candidate{
                {track, side, true},
                {true, {track.x, run.top, track.width, run.bottom - run.top}, run.confidence}};
            candidate.autoDetectionScore = candidateSelectionScore(bgra, candidate);
            candidates.push_back(candidate);
        }
    };

    for (const int candidateWidth : candidateWidths) {
        const int lastX = bgra.cols - candidateWidth;
        collect(candidateWidth, 0, lastX);
        // Also look explicitly for a neutral gray thumb on a lighter neutral
        // track. Multiple widths preserve very thin game scrollbars even in a
        // wide top-level window where a percentage-derived strip would blend
        // the thumb into neighboring content.
        collectNeutralGrayCandidates(bgra, candidateWidth, 0, lastX,
                                     leftSideBoundary, candidates);
    }

    std::sort(candidates.begin(), candidates.end(), detail::scrollbarCandidateBefore);

    // Sliding a narrow detector across one scrollbar creates several nearly
    // identical hits. Keep only the strongest member of each horizontal
    // cluster while preserving genuinely separate scrollbars.
    std::vector<ScrollbarCandidate> distinct;
    for (const auto& candidate : candidates) {
        const int center = candidate.config.track.x + candidate.config.track.width / 2;
        const bool duplicate = std::any_of(distinct.begin(), distinct.end(), [&](const ScrollbarCandidate& existing) {
            const int existingCenter = existing.config.track.x + existing.config.track.width / 2;
            const int intersectionTop = std::max(candidate.config.track.y, existing.config.track.y);
            const int intersectionBottom = std::min(candidate.config.track.bottom(),
                                                    existing.config.track.bottom());
            const int verticalIntersection = std::max(0, intersectionBottom - intersectionTop);
            const int shorterTrack = std::min(candidate.config.track.height,
                                              existing.config.track.height);
            return std::abs(center - existingCenter) <=
                       std::max(candidate.config.track.width, existing.config.track.width) * 2 &&
                   verticalIntersection * 2 >= shorterTrack;
        });
        if (!duplicate) distinct.push_back(candidate);
    }

    return distinct;
}

ScrollbarObservation ScrollbarDetector::detect(const cv::Mat& bgra, const ScrollbarConfig& config) {
    if (!config.enabled || bgra.empty() || bgra.type() != CV_8UC4) return {};
    const Rect track = config.track.clampTo(bgra.cols, bgra.rows);
    if (!track.valid()) return {};
    // A thumb can occupy most of its track. In that case the median profile
    // becomes the thumb, so use the same locally bounded neutral proposal as
    // discovery when it explains the full supplied track.
    std::vector<ScrollbarCandidate> localCandidates;
    const cv::Mat local = bgra(cv::Rect(track.x, track.y, track.width, track.height));
    collectNeutralGrayCandidates(local, track.width, 0, 0, 0, localCandidates);
    std::sort(localCandidates.begin(), localCandidates.end(), detail::scrollbarCandidateBefore);
    for (const auto& candidate : localCandidates) {
        if (candidate.config.track.height < track.height - std::max(4, track.height / 20)) continue;
        Rect thumb = candidate.observation.thumb;
        thumb.x += track.x;
        thumb.y += track.y;
        return {true, thumb, candidate.observation.confidence};
    }
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
