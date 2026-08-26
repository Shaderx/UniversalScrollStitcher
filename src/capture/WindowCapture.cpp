#include "universal_stitcher/WindowCapture.h"

#include <opencv2/core.hpp>

#include <algorithm>

namespace universal_stitcher {

bool GdiCaptureSource::start(HWND target) {
    stop();
    if (!IsWindow(target)) return false;
    RECT client{};
    if (!GetClientRect(target, &client)) return false;
    width_ = client.right - client.left;
    height_ = client.bottom - client.top;
    if (width_ <= 0 || height_ <= 0) {
        stop();
        return false;
    }
    target_ = target;
    return true;
}

std::optional<CaptureFrame> GdiCaptureSource::capture() {
    if (!IsWindow(target_)) return std::nullopt;
    RECT client{};
    if (!GetClientRect(target_, &client)) return std::nullopt;
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0 || width != width_ || height != height_) return std::nullopt;

    HDC source = GetDC(target_);
    HDC memory = CreateCompatibleDC(source);
    if (!source || !memory) {
        if (memory) DeleteDC(memory);
        if (source) ReleaseDC(target_, source);
        return std::nullopt;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits || SelectObject(memory, bitmap) == nullptr) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(target_, source);
        return std::nullopt;
    }

    const BOOL copied = BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY | CAPTUREBLT);
    CaptureFrame frame;
    if (copied) {
        const cv::Mat view(height, width, CV_8UC4, bits, static_cast<std::size_t>(width) * 4U);
        frame.bgra = view.clone();
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(target_, source);
    if (!frame.valid()) return std::nullopt;
    return frame;
}

void GdiCaptureSource::stop() noexcept {
    target_ = nullptr;
    width_ = 0;
    height_ = 0;
}

std::unique_ptr<IFrameSource> createFrameSource(HWND target) {
    auto graphics = std::make_unique<GraphicsCaptureSource>();
    if (graphics->start(target)) return graphics;
    auto gdi = std::make_unique<GdiCaptureSource>();
    if (gdi->start(target)) return gdi;
    return nullptr;
}

} // namespace universal_stitcher

