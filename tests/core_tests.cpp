#include "universal_stitcher/ScrollbarDetector.h"
#include "universal_stitcher/SeamFinder.h"
#include "universal_stitcher/StitchSession.h"
#include "universal_stitcher/VerticalShiftEstimator.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

using namespace universal_stitcher;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

cv::Mat documentFrame(int firstRow, int width = 96, int height = 96) {
    cv::Mat image(height, width, CV_8UC4);
    for (int y = 0; y < height; ++y) {
        const int documentY = firstRow + y;
        for (int x = 0; x < width; ++x) {
            auto& pixel = image.at<cv::Vec4b>(y, x);
            pixel[0] = static_cast<unsigned char>((documentY * 17 + x * 3) & 0xff);
            pixel[1] = static_cast<unsigned char>((documentY * 43 + x * 5 + (x / 7) * 19) & 0xff);
            pixel[2] = static_cast<unsigned char>((documentY * 71 + x * 11 + (documentY / 9) * 23) & 0xff);
            pixel[3] = 255;
        }
    }
    return image;
}

cv::Mat fullFrame(int documentOffset, int thumbTop, int width = 120, int height = 96) {
    cv::Mat frame(height, width, CV_8UC4, cv::Scalar(25, 25, 25, 255));
    documentFrame(documentOffset, 96, height).copyTo(frame(cv::Rect(0, 0, 96, height)));
    for (int y = 0; y < height; ++y) {
        for (int x = 112; x < 120; ++x) frame.at<cv::Vec4b>(y, x) = cv::Vec4b(210, 210, 210, 255);
    }
    for (int y = thumbTop; y < thumbTop + 18 && y < height; ++y) {
        for (int x = 112; x < 120; ++x) frame.at<cv::Vec4b>(y, x) = cv::Vec4b(35, 35, 35, 255);
    }
    return frame;
}

// A frame whose scrollbar track starts below a band of window chrome, with the
// thumb parked away from that boundary.
cv::Mat chromeFrame(int thumbTop) {
    constexpr int width = 160;
    constexpr int height = 120;
    constexpr int chromeHeight = 25;
    constexpr int trackLeft = 152;
    cv::Mat frame(height, width, CV_8UC4, cv::Scalar(25, 25, 25, 255));
    documentFrame(0, trackLeft, height).copyTo(frame(cv::Rect(0, 0, trackLeft, height)));
    for (int y = 0; y < chromeHeight; ++y) {
        for (int x = 0; x < width; ++x) frame.at<cv::Vec4b>(y, x) = cv::Vec4b(90, 90, 90, 255);
    }
    for (int y = chromeHeight; y < height; ++y) {
        for (int x = trackLeft; x < width; ++x) frame.at<cv::Vec4b>(y, x) = cv::Vec4b(210, 210, 210, 255);
    }
    for (int y = thumbTop; y < thumbTop + 25 && y < height; ++y) {
        for (int x = trackLeft; x < width; ++x) frame.at<cv::Vec4b>(y, x) = cv::Vec4b(35, 35, 35, 255);
    }
    return frame;
}

// A frame whose scrollbar track is present but whose thumb has faded out, as
// overlay scrollbars do while the pointer is idle.
cv::Mat thumblessFrame(int documentOffset) {
    cv::Mat frame = fullFrame(documentOffset, 0);
    for (int y = 0; y < frame.rows; ++y) {
        for (int x = 112; x < 120; ++x) frame.at<cv::Vec4b>(y, x) = cv::Vec4b(210, 210, 210, 255);
    }
    return frame;
}

cv::Mat multipleScrollbarFrame() {
    constexpr int width = 160;
    constexpr int height = 120;
    cv::Mat frame(height, width, CV_8UC4, cv::Scalar(25, 25, 25, 255));
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < 8; ++x) frame.at<cv::Vec4b>(y, x) = cv::Vec4b(210, 210, 210, 255);
        for (int x = width - 8; x < width; ++x) frame.at<cv::Vec4b>(y, x) = cv::Vec4b(205, 205, 205, 255);
    }
    for (int y = 12; y < 34; ++y) {
        for (int x = 0; x < 8; ++x) frame.at<cv::Vec4b>(y, x) = cv::Vec4b(35, 35, 35, 255);
    }
    for (int y = 64; y < 88; ++y) {
        for (int x = width - 8; x < width; ++x) frame.at<cv::Vec4b>(y, x) = cv::Vec4b(45, 45, 45, 255);
    }
    return frame;
}

