#pragma once

#include <opencv2/core.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace universal_stitcher {

struct ExportOptions {
    float jpegQuality = 0.95F;
};

struct ExportResult {
    bool success = false;
    std::vector<std::filesystem::path> files;
    std::string message;
};

class ImageExporter final {
public:
    [[nodiscard]] static ExportResult write(const cv::Mat& bgra,
                                            const std::filesystem::path& requestedPath,
                                            const ExportOptions& options = {});
};

} // namespace universal_stitcher

