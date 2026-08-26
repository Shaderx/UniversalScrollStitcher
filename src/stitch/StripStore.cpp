#include "universal_stitcher/StripStore.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

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
    rowCount_ = 0;
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
    if (!output) return false;
    rowCount_ += rowCount;
    return true;
}

bool StripStore::forEachChunk(const ChunkCallback& callback, std::uint64_t firstRow,
                              std::uint64_t rowCount) const {
    if (!open_ || !callback || firstRow > rowCount_) return false;
    if (rowCount > 0 && rowCount > std::numeric_limits<std::uint64_t>::max() - firstRow) return false;
    const std::uint64_t requestedEnd = rowCount == 0
        ? rowCount_
        : std::min(rowCount_, firstRow + rowCount);
    if (requestedEnd <= firstRow) return rowCount == 0 && firstRow == rowCount_;

    std::ifstream input(path_, std::ios::binary);
    if (!input) return false;
    std::uint32_t magic = 0;
    std::uint32_t storedWidth = 0;
    input.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    input.read(reinterpret_cast<char*>(&storedWidth), sizeof(storedWidth));
    if (!input || magic != kMagic || storedWidth != static_cast<std::uint32_t>(width_)) return false;

    constexpr std::uint32_t kRowsPerRead = 64;
    const std::size_t rowBytes = static_cast<std::size_t>(width_) * 4U;
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(kRowsPerRead) * rowBytes);
    std::uint64_t globalRow = 0;
    while (globalRow < rowCount_) {
        std::uint32_t storedRows = 0;
        input.read(reinterpret_cast<char*>(&storedRows), sizeof(storedRows));
        if (!input || storedRows == 0 || globalRow > rowCount_ ||
            static_cast<std::uint64_t>(storedRows) > rowCount_ - globalRow) return false;
        std::uint32_t localRow = 0;
        while (localRow < storedRows) {
            const std::uint32_t count = std::min(kRowsPerRead, storedRows - localRow);
            const std::uint64_t chunkFirst = globalRow + localRow;
            const std::uint64_t chunkLast = chunkFirst + count;
            if (chunkLast <= firstRow || chunkFirst >= requestedEnd) {
                const auto bytes = static_cast<std::streamoff>(static_cast<std::uint64_t>(count) * rowBytes);
                input.seekg(bytes, std::ios::cur);
                if (!input) return false;
            } else {
                input.read(reinterpret_cast<char*>(buffer.data()),
                           static_cast<std::streamsize>(static_cast<std::uint64_t>(count) * rowBytes));
                if (!input) return false;
                const std::uint64_t clippedFirst = std::max(chunkFirst, firstRow);
                const std::uint64_t clippedLast = std::min(chunkLast, requestedEnd);
                const auto offset = static_cast<std::size_t>((clippedFirst - chunkFirst) * rowBytes);
                const int clippedRows = static_cast<int>(clippedLast - clippedFirst);
                if (!callback(buffer.data() + offset, clippedRows, width_, clippedFirst)) return false;
            }
            localRow += count;
        }
        globalRow += storedRows;
    }
    return globalRow == rowCount_;
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
    int destinationRow = 0;
    if (!forEachChunk([&](const std::uint8_t* bytes, int rows, int width, std::uint64_t) {
            if (width != result.cols || destinationRow + rows > result.rows) return false;
            const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
            for (int row = 0; row < rows; ++row) {
                std::memcpy(result.ptr(destinationRow++), bytes + static_cast<std::size_t>(row) * rowBytes, rowBytes);
            }
            return true;
        })) return {};
    if (destinationRow != result.rows) return {};
    return result;
}

void StripStore::close(bool removeFile) noexcept {
    if (removeFile && !path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    path_.clear();
    width_ = 0;
    rowCount_ = 0;
    open_ = false;
}

} // namespace universal_stitcher
