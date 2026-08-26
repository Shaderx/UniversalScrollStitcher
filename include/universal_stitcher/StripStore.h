#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
#include <filesystem>

namespace universal_stitcher {

// Stores committed strips as raw BGRA chunks. Only the current and previous
// viewport are kept by StitchSession; readAll is intentionally the one point
// where the final image is materialized for encoding.
class StripStore final {
public:
    StripStore() = default;
    ~StripStore();

    StripStore(const StripStore&) = delete;
    StripStore& operator=(const StripStore&) = delete;

    [[nodiscard]] bool open(int width, const std::filesystem::path& path = {});
    [[nodiscard]] bool append(const cv::Mat& bgra, int firstRow, int lastRow);
    [[nodiscard]] cv::Mat readAll() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] bool isOpen() const noexcept { return open_; }

    void close(bool removeFile = true) noexcept;

private:
    static constexpr std::uint32_t kMagic = 0x31545355; // "UST1"
    std::filesystem::path path_;
    int width_ = 0;
    bool open_ = false;
};

} // namespace universal_stitcher