void testShiftAndSeam() {
    const int shift = 13;
    const cv::Mat previous = documentFrame(0);
    const cv::Mat current = documentFrame(shift);
    const ShiftEstimate estimate = VerticalShiftEstimator::estimate(previous, current, shift);
    require(estimate.accepted, "known vertical shift should be accepted");
    require(estimate.shift == shift, "known vertical shift should be exact");
    require(estimate.confidence > 0.90F, "known vertical shift should have high confidence");
    const Seam seam = SeamFinder::find(previous, current, estimate.shift);
    require(seam.valid, "seam should be found inside the overlap");
    require(seam.currentRow > 0 && seam.currentRow < previous.rows - shift,
            "seam should stay inside the overlap guard band");
}

void testLargeWheelLikeShift() {
    // Hash-textured rows so a large jump has one clear match and no ramp aliases.
    constexpr int height = 120;
    constexpr int width = 96;
    constexpr int shift = 95; // ~21% overlap remaining
    auto uniqueDocument = [](int firstRow) {
        cv::Mat image(height, width, CV_8UC4);
        for (int y = 0; y < height; ++y) {
            const int documentY = firstRow + y;
            for (int x = 0; x < width; ++x) {
                const std::uint32_t hash = static_cast<std::uint32_t>(documentY) * 2246822519u +
                                          static_cast<std::uint32_t>(x) * 3266489917u;
                auto& pixel = image.at<cv::Vec4b>(y, x);
                pixel[0] = static_cast<unsigned char>(hash & 0xff);
                pixel[1] = static_cast<unsigned char>((hash >> 8) & 0xff);
                pixel[2] = static_cast<unsigned char>((hash >> 16) & 0xff);
                pixel[3] = 255;
            }
        }
        return image;
    };
    const cv::Mat previous = uniqueDocument(0);
    const cv::Mat current = uniqueDocument(shift);
    // Misleading scrollbar prior: true shift is far outside the prior window.
    const ShiftEstimate estimate = VerticalShiftEstimator::estimate(previous, current, 18);
    require(estimate.accepted, "a large but overlapping wheel jump should still stitch");
    require(estimate.shift == shift, "broad fallback should recover the true large shift");
}

void testDisagreementIsRejected() {
    const cv::Mat previous = documentFrame(0);
    cv::Mat unrelated(96, 96, CV_8UC4, cv::Scalar(7, 31, 99, 255));
    for (int y = 0; y < unrelated.rows; ++y) {
        for (int x = 0; x < unrelated.cols; ++x) {
            unrelated.at<cv::Vec4b>(y, x) = cv::Vec4b(
                static_cast<unsigned char>((x * 19 + y * 7) & 0xff),
                static_cast<unsigned char>((x * 5 + 40) & 0xff),
                static_cast<unsigned char>((y * 11 + 90) & 0xff), 255);
        }
    }
    ShiftEstimatorOptions options;
    options.minimumConfidence = 0.93F;
    const ShiftEstimate estimate = VerticalShiftEstimator::estimate(previous, unrelated, 8, options);
    require(!estimate.accepted, "unrelated frames should not be accepted as a stitch");
}

void testScrollbarDetection() {
    const cv::Mat frame = fullFrame(0, 20);
    const ScrollbarConfig config{{112, 0, 8, 96}, ScrollbarSide::Right, true};
    const ScrollbarObservation observation = ScrollbarDetector::detect(frame, config);
    require(observation.detected, "configured scrollbar should be detected");
    require(std::abs(observation.thumbTop() - 20) <= 2, "thumb top should be measured");
    require(std::abs(observation.thumb.height - 18) <= 2, "thumb height should be measured exactly");
    require(std::abs(observation.thumbBottom() - 38) <= 2, "thumb bottom should be measured exactly");
    require(observation.confidence > 0.50F, "scrollbar confidence should be useful");
    const auto automatic = ScrollbarDetector::autoDetect(frame);
    require(automatic.has_value(), "edge scrollbar should be auto-detected");

    const cv::Mat bottomFrame = fullFrame(0, 78);
    const ScrollbarObservation bottom = ScrollbarDetector::detect(bottomFrame, config);
    require(bottom.detected, "bottom scrollbar should be detected");
    require(std::abs(bottom.thumbTop() - 78) <= 2, "bottom thumb top should be measured");
    require(std::abs(bottom.thumbBottom() - 96) <= 2, "bottom thumb should terminate at track bottom");
}

void testMultipleScrollbarCandidates() {
    const auto candidates = ScrollbarDetector::autoDetectAll(multipleScrollbarFrame());
    require(candidates.size() == 2, "two distinct edge scrollbars should produce two candidates");
    const bool hasLeft = std::any_of(candidates.begin(), candidates.end(), [](const ScrollbarCandidate& candidate) {
        return candidate.config.side == ScrollbarSide::Left && candidate.config.track.x <= 2;
    });
    const bool hasRight = std::any_of(candidates.begin(), candidates.end(), [](const ScrollbarCandidate& candidate) {
        return candidate.config.side == ScrollbarSide::Right && candidate.config.track.right() >= 158;
    });
    require(hasLeft && hasRight, "candidate list should preserve both scrollbar locations");
    const auto best = ScrollbarDetector::autoDetect(multipleScrollbarFrame());
    require(best.has_value(), "legacy best-candidate API should remain available");
}

