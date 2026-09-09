// Exercise the actual MainWindow controls and workflow transitions with a
// deterministic Win32 target.  The test deliberately includes the production
// entry point so it can inspect the private UI state through UiWorkflowTests.
#include "../src/main.cpp"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace universal_stitcher {
namespace {

constexpr wchar_t kFixtureClass[] = L"UniversalScrollStitcherUiWorkflowFixture";
constexpr wchar_t kFixtureTitle[] = L"USS neutral reading list";

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

LRESULT CALLBACK fixtureWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        HFONT font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(RGB(31, 36, 45));
        FillRect(dc, &client, background);
        DeleteObject(background);

        RECT header{0, 0, client.right, 42};
        HBRUSH headerBrush = CreateSolidBrush(RGB(48, 57, 72));
        FillRect(dc, &header, headerBrush);
        DeleteObject(headerBrush);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 240, 248));
        RECT heading = {16, 10, client.right - 28, 34};
        DrawTextW(dc, L"Reading list", -1, &heading,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        const int scrollbarWidth = 18;
        const int trackTop = 48;
        const LONG trackBottom = std::max<LONG>(trackTop + 32L, client.bottom - 18L);
        RECT content{14, trackTop, std::max<LONG>(14L, client.right - scrollbarWidth - 10L), trackBottom};
        HBRUSH contentBrush = CreateSolidBrush(RGB(38, 45, 57));
        FillRect(dc, &content, contentBrush);
        DeleteObject(contentBrush);
        SetTextColor(dc, RGB(200, 211, 228));
        for (int row = 0, y = trackTop + 12; y + 18 < trackBottom; ++row, y += 24) {
            if ((row & 1) == 0) {
                RECT stripe{content.left, y - 4, content.right, y + 18};
                HBRUSH stripeBrush = CreateSolidBrush(RGB(43, 51, 64));
                FillRect(dc, &stripe, stripeBrush);
                DeleteObject(stripeBrush);
            }
            std::wstring text = L"Topic " + std::to_wstring(row + 1) +
                                L"  /  sample scroll content";
            TextOutW(dc, content.left + 10, y, text.c_str(), static_cast<int>(text.size()));
        }

        RECT track{client.right - scrollbarWidth, trackTop,
                   client.right, trackBottom};
        HBRUSH trackBrush = CreateSolidBrush(RGB(55, 63, 76));
        FillRect(dc, &track, trackBrush);
        DeleteObject(trackBrush);
        RECT thumb{track.left + 3, track.top + 12, track.right - 3,
                   track.top + 76};
        HBRUSH thumbBrush = CreateSolidBrush(RGB(141, 157, 181));
        FillRect(dc, &thumb, thumbBrush);
        DeleteObject(thumbBrush);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

HWND createFixtureWindow(HINSTANCE instance) {
    WNDCLASSEXW klass{sizeof(WNDCLASSEXW)};
    klass.hInstance = instance;
    klass.lpfnWndProc = fixtureWindowProc;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    klass.lpszClassName = kFixtureClass;
    if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;

    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW, kFixtureClass, kFixtureTitle,
        WS_OVERLAPPEDWINDOW, 24, 24, 800, 640, nullptr, nullptr, instance, nullptr);
    if (!window) return nullptr;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);
    return window;
}

void pumpMessages(DWORD durationMs = 50) {
    const ULONGLONG deadline = GetTickCount64() + durationMs;
    MSG message{};
    do {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    } while (GetTickCount64() < deadline);
}

