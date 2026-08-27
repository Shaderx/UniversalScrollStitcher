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
    const Rect selectedViewport = options.viewport.clampTo(firstFrame.cols, firstFrame.rows);
    options_.viewport = selectedViewport;
    int staticHeaderRows = 0;
    if (options_.maskStaticOutsideScrollbar && options_.scrollbar.enabled &&
        options_.scrollbar.track.valid()) {
        const Rect track = options_.scrollbar.track.clampTo(firstFrame.cols, firstFrame.rows);
        const int movingTop = std::max(options_.viewport.y, track.y);
        const int movingBottom = std::min(options_.viewport.bottom(), track.bottom());
        const int movingHeight = movingBottom - movingTop;
        // A scrollbar track describes the vertical travel of the moving
        // content. Restricting registration and output to that band masks
        // fixed game chrome above and below a centered scrolling panel. Keep
        // a conservative lower bound so a bad tiny manual track cannot erase
        // an otherwise usable viewport.
        const int minimumUsefulHeight = std::max(32, options_.viewport.height / 5);
        if (movingHeight >= minimumUsefulHeight) {
            staticHeaderRows = movingTop - selectedViewport.y;
            options_.viewport.y = movingTop;
            options_.viewport.height = movingHeight;
        }
    }
    if (!options_.viewport.valid() || !store_.open(options_.viewport.width)) {
        state_ = SessionState::Failed;
        lastMessage_ = "unable to initialize strip storage";
        return false;
    }
    // A fixed header provides useful context but must not be present in every
    // registered frame. Commit the selected rows above the scrollbar track
    // once, then stitch only the moving band below them.
    if (staticHeaderRows > 0) {
        const cv::Mat staticHeader = firstFrame(cv::Rect(
            selectedViewport.x, selectedViewport.y, selectedViewport.width, staticHeaderRows));
        if (!store_.append(staticHeader, 0, staticHeader.rows)) {
            state_ = SessionState::Failed;
            lastMessage_ = "unable to preserve the selected static header";
            return false;
        }
    }
    outputRows_ = staticHeaderRows;
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
    }
    state_ = SessionState::Capturing;
    if (!initial.detected) {
        lastMessage_ = "capturing; waiting for scrollbar detection";
    } else if (scrollbar_.atTop) {
        lastMessage_ = "capturing; scroll down manually";
    } else {
        // Starting mid-document is a legitimate request, and the track
        // rectangle may simply not line up with the real track. Report it
        // instead of refusing to capture.
        lastMessage_ = "capturing from the current position; the target does not appear "
                       "to be scrolled to the top";
    }
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
    update.scrollbarVisible = observation.detected;
    if (!observation.detected) {
        // Overlay scrollbars fade out while the pointer is idle. Hold the
        // pending frame and keep waiting; the thumb reappears on the next
        // scroll and the delta is still measured from the accepted baseline.
        // Once a thumb has been seen, its disappearance must not time out a
        // valid manual capture just because the user pauses to read.
        ++consecutiveScrollbarMisses_;
        if (!scrollbar_.valid &&
            consecutiveScrollbarMisses_ > kInitialScrollbarMissPauseThreshold) {
            state_ = SessionState::Paused;
            lastMessage_ = "scrollbar not detected; adjust the track rectangle and start again";
            update.paused = true;
            update.message = lastMessage_;
            return update;
        }
        update.message = "scrollbar not visible; waiting for it to reappear";
        lastMessage_ = update.message;
        return update;
    }
    consecutiveScrollbarMisses_ = 0;

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
    if (thumbDelta < -kThumbNoiseTolerance) {
        state_ = SessionState::Paused;
        lastMessage_ = "upward scrolling is not supported; start again at the top";
        update.paused = true;
        update.message = lastMessage_;
        return update;
    }
    if (thumbDelta <= 0) {
        // Sub-pixel thumb rendering and hover animations move the measured
        // top by a pixel on a stationary view. Treat that as no movement
        // rather than as a reversal.
        update.message = "waiting for manual scroll";
        lastMessage_ = update.message;
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
    update.margin = estimate.margin;

    // A rejected frame is not a failed session. The pending frame and the
    // accepted scrollbar baseline are both left untouched, so the overlap
    // returns as soon as the view comes back within range and the capture
    // continues with no gap. Only a sustained failure escalates to Paused.
    auto reject = [&](const std::string& reason) {
        ++consecutiveRejections_;
        update.rejected = true;
        update.consecutiveRejections = consecutiveRejections_;
        if (consecutiveRejections_ >= kRejectionPauseThreshold) {
            state_ = SessionState::Paused;
            update.paused = true;
            lastMessage_ = "visual overlap was lost and did not recover: " + reason;
        } else {
            lastMessage_ = "lost visual overlap (" + reason + "); scroll back up slightly to recover";
        }
        update.message = lastMessage_;
        return update;
    };

    if (estimate.duplicate) {
        // The thumb moved but the content did not, which happens while a
        // scrollbar animates or a view rubber-bands. Nothing to stitch.
        update.message = "waiting for manual scroll";
        lastMessage_ = update.message;
        return update;
    }
    if (!estimate.accepted) return reject(estimate.reason);

    const int minimumSeamRow = std::max(1, pendingStart_ - estimate.shift + 1);
    const Seam seam = SeamFinder::find(pending_, current, estimate.shift, 3, minimumSeamRow);
    const int commitEnd = estimate.shift + seam.currentRow;
    if (!seam.valid || commitEnd <= pendingStart_ || commitEnd > pending_.rows) {
        return reject("the seam made no forward progress");
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
    consecutiveRejections_ = 0;
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
    consecutiveRejections_ = 0;
    consecutiveScrollbarMisses_ = 0;
    state_ = SessionState::Idle;
    lastMessage_.clear();
}

} // namespace universal_stitcher
