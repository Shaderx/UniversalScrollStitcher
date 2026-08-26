#include "universal_stitcher/ImageExporter.h"

#include <opencv2/imgproc.hpp>

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace universal_stitcher {
namespace {

using Microsoft::WRL::ComPtr;

// JPEG cannot describe more than 65,535 rows, and some Windows codec versions
// reject the last few, so keep a margin. PNG has no comparable limit; splitting
// it too would defeat the point of a single continuous capture. The PNG value
// is only a sanity bound against a runaway session.
constexpr std::uint64_t kSafeMaxJpegRows = 65000ULL;
constexpr std::uint64_t kSafeMaxPngRows = 500000ULL;

constexpr std::uint64_t maxRowsFor(bool jpeg) noexcept {
    return jpeg ? kSafeMaxJpegRows : kSafeMaxPngRows;
}

std::wstring lowerExtension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return extension;
}

bool writeOne(const cv::Mat& bgra, const std::filesystem::path& path, float jpegQuality,
              std::string& error) {
    const bool jpeg = lowerExtension(path) == L".jpg" || lowerExtension(path) == L".jpeg";
    if (bgra.empty() || bgra.type() != CV_8UC4 || bgra.cols <= 0 || bgra.rows <= 0) {
        error = "image is empty or not BGRA";
        return false;
    }

    HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(apartmentResult);
    if (FAILED(apartmentResult) && apartmentResult != RPC_E_CHANGED_MODE) {
        error = "COM initialization failed";
        return false;
    }

    HRESULT hr = S_OK;
    {
        ComPtr<IWICImagingFactory> factory;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.GetAddressOf()));
        if (SUCCEEDED(hr)) {
        ComPtr<IWICStream> stream;
        hr = factory->CreateStream(stream.GetAddressOf());
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);

        GUID container = jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
        ComPtr<IWICBitmapEncoder> encoder;
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(container, nullptr, encoder.GetAddressOf());
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
        if (SUCCEEDED(hr) && jpeg) {
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = std::clamp(jpegQuality, 0.1F, 1.0F);
            hr = properties ? properties->Write(1, &option, &value) : E_POINTER;
            VariantClear(&value);
        }
        if (SUCCEEDED(hr)) hr = frame->Initialize(properties.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(bgra.cols), static_cast<UINT>(bgra.rows));
        if (SUCCEEDED(hr)) {
            GUID pixelFormat = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
            hr = frame->SetPixelFormat(&pixelFormat);
        }
        if (SUCCEEDED(hr)) {
            cv::Mat bgr;
            const cv::Mat* source = &bgra;
            if (jpeg) {
                cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
                source = &bgr;
            }
            const UINT stride = static_cast<UINT>(source->cols * source->elemSize());
            const UINT bytes = stride * static_cast<UINT>(source->rows);
            hr = frame->WritePixels(static_cast<UINT>(source->rows), stride, bytes,
                                    const_cast<BYTE*>(source->ptr<BYTE>()));
        }
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        }
    }
    if (uninitialize) CoUninitialize();
    if (FAILED(hr)) {
        std::ostringstream message;
        message << "WIC encoder failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ")";
        error = message.str();
        return false;
    }
    return true;
}

bool writeStorePart(const StripStore& store, const std::filesystem::path& path,
                    std::uint64_t firstRow, std::uint64_t rowCount, float jpegQuality,
                    std::string& error) {
    const bool jpeg = lowerExtension(path) == L".jpg" || lowerExtension(path) == L".jpeg";
    if (!store.isOpen() || store.width() <= 0 || rowCount == 0 ||
        rowCount > std::numeric_limits<UINT>::max()) {
        error = "strip store is empty or too large for WIC";
        return false;
    }
    if (jpeg && store.width() > 65535) {
        error = "JPEG width exceeds the 65,535-pixel format limit";
        return false;
    }

    HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(apartmentResult);
    if (FAILED(apartmentResult) && apartmentResult != RPC_E_CHANGED_MODE) {
        error = "COM initialization failed";
        return false;
    }

    HRESULT hr = S_OK;
    {
        ComPtr<IWICImagingFactory> factory;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.GetAddressOf()));
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        if (SUCCEEDED(hr)) hr = factory->CreateStream(stream.GetAddressOf());
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        const GUID container = jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(container, nullptr, encoder.GetAddressOf());
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
        if (SUCCEEDED(hr) && jpeg) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = std::clamp(jpegQuality, 0.1F, 1.0F);
        hr = properties ? properties->Write(1, &option, &value) : E_POINTER;
        VariantClear(&value);
        }
        if (SUCCEEDED(hr)) hr = frame->Initialize(properties.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(store.width()), static_cast<UINT>(rowCount));
        if (SUCCEEDED(hr)) {
            GUID pixelFormat = jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
            hr = frame->SetPixelFormat(&pixelFormat);
        }
        if (SUCCEEDED(hr)) {
            const bool streamed = store.forEachChunk(
                [&](const std::uint8_t* bytes, int rows, int width, std::uint64_t) {
                    if (width != store.width() || rows <= 0) return false;
                    const std::size_t bgraStride = static_cast<std::size_t>(width) * 4U;
                    cv::Mat bgra(rows, width, CV_8UC4, const_cast<std::uint8_t*>(bytes), bgraStride);
                    cv::Mat bgr;
                    const std::uint8_t* source = bytes;
                    UINT stride = static_cast<UINT>(bgraStride);
                    UINT bytesToWrite = static_cast<UINT>(static_cast<std::uint64_t>(rows) * bgraStride);
                    if (jpeg) {
                        cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
                        source = bgr.ptr<std::uint8_t>();
                        stride = static_cast<UINT>(bgr.step);
                        bytesToWrite = static_cast<UINT>(static_cast<std::uint64_t>(rows) * stride);
                    }
                    const HRESULT writeResult = frame->WritePixels(static_cast<UINT>(rows), stride,
                                                                    bytesToWrite,
                                                                    const_cast<BYTE*>(source));
                    return SUCCEEDED(writeResult);
                }, firstRow, rowCount);
            if (!streamed) hr = E_FAIL;
        }
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
    }
    if (uninitialize) CoUninitialize();
    if (FAILED(hr)) {
        std::ostringstream message;
        message << "WIC streaming encoder failed (HRESULT 0x" << std::hex
                << static_cast<unsigned long>(hr) << ")";
        error = message.str();
        return false;
    }
    return true;
}

} // namespace

