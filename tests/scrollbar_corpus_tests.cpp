#include "universal_stitcher/ScrollbarDetector.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>

using namespace universal_stitcher;

struct Example {
    const char* file;
    int width, height, x, top, bottom, thumbTop, thumbBottom;
};

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    // Approximate visible core bounds, checked against the original pixels.
    // Allow four pixels for rounded/anti-aliased endpoints and strip placement.
    const std::array examples{
        Example{"image.png", 1917, 1115, 841, 209, 774, 209, 685},
        Example{"Screenshot_2026-08-26_15.39.43.png", 569, 1023, 546, 189, 710, 191, 338},
        Example{"Screenshot_2026-08-26_15.39.45.png", 569, 1023, 546, 189, 710, 307, 453},
        Example{"Screenshot_2026-08-26_15.39.46.png", 569, 1023, 546, 189, 710, 418, 564},
        Example{"Screenshot_2026-08-26_15.39.47.png", 569, 1023, 546, 189, 710, 502, 649},
        Example{"Screenshot_2026-08-26_15.39.48.png", 569, 1023, 546, 189, 711, 568, 710},
        Example{"Screenshot_2026-08-26_16.16.52.png", 711, 1279, 685, 235, 887, 235, 424},
        Example{"Screenshot_2026-08-26_16.16.55.png", 711, 1279, 685, 235, 887, 403, 592},
        Example{"Screenshot_2026-08-26_17.25.54.png", 711, 1279, 685, 235, 887, 535, 761}
    };
    bool passed = true;
    for (const auto& example : examples) {
        cv::Mat bgr = cv::imread((std::filesystem::path(argv[1]) / example.file).string());
        if (bgr.empty() || bgr.cols != example.width || bgr.rows != example.height) {
            std::cerr << "FAIL: missing or resized fixture " << example.file << '\n';
            passed = false;
            continue;
        }
        cv::Mat frame;
        cv::cvtColor(bgr, frame, cv::COLOR_BGR2BGRA);
        const auto candidates = ScrollbarDetector::autoDetectAll(frame);
        if (candidates.empty()) {
            std::cerr << "FAIL: no candidate for " << example.file << '\n';
            passed = false;
            continue;
        }
        const auto& best = candidates.front();
        const auto& track = best.config.track;
        const auto tracked = ScrollbarDetector::detect(frame, best.config);
        const auto near = [](int a, int b) { return std::abs(a - b) <= 4; };
        const bool valid = near(track.x, example.x) && track.width >= 3 && track.width <= 10 &&
            near(track.y, example.top) && near(track.bottom(), example.bottom) &&
            near(best.observation.thumb.y, example.thumbTop) &&
            near(best.observation.thumb.bottom(), example.thumbBottom) && tracked.detected &&
            near(tracked.thumb.y, best.observation.thumb.y) &&
            near(tracked.thumb.bottom(), best.observation.thumb.bottom());
        std::cout << (valid ? "PASS: " : "FAIL: ") << example.file << " rank1 track="
                  << track.x << ',' << track.y << ',' << track.width << ',' << track.height
                  << " thumb=" << best.observation.thumb.y << ',' << best.observation.thumb.height
                  << " tracked=" << tracked.thumb.y << ',' << tracked.thumb.height << '\n';
        passed = passed && valid;
    }
    return passed ? 0 : 1;
}