void testSessionAssembly() {
    StitchOptions options;
    options.viewport = {0, 0, 96, 96};
    options.scrollbar = {{112, 0, 8, 96}, ScrollbarSide::Right, true};
    StitchSession session;
    require(session.start(fullFrame(0, 2), options), "session should start");
    const StitchUpdate first = session.process(fullFrame(8, 6));
    require(first.accepted, "first manual scroll should be accepted");
    const StitchUpdate second = session.process(fullFrame(16, 10));
    require(second.accepted, "second manual scroll should be accepted");
    require(session.finish(), "session should finalize");
    require(session.finalImage().empty(), "finalization should not materialize a full image");
    require(session.hasOutput(), "finalized session should expose a streaming output store");
    const cv::Mat assembled = session.outputStore().readAll();
    require(assembled.rows == 112, "assembled image should have exact expected height");
    require(assembled.cols == 96, "assembled image should preserve viewport width");

    cv::Mat expected(112, 96, CV_8UC4);
    documentFrame(0, 96, 11).copyTo(expected(cv::Rect(0, 0, 96, 11)));
    documentFrame(8, 96, 96)(cv::Rect(0, 3, 96, 8)).copyTo(expected(cv::Rect(0, 11, 96, 8)));
    documentFrame(16, 96, 96)(cv::Rect(0, 3, 96, 93)).copyTo(expected(cv::Rect(0, 19, 96, 93)));
    require(cv::countNonZero(assembled.reshape(1) != expected.reshape(1)) == 0,
            "assembled pixels should exactly match the expected overlap cuts");

    std::uint64_t callbackRows = 0;
    require(session.outputStore().forEachChunk(
                [&](const std::uint8_t* bytes, int rows, int width, std::uint64_t firstRow) {
                    require(bytes != nullptr && width == 96, "stream callback should receive valid rows");
                    require(firstRow == callbackRows, "stream callback rows should be ordered");
                    callbackRows += static_cast<std::uint64_t>(rows);
                    return true;
                }), "store should stream all chunks");
    require(callbackRows == 112, "stream callback should cover the complete output");

    const StitchUpdate bad = session.process(fullFrame(30, 20));
    require(!bad.accepted && !bad.paused, "frames after finalization should be ignored");
    session.reset();
    require(session.state() == SessionState::Idle && !session.hasOutput(),
            "reset should discard finalized output and session state");
}

cv::Mat unregisterableFrame() {
    cv::Mat unrelated = fullFrame(37, 20);
    unrelated(cv::Rect(0, 0, 96, 96)).setTo(cv::Scalar(7, 31, 99, 255));
    return unrelated;
}

void testRejectedFrameKeepsSessionLive() {
    StitchOptions options;
    options.viewport = {0, 0, 96, 96};
    options.scrollbar = {{112, 0, 8, 96}, ScrollbarSide::Right, true};
    StitchSession session;
    require(session.start(fullFrame(0, 2), options), "recovery test session should start");

    const StitchUpdate rejected = session.process(unregisterableFrame());
    require(rejected.rejected && !rejected.accepted, "an unregisterable frame should be rejected");
    require(!rejected.paused && session.state() == SessionState::Capturing,
            "a single rejection should not pause the session");

    // The pending frame and the scrollbar baseline survive the rejection, so
    // the very next usable frame stitches with no gap.
    const StitchUpdate recovered = session.process(fullFrame(8, 6));
    require(recovered.accepted, "the session should stitch again after a transient rejection");
    require(recovered.shift == 8, "recovery should measure the displacement from the accepted baseline");
    require(session.finish(), "a recovered session should finalize");
    require(session.outputStore().rows() == 104,
            "recovered output should be contiguous with no dropped rows");
}

void testSustainedRejectionEventuallyPauses() {
    StitchOptions options;
    options.viewport = {0, 0, 96, 96};
    options.scrollbar = {{112, 0, 8, 96}, ScrollbarSide::Right, true};
    StitchSession session;
    require(session.start(fullFrame(0, 2), options), "pause test session should start");

    const cv::Mat unrelated = unregisterableFrame();
    bool paused = false;
    for (int attempt = 0; attempt < 200 && !paused; ++attempt) {
        paused = session.process(unrelated).paused;
    }
    require(paused, "sustained rejection should eventually pause the session");
    require(session.state() == SessionState::Paused, "session should report paused state");
    require(session.finish(), "stop should finalize the valid prefix after a pause");
    require(session.hasOutput() && session.outputStore().rows() == 96,
            "paused finalization should preserve the initial viewport");
}

