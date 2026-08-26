#include "universal_stitcher/ImageExporter.h"
#include "universal_stitcher/StitchSession.h"
#include "universal_stitcher/WindowCapture.h"

#include <opencv2/core.hpp>

#include <windows.h>
#include <commdlg.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#ifdef UNIVERSAL_STITCHER_HAS_WGC
#include <winrt/base.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace universal_stitcher {
namespace {

enum ControlId : int {
    kTargetCombo = 100,
    kRefresh = 101,
    kPreview = 102,
    kAutoDetect = 103,
    kStart = 104,
    kStop = 105,
    kExport = 106,
    kViewportX = 120,
    kViewportY = 121,
    kViewportWidth = 122,
    kViewportHeight = 123,
    kTrackX = 130,
    kTrackY = 131,
    kTrackWidth = 132,
    kTrackHeight = 133,
    kStatus = 150
};

struct WindowInfo {
    HWND handle = nullptr;
    std::wstring title;
};

class MainWindow final {
public:
    bool create(HINSTANCE instance) {
        instance_ = instance;
        WNDCLASSEXW previewClass{sizeof(WNDCLASSEXW)};
        previewClass.lpfnWndProc = &MainWindow::previewProc;
        previewClass.hInstance = instance;
        previewClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        previewClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        previewClass.lpszClassName = L"UniversalScrollStitcherPreview";
        if (!RegisterClassExW(&previewClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        WNDCLASSEXW klass{sizeof(WNDCLASSEXW)};
        klass.lpfnWndProc = &MainWindow::windowProc;
        klass.hInstance = instance;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        klass.lpszClassName = L"UniversalScrollStitcherWindow";
        if (!RegisterClassExW(&klass)) return false;
        window_ = CreateWindowExW(0, klass.lpszClassName, L"Universal Scroll Stitcher",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  820, 850, nullptr, nullptr, instance, this);
        if (!window_) return false;
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        return true;
    }

private:
    static constexpr UINT_PTR kTimer = 1;
    static constexpr UINT kTimerPeriodMs = 100;

    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND targetCombo_ = nullptr;
    HWND previewCanvas_ = nullptr;
    HWND status_ = nullptr;
    std::array<HWND, 4> viewportEdits_{};
    std::array<HWND, 4> trackEdits_{};
    std::vector<WindowInfo> windows_;
    std::unique_ptr<IFrameSource> source_;
    HWND target_ = nullptr;
    HWND sourceTarget_ = nullptr;
    cv::Mat preview_;
    StitchSession session_;
    bool capturing_ = false;

    enum class CanvasTarget { None, Viewport, Track };
    enum CanvasEdge : int { EdgeNone = 0, EdgeLeft = 1, EdgeTop = 2, EdgeRight = 4, EdgeBottom = 8 };
    CanvasTarget dragTarget_ = CanvasTarget::None;
    int dragEdges_ = EdgeNone;
    POINT dragStartImage_{};
    Rect dragOriginal_{};

    static std::wstring widen(const std::string& value) {
        if (value.empty()) return {};
        const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (size <= 0) return std::wstring(value.begin(), value.end());
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
        return result;
    }

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<MainWindow*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handleMessage(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK previewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<MainWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->handlePreviewMessage(window, message, wParam, lParam) :
                      DefWindowProcW(window, message, wParam, lParam);
    }

    static BOOL CALLBACK enumerateWindow(HWND window, LPARAM parameter) {
        auto* context = reinterpret_cast<std::pair<MainWindow*, std::vector<WindowInfo>*>*>(parameter);
        if (!IsWindowVisible(window) || window == context->first->window_) return TRUE;
        wchar_t title[512]{};
        if (GetWindowTextW(window, title, static_cast<int>(std::size(title))) <= 0) return TRUE;
        if (GetWindow(window, GW_OWNER) != nullptr) return TRUE;
        context->second->push_back({window, title});
        return TRUE;
    }

    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            createControls();
            refreshTargets();
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) onButton(LOWORD(wParam));
            if (HIWORD(wParam) == EN_CHANGE) InvalidateRect(previewCanvas_, nullptr, FALSE);
            return 0;
        case WM_TIMER:
            if (wParam == kTimer) captureTick();
            return 0;
        case WM_SIZE:
            if (previewCanvas_) {
                const int width = std::max(100, LOWORD(lParam) - 36);
                const int height = std::max(160, HIWORD(lParam) - 445);
                MoveWindow(previewCanvas_, 18, 425, width, height, TRUE);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(window_, kTimer);
            if (source_) source_->stop();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    HWND addLabel(const wchar_t* text, int x, int y, int width = 180) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                               x, y, width, 22, window_, nullptr, instance_, nullptr);
    }

