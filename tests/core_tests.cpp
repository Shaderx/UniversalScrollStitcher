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

void testDisagreementIsRejected() {
    const cv::Mat previous = documentFrame(0);
    const cv::Mat unrelated = documentFrame(47);
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

void testPausedSessionCanFinalizePrefix() {
    StitchOptions options;
    options.viewport = {0, 0, 96, 96};
    options.scrollbar = {{112, 0, 8, 96}, ScrollbarSide::Right, true};
    StitchSession session;
    require(session.start(fullFrame(0, 2), options), "pause test session should start");
    cv::Mat unrelated = fullFrame(37, 20);
    unrelated(cv::Rect(0, 0, 96, 96)).setTo(cv::Scalar(7, 31, 99, 255));
    const StitchUpdate update = session.process(unrelated);
    require(update.paused, "a rejected visual overlap should pause the session");
    require(session.state() == SessionState::Paused, "session should report paused state");
    require(session.finish(), "stop should finalize the valid prefix after a pause");
    require(session.hasOutput() && session.outputStore().rows() == 96,
            "paused finalization should preserve the initial viewport");
}

} // namespace

int main() {
    testShiftAndSeam();
    testDisagreementIsRejected();
    testScrollbarDetection();
    testMultipleScrollbarCandidates();
    testSessionAssembly();
    testPausedSessionCanFinalizePrefix();
    std::cout << "UniversalScrollStitcher core tests passed\n";
    return 0;
}