void testDuplicateFrameIsNotAFailure() {
    StitchOptions options;
    options.viewport = {0, 0, 96, 96};
    options.scrollbar = {{112, 0, 8, 96}, ScrollbarSide::Right, true};
    StitchSession session;
    require(session.start(fullFrame(0, 2), options), "duplicate test session should start");

    // The thumb animates while the content stays put; that is not a stitch and
    // it is not an error either.
    const StitchUpdate duplicate = session.process(fullFrame(0, 9));
    require(!duplicate.accepted && !duplicate.rejected && !duplicate.paused,
            "a duplicate frame should be neither stitched nor treated as a failure");
    require(session.state() == SessionState::Capturing, "a duplicate frame should not end the session");
    require(session.process(fullFrame(8, 6)).accepted,
            "the session should still stitch after a duplicate frame");
}

void testSmallUpwardJitterIsTolerated() {
    StitchOptions options;
    options.viewport = {0, 0, 96, 96};
    options.scrollbar = {{112, 0, 8, 96}, ScrollbarSide::Right, true};
    StitchSession session;
    require(session.start(fullFrame(0, 6), options), "jitter test session should start");
    const StitchUpdate jitter = session.process(fullFrame(0, 5));
    require(!jitter.paused && session.state() == SessionState::Capturing,
            "a one-pixel thumb wobble should not be read as upward scrolling");
    const StitchUpdate upward = session.process(fullFrame(0, 40));
    require(!upward.paused, "a downward move after jitter should still be evaluated");
}

void testVanishedThumbIsTolerated() {
    StitchOptions options;
    options.viewport = {0, 0, 96, 96};
    options.scrollbar = {{112, 0, 8, 96}, ScrollbarSide::Right, true};
    StitchSession session;
    require(session.start(fullFrame(0, 2), options), "overlay scrollbar session should start");
    // A user may pause for an arbitrary amount of time while an overlay thumb
    // is hidden. Once calibrated, disappearance alone must not end capture.
    for (int frame = 0; frame < 100; ++frame) {
        const StitchUpdate update = session.process(thumblessFrame(0));
        require(!update.scrollbarVisible, "a faded thumb should be reported as not visible");
        require(!update.paused, "a briefly faded overlay scrollbar should not pause the session");
    }
    require(session.process(fullFrame(8, 6)).accepted,
            "capture should resume once the thumb reappears");
}

void testTrackExtentExcludesWindowChrome() {
    const auto candidates = ScrollbarDetector::autoDetectAll(chromeFrame(45));
    const ScrollbarCandidate* scrollbar = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.config.track.right() >= 158 &&
            (!scrollbar || candidate.observation.confidence > scrollbar->observation.confidence)) {
            scrollbar = &candidate;
        }
    }
    require(scrollbar != nullptr, "the right-edge scrollbar should be detected");
    // Reporting a track that starts at row 0 makes a document that is at the
    // top look scrolled, because top-of-document state is measured against
    // track.y.
    require(scrollbar->config.track.y >= 20 && scrollbar->config.track.y <= 30,
            "the detected track should start below the window chrome");
    require(scrollbar->config.track.bottom() >= 115,
            "the detected track should extend to the bottom of the scrollbar");
    require(std::abs(scrollbar->observation.thumbTop() - 45) <= 2,
            "the thumb should still be measured inside the tightened track");
}

void testTopOfDocumentIsRecognizedBelowChrome() {
    const cv::Mat frame = chromeFrame(25);
    const auto candidates = ScrollbarDetector::autoDetectAll(chromeFrame(45));
    const ScrollbarCandidate* calibration = nullptr;
    for (const auto& candidate : candidates) {
        if (candidate.config.track.right() >= 158 &&
            (!calibration || candidate.observation.confidence > calibration->observation.confidence)) {
            calibration = &candidate;
        }
    }
    require(calibration != nullptr, "calibration should produce a right-edge track");

    StitchOptions options;
    options.viewport = {0, 25, 152, 95};
    options.scrollbar = calibration->config;
    StitchSession session;
    require(session.start(frame, options),
            "a document already at the top should start with auto-detected calibration");
    require(session.state() == SessionState::Capturing, "the session should be capturing");
}

} // namespace

int main() {
    testShiftAndSeam();
    testLargeWheelLikeShift();
    testDisagreementIsRejected();
    testScrollbarDetection();
    testMultipleScrollbarCandidates();
    testTrackExtentExcludesWindowChrome();
    testTopOfDocumentIsRecognizedBelowChrome();
    testSessionAssembly();
    testRejectedFrameKeepsSessionLive();
    testSustainedRejectionEventuallyPauses();
    testDuplicateFrameIsNotAFailure();
    testSmallUpwardJitterIsTolerated();
    testVanishedThumbIsTolerated();
    std::cout << "UniversalScrollStitcher core tests passed\n";
    return 0;
}
