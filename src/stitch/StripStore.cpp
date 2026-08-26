#include "universal_stitcher/StripStore.h"

#include <chrono>
#include <fstream>
#include <limits>
#include <random>
#include <string>

namespace universal_stitcher {
namespace {

std::filesystem::path makeTempPath() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::mt19937_64 generator(static_cast<std::uint64_t>(now));
    return std::filesystem::temp_directory_path() /
           ("UniversalScrollStitcher_" + std::to_string(generator()) + ".usstitch");
}

} // namespace

StripStore::~StripStore() { close(true); }

bool StripStore::open(int width, const std::filesystem::path& requestedPath) {
    close(true);
    if (width <= 0) return false;
    path_ = requestedPath.empty() ? makeTempPath() : requestedPath;
    std::ofstream output(path_, std::ios::binary | std::ios::trunc);
    if (!output) {
        path_.clear();
        return false;
    }
    const std::uint32_t storedWidth = static_cast<std::uint32_t>(width);
    output.write(reinterpret_cast<const char*>(&kMagic), sizeof(kMagic));
    output.write(reinterpret_cast<const char*>(&storedWidth), sizeof(storedWidth));
    if (!output) {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        path_.clear();
        return false;
    }
    width_ = width;
    open_ = true;
    return true;
}

bool StripStore::append(const cv::Mat& bgra, int firstRow, int lastRow) {
    if (!open_ || bgra.empty() || bgra.type() != CV_8UC4 || bgra.cols != width_ ||
        firstRow < 0 || lastRow <= firstRow || lastRow > bgra.rows) return false;

    const std::uint32_t rowCount = static_cast<std::uint32_t>(lastRow - firstRow);
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(&rowCount), sizeof(rowCount));
    const std::size_t rowBytes = static_cast<std::size_t>(width_) * 4U;
    for (int row = firstRow; row < lastRow; ++row) {
        output.write(reinterpret_cast<const char*>(bgra.ptr(row)), static_cast<std::streamsize>(rowBytes));
    }
    return static_cast<bool>(output);
}

cv::Mat StripStore::readAll() const {
    if (!open_) return {};
    std::ifstream input(path_, std::ios::binary);
    if (!input) return {};
    std::uint32_t magic = 0;
    std::uint32_t storedWidth = 0;
    input.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    input.read(reinterpret_cast<char*>(&storedWidth), sizeof(storedWidth));
    if (!input || magic != kMagic || storedWidth != static_cast<std::uint32_t>(width_)) return {};

    std::uint64_t totalRows = 0;
    while (input) {
        std::uint32_t rows = 0;
        input.read(reinterpret_cast<char*>(&rows), sizeof(rows));
        if (!input) break;
        if (rows == 0 || totalRows + rows > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) return {};
        const std::uint64_t bytes = static_cast<std::uint64_t>(rows) * static_cast<std::uint64_t>(width_) * 4U;
        input.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
        if (!input) return {};
        totalRows += rows;
    }
    if (totalRows == 0) return {};

    cv::Mat result(static_cast<int>(totalRows), width_, CV_8UC4);
    input.clear();
    input.seekg(static_cast<std::streamoff>(sizeof(kMagic) + sizeof(std::uint32_t)), std::ios::beg);
    const std::size_t rowBytes = static_cast<std::size_t>(width_) * 4U;
    int destinationRow = 0;
    while (destinationRow < result.rows) {
        std::uint32_t rows = 0;
        input.read(reinterpret_cast<char*>(&rows), sizeof(rows));
        if (!input || rows == 0 || destinationRow + static_cast<int>(rows) > result.rows) return {};
        for (std::uint32_t row = 0; row < rows; ++row) {
            input.read(reinterpret_cast<char*>(result.ptr(destinationRow++)), static_cast<std::streamsize>(rowBytes));
            if (!input) return {};
        }
    }
    return result;
}

void StripStore::close(bool removeFile) noexcept {
    if (removeFile && !path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    path_.clear();
    width_ = 0;
    open_ = false;
}

} // namespace universal_stitcher