bool saveWindowScreenshot(HWND window, const std::filesystem::path& path) {
    if (!IsWindow(window)) return false;
    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return false;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    if (!memory) {
        if (screen) ReleaseDC(nullptr, screen);
        return false;
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
    HGDIOBJ previous = bitmap ? SelectObject(memory, bitmap) : nullptr;
    if (!bitmap || !bits || !previous || previous == HGDI_ERROR) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }

    RECT clear{0, 0, width, height};
    HBRUSH brush = CreateSolidBrush(RGB(20, 23, 29));
    FillRect(memory, &clear, brush);
    DeleteObject(brush);
    const BOOL printed = PrintWindow(window, memory, PW_RENDERFULLCONTENT);
    if (!printed) {
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory),
                     PRF_CLIENT | PRF_CHILDREN);
        SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(memory),
                     PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN);
    }
    GdiFlush();
    const cv::Mat captured(height, width, CV_8UC4, bits,
                           static_cast<std::size_t>(width) * 4U);
    cv::Mat bgr;
    cv::cvtColor(captured, bgr, cv::COLOR_BGRA2BGR);
    const bool written = cv::imwrite(path.string(), bgr);
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return written;
}

void maybeScreenshot(HWND window, const char* filename) {
    const char* directory = std::getenv("USS_SCREENSHOT_DIR");
    if (!directory || !*directory) return;
    const std::filesystem::path outputDirectory(directory);
    std::error_code error;
    std::filesystem::create_directories(outputDirectory, error);
    require(!error, "could not create USS_SCREENSHOT_DIR: " + error.message());
    require(saveWindowScreenshot(window, outputDirectory / filename),
            "could not write UI screenshot " + std::string(filename));
}

class FixedFrameSource final : public IFrameSource {
public:
    explicit FixedFrameSource(cv::Mat frame) : frame_(std::move(frame)) {}

    bool start(HWND target) override {
        target_ = target;
        return IsWindow(target_) && !frame_.empty();
    }

    std::optional<CaptureFrame> capture() override {
        if (!IsWindow(target_) || frame_.empty()) return std::nullopt;
        return CaptureFrame{frame_.clone()};
    }

    void stop() noexcept override { target_ = nullptr; }
    std::wstring name() const override { return L"Deterministic UI workflow source"; }
    int width() const noexcept override { return frame_.cols; }
    int height() const noexcept override { return frame_.rows; }

private:
    cv::Mat frame_;
    HWND target_ = nullptr;
};

