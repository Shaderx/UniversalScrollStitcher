#include "universal_stitcher/ImageExporter.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>

using namespace universal_stitcher;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    constexpr int width = 8;
    constexpr int rows = 65540;
    StripStore store;
    require(store.open(width), "store should open for streaming export test");

    cv::Mat chunk(64, width, CV_8UC4);
    for (int y = 0; y < chunk.rows; ++y) {
        for (int x = 0; x < width; ++x) {
            chunk.at<cv::Vec4b>(y, x) = cv::Vec4b(
                static_cast<unsigned char>((y * 3 + x) & 0xff),
                static_cast<unsigned char>((y * 7 + x * 2) & 0xff),
                static_cast<unsigned char>((y * 11 + x * 3) & 0xff), 255);
        }
    }
    int written = 0;
    while (written < rows) {
        const int count = std::min(chunk.rows, rows - written);
        require(store.append(chunk(cv::Rect(0, 0, width, count)), 0, count),
                "store should append each bounded chunk");
        written += count;
    }
    require(store.rows() == rows, "store should retain exact row count");

    const auto base = std::filesystem::temp_directory_path() / "UniversalScrollStitcher_export_test";
    const auto png = base.string() + ".png";
    const auto jpg = base.string() + ".jpg";
    std::error_code ignored;
    std::filesystem::remove(png, ignored);
    std::filesystem::remove(jpg, ignored);
    for (int part = 1; part <= 2; ++part) {
        std::filesystem::remove(base.string() + "_part00" + std::to_string(part) + ".png", ignored);
        std::filesystem::remove(base.string() + "_part00" + std::to_string(part) + ".jpg", ignored);
    }

    const auto small = base.string() + "_small.png";
    std::filesystem::remove(small, ignored);
    const ExportResult smallResult = ImageExporter::write(chunk, small);
    require(smallResult.success, "baseline WIC PNG export should work");
    std::filesystem::remove(small, ignored);

    const ExportResult pngResult = ImageExporter::write(store, png);
    require(pngResult.success && pngResult.files.size() == 2, "tall PNG should stream into two parts");
    for (const auto& file : pngResult.files) {
        require(std::filesystem::exists(file) && std::filesystem::file_size(file) > 0,
                "streamed PNG part should exist and be non-empty");
    }

    const ExportResult jpgResult = ImageExporter::write(store, jpg);
    require(jpgResult.success && jpgResult.files.size() == 2, "tall JPEG should stream into two parts");
    for (const auto& file : jpgResult.files) {
        require(std::filesystem::exists(file) && std::filesystem::file_size(file) > 0,
                "JPEG part should exist and be non-empty");
    }

    for (const auto& file : pngResult.files) std::filesystem::remove(file, ignored);
    for (const auto& file : jpgResult.files) std::filesystem::remove(file, ignored);
    store.close(true);
    std::cout << "UniversalScrollStitcher streaming export tests passed\n";
    return 0;
}
