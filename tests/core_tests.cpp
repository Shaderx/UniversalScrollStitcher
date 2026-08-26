#include "universal_stitcher/ScrollbarDetector.h"
#include "universal_stitcher/SeamFinder.h"
#include "universal_stitcher/StitchSession.h"
#include "universal_stitcher/VerticalShiftEstimator.h"

#include <opencv2/core.hpp>

#include <cstdlib>
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
    require(observation.confidence > 0.50F, "scrollbar confidence should be useful");
    const auto automatic = ScrollbarDetector::autoDetect(frame);
    require(automatic.has_value(), "edge scrollbar should be auto-detected");
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
    require(session.finalImage().rows > 96, "assembled image should contain newly exposed rows");
    require(session.finalImage().cols == 96, "assembled image should preserve viewport width");
}

} // namespace

int main() {
    testShiftAndSeam();
    testDisagreementIsRejected();
    testScrollbarDetection();
    testSessionAssembly();
    std::cout << "UniversalScrollStitcher core tests passed\n";
    return 0;
}
