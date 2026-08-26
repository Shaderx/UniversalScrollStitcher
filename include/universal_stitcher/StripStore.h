#pragma once

#include <opencv2/core.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>

namespace universal_stitcher {

// Stores committed strips as raw BGRA chunks. StitchSession keeps only the
// current viewport in memory; consumers should use forEachChunk() to stream
// the finalized image without materializing it.
class StripStore final {
public:
    using ChunkCallback = std::function<bool(const std::uint8_t* bgra,
                                             int rows,
                                             int width,
                                             std::uint64_t firstRow)>;

    StripStore() = default;
    ~StripStore();

    StripStore(const StripStore&) = delete;
    StripStore& operator=(const StripStore&) = delete;

    [[nodiscard]] bool open(int width, const std::filesystem::path& path = {});
    [[nodiscard]] bool append(const cv::Mat& bgra, int firstRow, int lastRow);
    [[nodiscard]] bool forEachChunk(const ChunkCallback& callback,
                                    std::uint64_t firstRow = 0,
                                    std::uint64_t rowCount = 0) const;
    [[nodiscard]] cv::Mat readAll() const;
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] std::uint64_t rows() const noexcept { return rowCount_; }
    [[nodiscard]] bool isOpen() const noexcept { return open_; }

    void close(bool removeFile = true) noexcept;

private:
    static constexpr std::uint32_t kMagic = 0x31545355; // "UST1"
    std::filesystem::path path_;
    int width_ = 0;
    std::uint64_t rowCount_ = 0;
    bool open_ = false;
};

} // namespace universal_stitcher
