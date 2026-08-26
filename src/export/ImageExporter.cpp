#include "universal_stitcher/ImageExporter.h"

#include <opencv2/imgproc.hpp>

#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace universal_stitcher {
namespace {

using Microsoft::WRL::ComPtr;

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

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
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
    if (uninitialize) CoUninitialize();
    if (FAILED(hr)) {
        std::ostringstream message;
        message << "WIC encoder failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ")";
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
    if (jpeg && (bgra.cols > 65535 || bgra.rows > 65535)) {
        // Height is split by write(); this guard protects the single-part
        // path and documents the other JPEG dimension limit.
        if (bgra.cols > 65535) {
            result.message = "JPEG width exceeds the 65,535-pixel format limit";
            return result;
        }
    }
    if (!jpeg || bgra.rows <= 65535) {
        std::string error;
        if (!writeOne(bgra, requestedPath, options.jpegQuality, error)) {
            result.message = error;
            return result;
        }
        result.success = true;
        result.files.push_back(requestedPath);
        result.message = "image exported";
        return result;
    }

    const int maxRows = 65535;
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
            result.message = error;
            return result;
        }
        result.files.push_back(partPath);
    }
    result.success = true;
    result.message = "image exceeded JPEG height limit and was split into numbered parts";
    return result;
}

} // namespace universal_stitcher
