#include "universal_stitcher/StitchSession.h"

#include <algorithm>
#include <cmath>

namespace universal_stitcher {

bool StitchSession::start(const cv::Mat& firstFrame, const StitchOptions& options) {
    reset();
    if (firstFrame.empty() || firstFrame.type() != CV_8UC4 || !options.viewport.valid()) {
        state_ = SessionState::Failed;
        lastMessage_ = "first frame or viewport is invalid";
        return false;
    }
    options_ = options;
    options_.viewport = options.viewport.clampTo(firstFrame.cols, firstFrame.rows);
    if (!options_.viewport.valid() || !store_.open(options_.viewport.width)) {
        state_ = SessionState::Failed;
        lastMessage_ = "unable to initialize strip storage";
        return false;
    }
    pending_ = firstFrame(cv::Rect(options_.viewport.x, options_.viewport.y,
                                   options_.viewport.width, options_.viewport.height)).clone();
    if (pending_.empty()) {
        state_ = SessionState::Failed;
        lastMessage_ = "unable to crop the selected viewport";
        return false;
    }
    const ScrollbarObservation initial = ScrollbarDetector::detect(firstFrame, options_.scrollbar);
    if (initial.detected) {
        scrollbar_.valid = true;
        scrollbar_.top = initial.thumbTop();
        scrollbar_.previousTop = scrollbar_.top;
        scrollbar_.thumbHeight = initial.thumb.height;
        scrollbar_.trackTop = options_.scrollbar.track.y;
        scrollbar_.trackBottom = options_.scrollbar.track.bottom();
        scrollbar_.confidence = initial.confidence;
        scrollbar_.atTop = scrollbar_.top <= scrollbar_.trackTop + 2;
        scrollbar_.atBottom = initial.thumbBottom() >= scrollbar_.trackBottom - 2;
        if (!scrollbar_.atTop) {
            state_ = SessionState::Failed;
            lastMessage_ = "return the target to the top before starting a full capture";
            store_.close(true);
            pending_.release();
            return false;
        }
    }
    state_ = SessionState::Capturing;
    lastMessage_ = initial.detected ? "capturing; scroll down manually" :
                                     "capturing; waiting for scrollbar detection";
    return true;
}

StitchUpdate StitchSession::process(const cv::Mat& frame) {
    StitchUpdate update;
    if (state_ != SessionState::Capturing) {
        update.paused = state_ == SessionState::Paused;
        update.message = lastMessage_;
        return update;
    }
    if (frame.empty() || frame.type() != CV_8UC4 || frame.cols < options_.viewport.right() ||
        frame.rows < options_.viewport.bottom()) {
        state_ = SessionState::Paused;
        lastMessage_ = "capture size changed; recalibrate and start a new session";
        update.paused = true;
        update.message = lastMessage_;
        return update;
    }

    const ScrollbarObservation observation = ScrollbarDetector::detect(frame, options_.scrollbar);
    update.scrollbarConfidence = observation.confidence;
    if (!observation.detected) {
        state_ = SessionState::Paused;
        lastMessage_ = "scrollbar not detected; adjust the track rectangle and start again";
        update.paused = true;
        update.message = lastMessage_;
        return update;
    }

    if (!scrollbar_.valid) {
        scrollbar_.valid = true;
        scrollbar_.top = observation.thumbTop();
        scrollbar_.previousTop = scrollbar_.top;
        scrollbar_.thumbHeight = observation.thumb.height;
        scrollbar_.trackTop = options_.scrollbar.track.y;
        scrollbar_.trackBottom = options_.scrollbar.track.bottom();
        scrollbar_.confidence = observation.confidence;
        lastMessage_ = "scrollbar calibrated; scroll down manually";
        update.message = lastMessage_;
        return update;
    }

    const int thumbDelta = observation.thumbTop() - scrollbar_.top;
    update.atTop = observation.thumbTop() <= options_.scrollbar.track.y + 2;
    update.atBottom = observation.thumbBottom() >= options_.scrollbar.track.bottom() - 2;
    if (thumbDelta == 0) {
        update.message = "waiting for manual scroll";
        lastMessage_ = update.message;
        return update;
    }
    if (thumbDelta < 0) {
        state_ = SessionState::Paused;
        lastMessage_ = "upward scrolling is not supported; start again at the top";
        update.paused = true;
        update.message = lastMessage_;
        return update;
    }

    scrollbar_.delta = thumbDelta;
    scrollbar_.thumbHeight = observation.thumb.height;
    scrollbar_.trackTop = options_.scrollbar.track.y;
    scrollbar_.trackBottom = options_.scrollbar.track.bottom();
    const int expected = scrollbar_.expectedContentShift(pending_.rows);
    update.expectedShift = expected;
    const cv::Mat current = frame(cv::Rect(options_.viewport.x, options_.viewport.y,
                                           options_.viewport.width, options_.viewport.height)).clone();
    const ShiftEstimate estimate = VerticalShiftEstimator::estimate(
        pending_, current, expected, options_.estimator);
    update.shift = estimate.shift;
    update.confidence = estimate.confidence;
    if (!estimate.accepted) {
        state_ = SessionState::Paused;
        lastMessage_ = "scrollbar moved but visual overlap was rejected: " + estimate.reason;
        update.paused = true;
        update.message = lastMessage_;
        return update;
    }

    const Seam seam = SeamFinder::find(pending_, current, estimate.shift);
    const int commitEnd = estimate.shift + seam.currentRow;
    if (!seam.valid || commitEnd <= pendingStart_ || commitEnd > pending_.rows) {
        state_ = SessionState::Paused;
        lastMessage_ = "visual seam made no forward progress; scroll more slowly and start again";
        update.paused = true;
        update.message = lastMessage_;
        return update;
    }
    if (!store_.append(pending_, pendingStart_, commitEnd)) {
        state_ = SessionState::Failed;
        lastMessage_ = "unable to write the temporary strip store";
        update.paused = true;
        update.message = lastMessage_;
        return update;
    }

    outputRows_ += commitEnd - pendingStart_;
    pending_ = current;
    pendingStart_ = seam.currentRow;
    scrollbar_.previousTop = scrollbar_.top;
    scrollbar_.top = observation.thumbTop();
    scrollbar_.confidence = observation.confidence;
    scrollbar_.atTop = update.atTop;
    scrollbar_.atBottom = update.atBottom;
    if (thumbDelta > 0 && estimate.shift > 0) {
        const float observedScale = static_cast<float>(estimate.shift) / static_cast<float>(thumbDelta);
        scrollbar_.learnedContentPerThumbPixel = scrollbar_.learnedContentPerThumbPixel <= 0.0F
            ? observedScale
            : scrollbar_.learnedContentPerThumbPixel * 0.75F + observedScale * 0.25F;
    }
    ++acceptedFrames_;
    update.accepted = true;
    lastMessage_ = update.atBottom ? "bottom detected; stop to finalize" : "stitched; continue scrolling down";
    update.message = lastMessage_;
    return update;
}

bool StitchSession::finish() {
    if (state_ != SessionState::Capturing && state_ != SessionState::Paused) return false;
    if (pending_.empty() || pendingStart_ < 0 || pendingStart_ >= pending_.rows) {
        state_ = SessionState::Failed;
        lastMessage_ = "there is no captured image to finalize";
        return false;
    }
    if (!store_.append(pending_, pendingStart_, pending_.rows)) {
        state_ = SessionState::Failed;
        lastMessage_ = "unable to commit the final strip";
        return false;
    }
    outputRows_ += pending_.rows - pendingStart_;
    // Keep the strip store open for streaming export. Materializing the
    // complete result here made long captures consume hundreds of megabytes
    // (or more) before the encoder even started.
    finalImage_.release();
    if (store_.rows() == 0 || store_.rows() != static_cast<std::uint64_t>(outputRows_)) {
        state_ = SessionState::Failed;
        lastMessage_ = "unable to validate the assembled strip store";
        store_.close(true);
        return false;
    }
    state_ = SessionState::Finalized;
    lastMessage_ = "capture finalized";
    return true;
}

void StitchSession::reset() {
    store_.close(true);
    options_ = {};
    scrollbar_ = {};
    pending_.release();
    finalImage_.release();
    pendingStart_ = 0;
    acceptedFrames_ = 0;
    outputRows_ = 0;
    state_ = SessionState::Idle;
    lastMessage_.clear();
}

} // namespace universal_stitcher
