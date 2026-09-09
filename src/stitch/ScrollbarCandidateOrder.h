#pragma once

#include "universal_stitcher/ScrollbarDetector.h"

#include <cmath>
#include <limits>
#include <tuple>

namespace universal_stitcher::detail {

// Scores order evidence, not calibrated probabilities. Never compare with an
// epsilon here: approximate equality is not transitive and breaks std::sort.
inline bool scrollbarCandidateBefore(const ScrollbarCandidate& left,
                                     const ScrollbarCandidate& right) {
    const auto score = [](float value) {
        return std::isfinite(value) ? value : -std::numeric_limits<float>::infinity();
    };
    const float leftScore = score(left.autoDetectionScore), rightScore = score(right.autoDetectionScore);
    if (leftScore != rightScore) return leftScore > rightScore;
    const float leftEvidence = score(left.observation.confidence), rightEvidence = score(right.observation.confidence);
    if (leftEvidence != rightEvidence) return leftEvidence > rightEvidence;
    if (left.config.side != right.config.side) return left.config.side == ScrollbarSide::Right;
    if (left.config.track.x != right.config.track.x) {
        return left.config.side == ScrollbarSide::Left
            ? left.config.track.x < right.config.track.x
            : left.config.track.x > right.config.track.x;
    }
    const auto geometry = [](const ScrollbarCandidate& candidate) {
        const auto& track = candidate.config.track;
        const auto& thumb = candidate.observation.thumb;
        return std::tuple(track.y, track.width, track.height,
                          thumb.x, thumb.y, thumb.width, thumb.height,
                          candidate.config.enabled, candidate.observation.detected);
    };
    return geometry(left) < geometry(right);
}

} // namespace universal_stitcher::detail