    HWND addButton(const wchar_t* text, int id, int x, int y, int width = 110) {
        return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               x, y, width, 28, window_, menuId(id), instance_, nullptr);
    }

    HWND addEdit(int id, int x, int y, int width = 70) {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                               x, y, width, 24, window_, menuId(id), instance_, nullptr);
    }

    void createControls() {
        addLabel(L"Capture target window:", 18, 18, 150);
        targetCombo_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                       WS_CHILD | WS_VISIBLE | WS_BORDER | CBS_DROPDOWNLIST,
                                       170, 14, 470, 240, window_, menuId(kTargetCombo), instance_, nullptr);
        addButton(L"Refresh", kRefresh, 650, 14, 90);
        addButton(L"Capture preview", kPreview, 18, 55, 125);
        addButton(L"Auto-detect scrollbar", kAutoDetect, 152, 55, 165);
        addButton(L"Start", kStart, 326, 55, 90);
        addButton(L"Stop", kStop, 424, 55, 90);
        addButton(L"Export PNG/JPEG", kExport, 522, 55, 125);

        addLabel(L"Content viewport (capture pixels)", 18, 105, 270);
        addLabel(L"X", 18, 136, 20); addLabel(L"Y", 130, 136, 20);
        addLabel(L"Width", 242, 136, 45); addLabel(L"Height", 354, 136, 50);
        viewportEdits_[0] = addEdit(kViewportX, 38, 132);
        viewportEdits_[1] = addEdit(kViewportY, 150, 132);
        viewportEdits_[2] = addEdit(kViewportWidth, 290, 132);
        viewportEdits_[3] = addEdit(kViewportHeight, 410, 132);

        addLabel(L"Scrollbar track (adjust if detection is wrong)", 18, 180, 330);
        addLabel(L"X", 18, 211, 20); addLabel(L"Y", 130, 211, 20);
        addLabel(L"Width", 242, 211, 45); addLabel(L"Height", 354, 211, 50);
        trackEdits_[0] = addEdit(kTrackX, 38, 207);
        trackEdits_[1] = addEdit(kTrackY, 150, 207);
        trackEdits_[2] = addEdit(kTrackWidth, 290, 207);
        trackEdits_[3] = addEdit(kTrackHeight, 410, 207);

        addLabel(L"The app only captures frames and never sends input to the target.", 18, 260, 560);
        addLabel(L"Scroll down manually in small increments. A scrollbar/visual disagreement pauses the session.", 18, 286, 740);
        status_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"Select a target and capture a preview.",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT, 18, 330, 760, 75,
                                  window_, menuId(kStatus), instance_, nullptr);
        addLabel(L"Preview: drag inside a rectangle to move it; drag an edge to resize it.", 18, 400, 740);
        previewCanvas_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"UniversalScrollStitcherPreview", nullptr,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         18, 425, 760, 380, window_, nullptr, instance_, this);
    }

    void refreshTargets() {
        windows_.clear();
        std::pair<MainWindow*, std::vector<WindowInfo>*> context{this, &windows_};
        EnumWindows(&MainWindow::enumerateWindow, reinterpret_cast<LPARAM>(&context));
        SendMessageW(targetCombo_, CB_RESETCONTENT, 0, 0);
        for (const auto& entry : windows_) {
            const LRESULT index = SendMessageW(targetCombo_, CB_ADDSTRING, 0,
                                               reinterpret_cast<LPARAM>(entry.title.c_str()));
            SendMessageW(targetCombo_, CB_SETITEMDATA, index, reinterpret_cast<LPARAM>(entry.handle));
        }
        if (!windows_.empty()) SendMessageW(targetCombo_, CB_SETCURSEL, 0, 0);
        setStatus(L"Select a target, then capture a preview to calibrate the viewport.");
    }

    [[nodiscard]] HWND selectedTarget() const {
        const LRESULT index = SendMessageW(targetCombo_, CB_GETCURSEL, 0, 0);
        if (index == CB_ERR) return nullptr;
        return reinterpret_cast<HWND>(SendMessageW(targetCombo_, CB_GETITEMDATA, index, 0));
    }

    static int valueFrom(HWND edit) {
        wchar_t text[32]{};
        GetWindowTextW(edit, text, static_cast<int>(std::size(text)));
        return std::max(0, _wtoi(text));
    }

    static void setValue(HWND edit, int value) {
        wchar_t text[32]{};
        _itow_s(value, text, 10);
        SetWindowTextW(edit, text);
    }

    static HMENU menuId(int id) {
        return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
    }

    Rect readRect(const std::array<HWND, 4>& edits) const {
        return {valueFrom(edits[0]), valueFrom(edits[1]), valueFrom(edits[2]), valueFrom(edits[3])};
    }

    void setRect(const std::array<HWND, 4>& edits, const Rect& rect) {
        setValue(edits[0], rect.x); setValue(edits[1], rect.y);
        setValue(edits[2], rect.width); setValue(edits[3], rect.height);
    }

    struct DisplayTransform {
        RECT destination{};
        double scale = 1.0;
    };

    [[nodiscard]] std::optional<DisplayTransform> displayTransform(HWND canvas) const {
        if (preview_.empty()) return std::nullopt;
        RECT client{};
        GetClientRect(canvas, &client);
        const int clientWidth = client.right - client.left;
        const int clientHeight = client.bottom - client.top;
        if (clientWidth <= 16 || clientHeight <= 16) return std::nullopt;
        const double scale = std::min(static_cast<double>(clientWidth - 16) / preview_.cols,
                                      static_cast<double>(clientHeight - 16) / preview_.rows);
        if (!(scale > 0.0)) return std::nullopt;
        const int width = std::max(1, static_cast<int>(std::lround(preview_.cols * scale)));
        const int height = std::max(1, static_cast<int>(std::lround(preview_.rows * scale)));
        const int left = (clientWidth - width) / 2;
        const int top = (clientHeight - height) / 2;
        return DisplayTransform{{left, top, left + width, top + height}, scale};
    }

    [[nodiscard]] bool clientToImage(HWND canvas, int clientX, int clientY, POINT& imagePoint) const {
        const auto transform = displayTransform(canvas);
        if (!transform) return false;
        const RECT& destination = transform->destination;
        if (clientX < destination.left || clientX >= destination.right ||
            clientY < destination.top || clientY >= destination.bottom) return false;
        imagePoint.x = std::clamp(static_cast<int>((clientX - destination.left) / transform->scale),
                                  0, preview_.cols - 1);
        imagePoint.y = std::clamp(static_cast<int>((clientY - destination.top) / transform->scale),
                                  0, preview_.rows - 1);
        return true;
    }

    static RECT scaledRect(const Rect& rect, const DisplayTransform& transform) {
        return {
            transform.destination.left + static_cast<LONG>(std::lround(rect.x * transform.scale)),
            transform.destination.top + static_cast<LONG>(std::lround(rect.y * transform.scale)),
            transform.destination.left + static_cast<LONG>(std::lround(rect.right() * transform.scale)),
            transform.destination.top + static_cast<LONG>(std::lround(rect.bottom() * transform.scale))};
    }

    static bool hitRect(const Rect& rect, const POINT& point, int radius, int& edges) {
        if (!rect.valid()) return false;
        const bool inside = point.x >= rect.x - radius && point.x <= rect.right() + radius &&
                            point.y >= rect.y - radius && point.y <= rect.bottom() + radius;
        if (!inside) return false;
        edges = EdgeNone;
        if (std::abs(point.x - rect.x) <= radius) edges |= EdgeLeft;
        if (std::abs(point.y - rect.y) <= radius) edges |= EdgeTop;
        if (std::abs(point.x - rect.right()) <= radius) edges |= EdgeRight;
        if (std::abs(point.y - rect.bottom()) <= radius) edges |= EdgeBottom;
        return true;
    }

    void paintPreview(HWND canvas, HDC dc) const {
        RECT client{};
        GetClientRect(canvas, &client);
        FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        if (preview_.empty()) {
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, L"Capture a preview to display the target and calibration rectangles.", -1,
                      &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        const auto transform = displayTransform(canvas);
        if (!transform) return;
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = preview_.cols;
        bitmapInfo.bmiHeader.biHeight = -preview_.rows;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        const RECT& destination = transform->destination;
        StretchDIBits(dc, destination.left, destination.top,
                      destination.right - destination.left, destination.bottom - destination.top,
                      0, 0, preview_.cols, preview_.rows, preview_.data, &bitmapInfo,
                      DIB_RGB_COLORS, SRCCOPY);

        const Rect viewport = readRect(viewportEdits_);
        const Rect track = readRect(trackEdits_);
        const RECT viewportRect = scaledRect(viewport, *transform);
        const RECT trackRect = scaledRect(track, *transform);
        HPEN viewportPen = CreatePen(PS_SOLID, 2, RGB(50, 220, 90));
        HPEN trackPen = CreatePen(PS_SOLID, 2, RGB(255, 170, 30));
        HGDIOBJ oldPen = SelectObject(dc, viewportPen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, viewportRect.left, viewportRect.top, viewportRect.right, viewportRect.bottom);
        SelectObject(dc, trackPen);
        Rectangle(dc, trackRect.left, trackRect.top, trackRect.right, trackRect.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(viewportPen);
        DeleteObject(trackPen);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(50, 220, 90));
        RECT viewportLabel{viewportRect.left + 4, viewportRect.top + 3, viewportRect.right - 4, viewportRect.top + 22};
        DrawTextW(dc, L"CONTENT VIEWPORT", -1, &viewportLabel, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(dc, RGB(255, 170, 30));
        RECT trackLabel{trackRect.left + 4, trackRect.top + 3, trackRect.right + 150, trackRect.top + 22};
        DrawTextW(dc, L"SCROLLBAR TRACK", -1, &trackLabel, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void beginPreviewDrag(HWND canvas, int clientX, int clientY) {
        POINT imagePoint{};
        if (!clientToImage(canvas, clientX, clientY, imagePoint)) return;
        const int radius = std::max(4, static_cast<int>(std::ceil(7.0 / displayTransform(canvas)->scale)));
        const Rect viewport = readRect(viewportEdits_);
        const Rect track = readRect(trackEdits_);
        int trackEdges = EdgeNone;
        int viewportEdges = EdgeNone;
        const bool hitTrack = hitRect(track, imagePoint, radius, trackEdges);
        const bool hitViewport = hitRect(viewport, imagePoint, radius, viewportEdges);
        if (hitTrack) {
            dragTarget_ = CanvasTarget::Track;
            dragEdges_ = trackEdges;
            dragOriginal_ = track;
        } else if (hitViewport) {
            dragTarget_ = CanvasTarget::Viewport;
            dragEdges_ = viewportEdges;
            dragOriginal_ = viewport;
        } else {
            dragTarget_ = CanvasTarget::None;
            return;
        }
        dragStartImage_ = imagePoint;
        SetCapture(canvas);
        SetCursor(LoadCursorW(nullptr, (dragEdges_ == EdgeNone) ? IDC_SIZEALL : IDC_SIZEWE));
    }

    void updatePreviewDrag(HWND canvas, int clientX, int clientY) {
        if (dragTarget_ == CanvasTarget::None) return;
        POINT imagePoint{};
        if (!clientToImage(canvas, clientX, clientY, imagePoint)) return;
        const int dx = imagePoint.x - dragStartImage_.x;
        const int dy = imagePoint.y - dragStartImage_.y;
        Rect result = dragOriginal_;
        const int minimumWidth = std::min(16, preview_.cols);
        const int minimumHeight = std::min(16, preview_.rows);
        if (dragEdges_ == EdgeNone) {
            result.x = std::clamp(dragOriginal_.x + dx, 0, preview_.cols - dragOriginal_.width);
            result.y = std::clamp(dragOriginal_.y + dy, 0, preview_.rows - dragOriginal_.height);
        } else {
            if (dragEdges_ & EdgeLeft) {
                const int left = std::clamp(dragOriginal_.x + dx, 0, dragOriginal_.right() - minimumWidth);
                result.x = left;
                result.width = dragOriginal_.right() - left;
            }
            if (dragEdges_ & EdgeRight) {
                result.width = std::clamp(dragOriginal_.width + dx, minimumWidth, preview_.cols - dragOriginal_.x);
            }
            if (dragEdges_ & EdgeTop) {
                const int top = std::clamp(dragOriginal_.y + dy, 0, dragOriginal_.bottom() - minimumHeight);
                result.y = top;
                result.height = dragOriginal_.bottom() - top;
            }
            if (dragEdges_ & EdgeBottom) {
                result.height = std::clamp(dragOriginal_.height + dy, minimumHeight, preview_.rows - dragOriginal_.y);
            }
        }
        if (dragTarget_ == CanvasTarget::Viewport) setRect(viewportEdits_, result);
        if (dragTarget_ == CanvasTarget::Track) setRect(trackEdits_, result);
        InvalidateRect(canvas, nullptr, FALSE);
    }

    void endPreviewDrag(HWND canvas) {
        if (GetCapture() == canvas) ReleaseCapture();
        dragTarget_ = CanvasTarget::None;
        dragEdges_ = EdgeNone;
        InvalidateRect(canvas, nullptr, FALSE);
    }

    LRESULT handlePreviewMessage(HWND canvas, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(canvas, &paint);
            paintPreview(canvas, dc);
            EndPaint(canvas, &paint);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(canvas);
            beginPreviewDrag(canvas, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE:
            if (GetCapture() == canvas) updatePreviewDrag(canvas, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_LBUTTONUP:
            endPreviewDrag(canvas);
            return 0;
        default:
            return DefWindowProcW(canvas, message, wParam, lParam);
        }
    }

    bool ensureSource() {
        target_ = selectedTarget();
        if (!IsWindow(target_)) {
            setStatus(L"Choose a valid visible top-level window first.");
            return false;
        }
        if (!source_ || source_->width() == 0 || sourceTarget_ != target_) {
            source_ = createFrameSource(target_);
            sourceTarget_ = target_;
        }
        if (!source_) {
            setStatus(L"Unable to initialize Windows Graphics Capture or visible-window fallback.");
            return false;
        }
        return true;
    }

    void capturePreview() {
        if (capturing_) {
            setStatus(L"Stop the current capture before recapturing a preview.");
            return;
        }
        target_ = selectedTarget();
        if (!IsWindow(target_)) {
            setStatus(L"Choose a valid visible top-level window first.");
            return;
        }
        // A preview belongs to exactly one target and calibration. Do not let
        // a failed recapture leave the previous target's image/coordinates in
        // place, and discard any finalized session tied to that image.
        preview_.release();
        session_.reset();
        InvalidateRect(previewCanvas_, nullptr, TRUE);
        source_ = createFrameSource(target_);
        sourceTarget_ = target_;
        if (!source_) {
            setStatus(L"Unable to capture the selected window.");
            return;
        }
        cv::Mat captured;
        for (int attempt = 0; attempt < 8 && captured.empty(); ++attempt) {
            const auto frame = source_->capture();
            if (frame) captured = frame->bgra.clone();
            Sleep(20);
        }
        if (captured.empty()) {
            setStatus(L"The capture source did not return a frame yet; try Capture preview again.");
            return;
        }
        preview_ = std::move(captured);
        setRect(viewportEdits_, {0, 0, preview_.cols, preview_.rows});
        const auto detected = ScrollbarDetector::autoDetect(preview_);
        if (detected) setRect(trackEdits_, detected->track);
        else setRect(trackEdits_, {std::max(0, preview_.cols - 18), 0, 18, preview_.rows});
        std::wstring message = L"Preview captured by " + source_->name() + L" (" +
            std::to_wstring(preview_.cols) + L"x" + std::to_wstring(preview_.rows) + L"). ";
        message += detected ? L"Scrollbar candidate detected; adjust fields if needed." :
                               L"No scrollbar candidate found; set the track fields manually.";
        setStatus(message);
        InvalidateRect(previewCanvas_, nullptr, TRUE);
    }

    void autoDetectScrollbar() {
        if (preview_.empty()) capturePreview();
        if (preview_.empty()) return;
        const auto detected = ScrollbarDetector::autoDetect(preview_);
        if (!detected) {
            setStatus(L"No scrollbar candidate was found. Set the track rectangle manually.");
            return;
        }
        setRect(trackEdits_, detected->track);
        setStatus(L"Scrollbar candidate applied. Verify or adjust its X/Y/width/height fields.");
    }

    void startCapture() {
        if (capturing_) return;
        if (preview_.empty() || selectedTarget() != sourceTarget_) capturePreview();
        if (!ensureSource()) return;
        const int calibratedWidth = preview_.cols;
        const int calibratedHeight = preview_.rows;
        const auto frame = source_->capture();
        if (!frame) {
            setStatus(L"Could not obtain the first frame; capture a preview again.");
            return;
        }
        if ((calibratedWidth > 0 && frame->width() != calibratedWidth) ||
            (calibratedHeight > 0 && frame->height() != calibratedHeight)) {
            preview_ = frame->bgra.clone();
            InvalidateRect(previewCanvas_, nullptr, TRUE);
            session_.reset();
            setStatus(L"The target changed size; capture a new preview and recalibrate.");
            return;
        }
        preview_ = frame->bgra.clone();
        InvalidateRect(previewCanvas_, nullptr, FALSE);
        StitchOptions options;
        options.viewport = readRect(viewportEdits_);
        options.scrollbar.track = readRect(trackEdits_);
        options.scrollbar.enabled = true;
        if (!options.viewport.valid() || !options.scrollbar.track.valid()) {
            setStatus(L"Viewport and scrollbar track must have positive width and height.");
            return;
        }
        if (!session_.start(preview_, options)) {
            setStatus(widen(session_.lastMessage()));
            return;
        }
        capturing_ = true;
        SetTimer(window_, kTimer, kTimerPeriodMs, nullptr);
        setStatus(L"Capturing. Scroll the target down manually; no input is injected.");
    }

    void stopCapture() {
        if (!capturing_ && session_.state() != SessionState::Paused &&
            session_.state() != SessionState::Capturing) return;
        capturing_ = false;
        KillTimer(window_, kTimer);
        if (session_.finish()) {
            setStatus(L"Finalized " + std::to_wstring(session_.outputStore().width()) + L"x" +
                      std::to_wstring(session_.outputStore().rows()) +
                      L" in disk-backed storage. Choose Export PNG/JPEG.");
        } else {
            setStatus(widen(session_.lastMessage()));
        }
    }

    void captureTick() {
        if (!capturing_ || !source_) return;
        const auto frame = source_->capture();
        if (!frame) {
            if (!IsWindow(target_)) {
                capturing_ = false;
                KillTimer(window_, kTimer);
                setStatus(L"The target window closed; click Stop to finalize the valid prefix.");
            } else if (source_->width() != preview_.cols || source_->height() != preview_.rows) {
                capturing_ = false;
                KillTimer(window_, kTimer);
                setStatus(L"Target size changed during capture; click Stop to finalize the valid prefix or recapture.");
            }
            // Windows Graphics Capture can have an empty frame queue between
            // compositor updates. Treat that as a transient miss, not a
            // failed capture.
            return;
        }
        if (frame->width() != preview_.cols || frame->height() != preview_.rows) {
            capturing_ = false;
            KillTimer(window_, kTimer);
            setStatus(L"Target size changed during capture; click Stop to finalize the valid prefix or recapture.");
            return;
        }
        const StitchUpdate update = session_.process(frame->bgra);
        if (update.paused) {
            capturing_ = false;
            KillTimer(window_, kTimer);
            setStatus(std::wstring(L"PAUSED: ") + widen(update.message));
            return;
        }
        if (update.accepted) {
            std::wstring message = widen(update.message) +
                L" (shift " + std::to_wstring(update.shift) + L", confidence " +
                std::to_wstring(update.confidence).substr(0, 5) + L")";
            setStatus(message);
        }
    }

    void exportImage() {
        if (capturing_) stopCapture();
        if (!session_.hasOutput()) {
            setStatus(L"Capture and stop a session before exporting.");
            return;
        }
        wchar_t filename[MAX_PATH] = L"stitched_capture.png";
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"PNG image (*.png)\0*.png\0JPEG image (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
        dialog.lpstrDefExt = L"png";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&dialog)) return;
        std::filesystem::path outputPath(filename);
        const std::wstring extension = outputPath.extension().wstring();
        const bool png = dialog.nFilterIndex != 2;
        if (extension.empty()) {
            outputPath += png ? L".png" : L".jpg";
        } else {
            std::wstring lower = extension;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
            if (lower != L".png" && lower != L".jpg" && lower != L".jpeg") {
                outputPath.replace_extension(png ? L".png" : L".jpg");
            }
        }
        const ExportResult result = ImageExporter::write(session_.outputStore(), outputPath);
        if (!result.success) {
            setStatus(widen(result.message));
            return;
        }
        setStatus(L"Exported " + std::to_wstring(result.files.size()) + L" file(s).");
    }

    void onButton(int id) {
        switch (id) {
        case kRefresh: refreshTargets(); break;
        case kPreview: capturePreview(); break;
        case kAutoDetect: autoDetectScrollbar(); break;
        case kStart: startCapture(); break;
        case kStop: stopCapture(); break;
        case kExport: exportImage(); break;
        default: break;
        }
    }

    void setStatus(const std::wstring& text) {
        if (status_) SetWindowTextW(status_, text.c_str());
    }
};

} // namespace
} // namespace universal_stitcher

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#ifdef UNIVERSAL_STITCHER_HAS_WGC
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (...) {
        // The app can still run with the GDI fallback if C++/WinRT is absent.
    }
#endif
    universal_stitcher::MainWindow app;
    if (!app.create(instance)) return 1;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