int findTargetIndex(HWND combo, HWND target) {
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT index = 0; index < count; ++index) {
        if (reinterpret_cast<HWND>(SendMessageW(combo, CB_GETITEMDATA, index, 0)) == target) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

struct UiWorkflowTests {
    static void run() {
        HINSTANCE instance = GetModuleHandleW(nullptr);
        INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_BAR_CLASSES};
        InitCommonControlsEx(&commonControls);

        MainWindow app;
        HWND fixture = nullptr;
        try {
            require(app.create(instance), "could not create the real MainWindow");
            require(app.uiStage() == MainWindow::UiStage::Choose,
                    "fresh MainWindow should start in Choose stage");
            require(app.selectedTarget() == nullptr,
                    "fresh target combo should not select a window implicitly");
            require(app.preview_.empty() && !app.calibrationReady_,
                    "fresh MainWindow should not have a preview or calibration");
            require(!IsWindowEnabled(GetDlgItem(app.window_, kStart)),
                    "Start capture should be disabled before a target is selected");

            SetWindowPos(app.window_, nullptr, 640, 36, 540, 800,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateWindow(app.window_);
            fixture = createFixtureWindow(instance);
            require(fixture != nullptr, "could not create deterministic target window");
            app.refreshTargets();
            const int targetIndex = findTargetIndex(app.targetCombo_, fixture);
            require(targetIndex >= 0, "Refresh should list the deterministic target window");

            SendMessageW(app.targetCombo_, CB_SETCURSEL, targetIndex, 0);
            SendMessageW(app.window_, WM_COMMAND,
                         MAKEWPARAM(kTargetCombo, CBN_SELCHANGE),
                         reinterpret_cast<LPARAM>(app.targetCombo_));
            require(app.selectedTarget() == fixture,
                    "WM_COMMAND target selection should update selectedTarget");
            require(!app.preview_.empty() && app.calibrationReady_,
                    "WM_COMMAND target selection should automatically prepare a preview");
            require(app.source_ != nullptr && app.sourceTarget_ == fixture,
                    "automatic preview should attach a source to the selected target");
            require(app.uiStage() == MainWindow::UiStage::Calibrate,
                    "a captured preview should advance the workflow to calibration");

            SetWindowPos(app.previewCanvas_, nullptr, 24, 24, 960, 760,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateWindow(app.previewCanvas_);

            const int imageWidth = app.preview_.cols;
            const int imageHeight = app.preview_.rows;
            require(imageWidth > 64 && imageHeight > 64,
                    "deterministic preview should have useful dimensions");
            const Rect viewport{8, 0, imageWidth - 36, imageHeight - 18};
            const Rect track{imageWidth - 18, 48, 18, imageHeight - 66};
            require(viewport.valid() && track.valid(), "workflow calibration fixture rectangles should be valid");
            app.setRect(app.viewportEdits_, viewport);
            app.setRect(app.trackEdits_, track);
            app.scrollbarSide_ = ScrollbarSide::Right;
            app.scrollbarCandidates_.clear();
            app.selectedScrollbarCandidate_.reset();
            app.refreshScrollbarCandidateCombo();
            app.refreshWorkflow();
            UpdateWindow(app.previewCanvas_);
            maybeScreenshot(app.previewCanvas_, "preview.png");
            require(app.validCalibration(), "valid workflow rectangles should enable calibration");
            require(IsWindowEnabled(GetDlgItem(app.window_, kStart)),
                    "Start capture should be enabled with valid calibration");
            maybeScreenshot(app.window_, "setup.png");

            app.setRect(app.viewportEdits_, {viewport.x, viewport.y, 0, viewport.height});
            app.refreshWorkflow();
            require(!app.validCalibration() &&
                    !IsWindowEnabled(GetDlgItem(app.window_, kStart)),
                    "an invalid capture rectangle should disable Start capture");
            app.setRect(app.viewportEdits_, viewport);
            app.refreshWorkflow();
            require(IsWindowEnabled(GetDlgItem(app.window_, kStart)),
                    "restoring a valid capture rectangle should re-enable Start capture");

            app.onButton(kAdvanced);
            app.scrollControls(SB_PAGEDOWN);
            UpdateWindow(app.window_);
            maybeScreenshot(app.window_, "precision.png");

            // The real target-selection path above is the interaction under
            // test. Use a deterministic in-memory source only for capture so
            // the result does not depend on compositor timing or a live user
            // scroll. It has the exact dimensions of the calibrated preview.
            auto fixedSource = std::make_unique<FixedFrameSource>(app.preview_.clone());
            require(fixedSource->start(fixture), "fixed workflow source should start");
            app.source_ = std::move(fixedSource);
            app.sourceTarget_ = fixture;
            app.target_ = fixture;

            app.onButton(kStart);
            require(app.capturing_ && app.session_.state() == SessionState::Capturing,
                    "Start capture should enter the live capture stage");
            require(app.uiStage() == MainWindow::UiStage::Capture,
                    "live capture should advance the workflow to Capture");
            require(!IsWindowEnabled(app.targetCombo_),
                    "target selection should be disabled during capture");
            for (HWND edit : app.viewportEdits_) {
                require(!IsWindowEnabled(edit), "capture area edits should be disabled during capture");
            }
            for (HWND edit : app.trackEdits_) {
                require(!IsWindowEnabled(edit), "scrollbar track edits should be disabled during capture");
            }
            maybeScreenshot(app.window_, "capture.png");

            // Force the real session into its paused recovery state with an
            // unrelated frame. The Capture stage must keep calibration and
            // target controls locked while the user decides whether to stop.
            cv::Mat unrelated(app.preview_.rows, app.preview_.cols,
                              CV_8UC4, cv::Scalar(7, 31, 99, 255));
            const Rect liveTrack = app.readRect(app.trackEdits_);
            const cv::Rect trackRoi(liveTrack.x, liveTrack.y,
                                    liveTrack.width, liveTrack.height);
            unrelated(trackRoi).setTo(cv::Scalar(76, 63, 55, 255));
            cv::rectangle(unrelated,
                          cv::Rect(liveTrack.x + 3, liveTrack.y + 24,
                                   std::max(2, liveTrack.width - 6),
                                   std::max(8, std::min(64, liveTrack.height - 28))),
                          cv::Scalar(181, 157, 141, 255), cv::FILLED);
            for (int attempt = 0; attempt < 80 && app.session_.state() == SessionState::Capturing; ++attempt) {
                const auto update = app.session_.process(unrelated);
                (void)update;
            }
            require(app.session_.state() == SessionState::Paused,
                    "sustained unrelated frames should put the live session into recovery");
            app.capturing_ = false;
            app.recovering_ = true;
            app.refreshWorkflow();
            require(app.uiStage() == MainWindow::UiStage::Capture,
                    "a paused session should remain in the Capture stage");
            require(!IsWindowEnabled(app.targetCombo_),
                    "target selection should remain disabled while capture is paused");
            for (HWND edit : app.viewportEdits_) {
                require(!IsWindowEnabled(edit), "capture edits should remain disabled while paused");
            }
            for (HWND edit : app.trackEdits_) {
                require(!IsWindowEnabled(edit), "track edits should remain disabled while paused");
            }

            app.onButton(kStop);
            require(!app.capturing_ && app.session_.hasOutput(),
                    "Stop capture should finalize a usable output");
            require(app.uiStage() == MainWindow::UiStage::Ready,
                    "a finalized session should advance the workflow to Ready");
            require(IsWindowEnabled(GetDlgItem(app.window_, kExport)),
                    "Export image should be enabled after finalization");
            wchar_t exportCaption[64]{};
            GetWindowTextW(GetDlgItem(app.window_, kExport), exportCaption,
                           static_cast<int>(std::size(exportCaption)));
            require(std::wstring(exportCaption) == L"Export image",
                    "Ready stage should keep the Export image primary action visible");
            maybeScreenshot(app.window_, "ready.png");

            const Rect savedViewport = app.readRect(app.viewportEdits_);
            const Rect savedTrack = app.readRect(app.trackEdits_);
            app.exported_ = true; // avoid the destructive confirmation dialog in this UI test
            app.onButton(kNewCapture);
            require(app.uiStage() == MainWindow::UiStage::Calibrate &&
                    app.calibrationReady_ && !app.preview_.empty(),
                    "New capture should return to calibration with the preview intact");
            require(app.readRect(app.viewportEdits_).x == savedViewport.x &&
                    app.readRect(app.viewportEdits_).y == savedViewport.y &&
                    app.readRect(app.viewportEdits_).width == savedViewport.width &&
                    app.readRect(app.viewportEdits_).height == savedViewport.height &&
                    app.readRect(app.trackEdits_).x == savedTrack.x &&
                    app.readRect(app.trackEdits_).y == savedTrack.y &&
                    app.readRect(app.trackEdits_).width == savedTrack.width &&
                    app.readRect(app.trackEdits_).height == savedTrack.height,
                    "New capture should preserve the calibrated areas");

            // Verify the GDI source geometry guard on the next calibrated
            // attempt. A resized source must force recalibration before any
            // stale rectangles are reused.
            auto gdiSource = std::make_unique<GdiCaptureSource>();
            require(gdiSource->start(fixture), "GDI resize fixture source should start");
            app.source_ = std::move(gdiSource);
            app.sourceTarget_ = fixture;
            SetWindowPos(fixture, nullptr, 24, 24, 520, 390,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateWindow(fixture);
            require(app.sourceGeometryChanged(),
                    "GDI source geometry guard should detect a resized target");
            app.captureTick();
            require(!app.calibrationReady_ && app.uiStage() == MainWindow::UiStage::Choose,
                    "a resized target should invalidate calibration before the next capture");

            pumpMessages(20);
            if (fixture) DestroyWindow(fixture);
            fixture = nullptr;
            if (app.window_) DestroyWindow(app.window_);
        } catch (...) {
            if (fixture) DestroyWindow(fixture);
            if (app.window_) DestroyWindow(app.window_);
            throw;
        }
    }
};

} // namespace
} // namespace universal_stitcher

int main() {
    try {
        universal_stitcher::UiWorkflowTests::run();
        std::cout << "UI workflow tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
