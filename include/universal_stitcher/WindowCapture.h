#pragma once

#include "universal_stitcher/Frame.h"

#include <memory>
#include <optional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace universal_stitcher {

class IFrameSource {
public:
    virtual ~IFrameSource() = default;
    [[nodiscard]] virtual bool start(HWND target) = 0;
    [[nodiscard]] virtual std::optional<CaptureFrame> capture() = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual std::wstring name() const = 0;
    [[nodiscard]] virtual int width() const noexcept = 0;
    [[nodiscard]] virtual int height() const noexcept = 0;
};

class GdiCaptureSource final : public IFrameSource {
public:
    [[nodiscard]] bool start(HWND target) override;
    [[nodiscard]] std::optional<CaptureFrame> capture() override;
    void stop() noexcept override;
    [[nodiscard]] std::wstring name() const override { return L"Visible-window GDI"; }
    [[nodiscard]] int width() const noexcept override { return width_; }
    [[nodiscard]] int height() const noexcept override { return height_; }

private:
    HWND target_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

class GraphicsCaptureSource final : public IFrameSource {
public:
    GraphicsCaptureSource();
    ~GraphicsCaptureSource() override;

    GraphicsCaptureSource(const GraphicsCaptureSource&) = delete;
    GraphicsCaptureSource& operator=(const GraphicsCaptureSource&) = delete;

    [[nodiscard]] bool start(HWND target) override;
    [[nodiscard]] std::optional<CaptureFrame> capture() override;
    void stop() noexcept override;
    [[nodiscard]] std::wstring name() const override { return L"Windows Graphics Capture"; }
    [[nodiscard]] int width() const noexcept override { return width_; }
    [[nodiscard]] int height() const noexcept override { return height_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int width_ = 0;
    int height_ = 0;
};

// Prefer Windows Graphics Capture. If the SDK/vcpkg C++/WinRT headers are not
// installed or the target cannot be captured, fall back to visible GDI.
[[nodiscard]] std::unique_ptr<IFrameSource> createFrameSource(HWND target);

} // namespace universal_stitcher

