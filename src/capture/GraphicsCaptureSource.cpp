#include "universal_stitcher/WindowCapture.h"

#ifdef UNIVERSAL_STITCHER_HAS_WGC

#include <inspectable.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <d3d11.h>
#include <dxgi.h>

#include <opencv2/core.hpp>

#include <cstdint>
#include <cstring>

namespace universal_stitcher {

struct GraphicsCaptureSource::Impl {
    HWND target = nullptr;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice directDevice{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{nullptr};
    winrt::Windows::Graphics::SizeInt32 size{};
};

GraphicsCaptureSource::GraphicsCaptureSource() : impl_(std::make_unique<Impl>()) {}
GraphicsCaptureSource::~GraphicsCaptureSource() { stop(); }

bool GraphicsCaptureSource::start(HWND target) {
    stop();
    try {
        impl_ = std::make_unique<Impl>();
        impl_->target = target;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL featureLevel{};
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                                nullptr, 0, D3D11_SDK_VERSION, impl_->device.put(),
                                                &featureLevel, impl_->context.put()));
        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        winrt::check_hresult(impl_->device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put())));
        winrt::com_ptr<IInspectable> inspectableDevice;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectableDevice.put()));
        impl_->directDevice = inspectableDevice.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

        auto activation = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
        auto interop = activation.as<IGraphicsCaptureItemInterop>();
        winrt::com_ptr<IInspectable> inspectableItem;
        winrt::check_hresult(interop->CreateForWindow(target,
            winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), inspectableItem.put_void()));
        impl_->item = inspectableItem.as<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>();
        impl_->size = impl_->item.Size();
        if (impl_->size.Width <= 0 || impl_->size.Height <= 0) return false;

        // A deeper pool retains intermediate frames during mouse-wheel bursts
        // so the 60 Hz stitch loop can drain them instead of skipping ahead.
        // Eight BGRA surfaces are a deliberate memory/latency tradeoff: enough
        // history for short accelerated-wheel bursts without an unbounded
        // capture queue.
        impl_->framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
            impl_->directDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            8, impl_->size);
        impl_->session = impl_->framePool.CreateCaptureSession(impl_->item);
        try {
            if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
                impl_->session.IsBorderRequired(false);
            }
            if (winrt::Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
                    L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsCursorCaptureEnabled")) {
                impl_->session.IsCursorCaptureEnabled(false);
            }
        } catch (...) {
            // Older Windows 10 builds may not expose optional properties.
        }
        impl_->session.StartCapture();
        width_ = impl_->size.Width;
        height_ = impl_->size.Height;
        return true;
    } catch (...) {
        stop();
        return false;
    }
}

std::optional<CaptureFrame> GraphicsCaptureSource::capture() {
    if (!impl_ || !impl_->framePool) return std::nullopt;
    try {
        auto frame = impl_->framePool.TryGetNextFrame();
        if (!frame) return std::nullopt;
        const auto contentSize = frame.ContentSize();
        if (contentSize.Width != impl_->size.Width || contentSize.Height != impl_->size.Height) {
            impl_->size = contentSize;
            impl_->framePool.Recreate(impl_->directDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                8, impl_->size);
            width_ = impl_->size.Width;
            height_ = impl_->size.Height;
            return std::nullopt;
        }
        auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
        D3D11_TEXTURE2D_DESC sourceDescription{};
        texture->GetDesc(&sourceDescription);
        D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.BindFlags = 0;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDescription.MiscFlags = 0;
        stagingDescription.ArraySize = 1;
        stagingDescription.MipLevels = 1;
        stagingDescription.SampleDesc.Count = 1;
        stagingDescription.SampleDesc.Quality = 0;
        winrt::com_ptr<ID3D11Texture2D> staging;
        winrt::check_hresult(impl_->device->CreateTexture2D(&stagingDescription, nullptr, staging.put()));
        impl_->context->CopyResource(staging.get(), texture.get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(impl_->context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
        CaptureFrame result;
        result.bgra.create(height_, width_, CV_8UC4);
        const std::size_t rowBytes = static_cast<std::size_t>(width_) * 4U;
        for (int row = 0; row < height_; ++row) {
            std::memcpy(result.bgra.ptr(row), static_cast<const std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(row) * mapped.RowPitch, rowBytes);
        }
        impl_->context->Unmap(staging.get(), 0);
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

void GraphicsCaptureSource::stop() noexcept {
    if (impl_) {
        try {
            if (impl_->session) impl_->session.Close();
            if (impl_->framePool) impl_->framePool.Close();
        } catch (...) {
        }
        impl_->session = nullptr;
        impl_->framePool = nullptr;
        impl_->item = nullptr;
        impl_->directDevice = nullptr;
        impl_->context = nullptr;
        impl_->device = nullptr;
        impl_->target = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

} // namespace universal_stitcher

#else

namespace universal_stitcher {
struct GraphicsCaptureSource::Impl {};
GraphicsCaptureSource::GraphicsCaptureSource() : impl_(std::make_unique<Impl>()) {}
GraphicsCaptureSource::~GraphicsCaptureSource() = default;
bool GraphicsCaptureSource::start(HWND) { return false; }
std::optional<CaptureFrame> GraphicsCaptureSource::capture() { return std::nullopt; }
void GraphicsCaptureSource::stop() noexcept { width_ = 0; height_ = 0; }
} // namespace universal_stitcher

#endif