ExportResult ImageExporter::write(const cv::Mat& bgra, const std::filesystem::path& requestedPath,
                                   const ExportOptions& options) {
    ExportResult result;
    if (requestedPath.empty() || bgra.empty() || bgra.type() != CV_8UC4) {
        result.message = "an output path and BGRA image are required";
        return result;
    }
    const std::wstring extension = lowerExtension(requestedPath);
    const bool jpeg = extension == L".jpg" || extension == L".jpeg";
    if (!jpeg && extension != L".png") {
        result.message = "output extension must be .png, .jpg, or .jpeg";
        return result;
    }
    if (bgra.cols > 65535) {
        result.message = jpeg
            ? "JPEG width exceeds the 65,535-pixel format limit"
            : "PNG width exceeds the WIC 65,535-pixel format limit";
        return result;
    }
    if (static_cast<std::uint64_t>(bgra.rows) <= maxRowsFor(jpeg)) {
        std::string error;
        if (!writeOne(bgra, requestedPath, options.jpegQuality, error)) {
            std::error_code ignored;
            std::filesystem::remove(requestedPath, ignored);
            result.message = error;
            return result;
        }
        result.success = true;
        result.files.push_back(requestedPath);
        result.message = "image exported";
        return result;
    }

    const int maxRows = static_cast<int>(maxRowsFor(jpeg));
    const int partCount = (bgra.rows + maxRows - 1) / maxRows;
    for (int part = 0; part < partCount; ++part) {
        const int top = part * maxRows;
        const int rows = std::min(maxRows, bgra.rows - top);
        std::filesystem::path partPath = requestedPath;
        std::wstringstream suffix;
        suffix << L"_part" << std::setfill(L'0') << std::setw(3) << (part + 1);
        partPath.replace_extension(suffix.str() + extension);
        std::string error;
        if (!writeOne(bgra(cv::Rect(0, top, bgra.cols, rows)).clone(), partPath, options.jpegQuality, error)) {
            std::error_code ignored;
            for (const auto& file : result.files) std::filesystem::remove(file, ignored);
            std::filesystem::remove(partPath, ignored);
            result.files.clear();
            result.message = error;
            return result;
        }
        result.files.push_back(partPath);
    }
    result.success = true;
    result.message = "image exceeded the WIC row limit and was split into numbered parts";
    return result;
}

ExportResult ImageExporter::write(const StripStore& store,
                                  const std::filesystem::path& requestedPath,
                                  const ExportOptions& options) {
    ExportResult result;
    if (requestedPath.empty() || !store.isOpen() || store.width() <= 0 || store.rows() == 0) {
        result.message = "an output path and non-empty strip store are required";
        return result;
    }
    const std::wstring extension = lowerExtension(requestedPath);
    const bool jpeg = extension == L".jpg" || extension == L".jpeg";
    if (!jpeg && extension != L".png") {
        result.message = "output extension must be .png, .jpg, or .jpeg";
        return result;
    }
    if (store.width() > 65535) {
        result.message = jpeg
            ? "JPEG width exceeds the 65,535-pixel format limit"
            : "PNG width exceeds the WIC 65,535-pixel format limit";
        return result;
    }
    const std::uint64_t maxRows = maxRowsFor(jpeg);
    const std::uint64_t partCount = (store.rows() + maxRows - 1) / maxRows;
    for (std::uint64_t part = 0; part < partCount; ++part) {
        const std::uint64_t firstRow = part * maxRows;
        const std::uint64_t rows = std::min(maxRows, store.rows() - firstRow);
        std::filesystem::path partPath = requestedPath;
        if (partCount > 1) {
            std::wstringstream suffix;
            suffix << L"_part" << std::setfill(L'0') << std::setw(3) << (part + 1);
            partPath.replace_extension(suffix.str() + extension);
        }
        std::string error;
        if (!writeStorePart(store, partPath, firstRow, rows, options.jpegQuality, error)) {
            std::error_code ignored;
            for (const auto& file : result.files) std::filesystem::remove(file, ignored);
            std::filesystem::remove(partPath, ignored);
            result.files.clear();
            result.message = error;
            return result;
        }
        result.files.push_back(partPath);
    }
    result.success = true;
    result.message = partCount > 1
        ? "image was streamed and split into numbered parts"
        : "image was streamed to the encoder";
    return result;
}

} // namespace universal_stitcher
