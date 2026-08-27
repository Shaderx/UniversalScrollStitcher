#include "universal_stitcher/ImageExporter.h"
#include "universal_stitcher/CalibrationProfile.h"
#include "universal_stitcher/DiagnosticLogger.h"
#include "universal_stitcher/StitchSession.h"
#include "universal_stitcher/WindowCapture.h"

#include <opencv2/core.hpp>

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#ifdef UNIVERSAL_STITCHER_HAS_WGC
#include <winrt/base.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
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
    kSaveTemplate = 107,
    kLoadTemplate = 108,
    kViewportX = 120,
    kViewportY = 121,
    kViewportWidth = 122,
    kViewportHeight = 123,
    kTrackX = 130,
    kTrackY = 131,
    kTrackWidth = 132,
    kTrackHeight = 133,
    kStatus = 150,
    kLoggingEnable = 151,
    kOpenLogs = 152,
    kLatestRelease = 153
};

constexpr wchar_t kLatestReleaseUrl[] =
    L"https://github.com/Shaderx/UniversalScrollStitcher/releases/latest";

struct WindowInfo {
    HWND handle = nullptr;
    std::wstring title;
};

class MainWindow final {
public:
    bool create(HINSTANCE instance) {
        instance_ = instance;
        backgroundBrush_ = CreateSolidBrush(RGB(246, 248, 251));
        cardBrush_ = CreateSolidBrush(RGB(255, 255, 255));
        statusBrush_ = CreateSolidBrush(RGB(239, 246, 255));
        WNDCLASSEXW previewClass{sizeof(WNDCLASSEXW)};
        previewClass.lpfnWndProc = &MainWindow::previewProc;
        previewClass.hInstance = instance;
        previewClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        previewClass.hbrBackground = backgroundBrush_;
        previewClass.lpszClassName = L"UniversalScrollStitcherPreview";
        if (!RegisterClassExW(&previewClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        WNDCLASSEXW klass{sizeof(WNDCLASSEXW)};
        klass.lpfnWndProc = &MainWindow::windowProc;
        klass.hInstance = instance;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = backgroundBrush_;
        klass.lpszClassName = L"UniversalScrollStitcherWindow";
        if (!RegisterClassExW(&klass)) return false;
        window_ = CreateWindowExW(0, klass.lpszClassName, L"Universal Scroll Stitcher",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  1040, 900, nullptr, nullptr, instance, this);
        if (!window_) return false;
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        return true;
    }

private:
    static constexpr UINT_PTR kTimer = 1;
    // ~30 Hz is enough to catch mouse-wheel notches while staying responsive on
    // the UI thread. Arrow-key scrolling was already fine at 10 Hz.
    static constexpr UINT kTimerPeriodMs = 33;
    // Bound work per UI tick so a burst cannot starve preview painting. The
    // WGC pool retains four frames and the next tick continues draining it.
    static constexpr int kMaximumFramesPerTick = 3;
    static constexpr int kMinimumClientWidth = 980;
    static constexpr int kMinimumClientHeight = 760;

    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND targetCombo_ = nullptr;
    HWND previewCanvas_ = nullptr;
    HWND status_ = nullptr;
    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND targetCard_ = nullptr;
    HWND calibrationCard_ = nullptr;
    HWND statusCard_ = nullptr;
    HWND previewCard_ = nullptr;
    HWND loggingCheckbox_ = nullptr;
    HWND openLogsButton_ = nullptr;
    HWND latestReleaseLink_ = nullptr;
    HFONT latestReleaseFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    HBRUSH statusBrush_ = nullptr;
    std::array<HWND, 4> viewportEdits_{};
    std::array<HWND, 4> trackEdits_{};
    std::vector<WindowInfo> windows_;
    std::unique_ptr<IFrameSource> source_;
    HWND target_ = nullptr;
    HWND sourceTarget_ = nullptr;
    cv::Mat preview_;
    std::vector<ScrollbarCandidate> scrollbarCandidates_;
    std::optional<std::size_t> selectedScrollbarCandidate_;
    std::optional<CalibrationProfile> pendingCalibrationProfile_;
    ScrollbarSide scrollbarSide_ = ScrollbarSide::Right;
    StitchSession session_;
    DiagnosticLogger logger_;
    bool capturing_ = false;
    bool updatingCalibrationEdits_ = false;
    std::uint64_t captureTickCount_ = 0;
    std::uint64_t transientMissCount_ = 0;

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
            if (LOWORD(wParam) == kLatestRelease && HIWORD(wParam) == STN_CLICKED) {
                logger_.info("ui", "GitHub latest-release credit clicked");
                ShellExecuteW(window_, L"open", kLatestReleaseUrl, nullptr, nullptr, SW_SHOWNORMAL);
            }
            if (HIWORD(wParam) == EN_CHANGE) {
                const int id = LOWORD(wParam);
                if (!updatingCalibrationEdits_ && id >= kTrackX && id <= kTrackHeight) {
                    selectedScrollbarCandidate_.reset();
                    const Rect track = readRect(trackEdits_);
                    if (!preview_.empty() && track.valid()) {
                        scrollbarSide_ = track.x + track.width / 2 < preview_.cols / 2
                            ? ScrollbarSide::Left : ScrollbarSide::Right;
                    }
                    logger_.info("calibration", "manual scrollbar track edit changed to{" +
                                 rectDescription(track) + "}");
                } else if (!updatingCalibrationEdits_ && id >= kViewportX && id <= kViewportHeight) {
                    logger_.info("calibration", "manual viewport edit changed to{" +
                                 rectDescription(readRect(viewportEdits_)) + "}");
                }
                InvalidateRect(previewCanvas_, nullptr, FALSE);
            }
            if (LOWORD(wParam) == kTargetCombo && HIWORD(wParam) == CBN_SELCHANGE) {
                logger_.info("target", "selected " + windowDescription(selectedTarget()));
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            RECT desiredClient{0, 0, kMinimumClientWidth, kMinimumClientHeight};
            UINT dpi = GetDpiForWindow(window_);
            if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;
            RECT desiredWindow = desiredClient;
            if (!AdjustWindowRectExForDpi(&desiredWindow, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) {
                AdjustWindowRectEx(&desiredWindow, WS_OVERLAPPEDWINDOW, FALSE, 0);
            }
            limits->ptMinTrackSize.x = desiredWindow.right - desiredWindow.left;
            limits->ptMinTrackSize.y = desiredWindow.bottom - desiredWindow.top;
            return 0;
        }
        case WM_TIMER:
            if (wParam == kTimer) captureTick();
            return 0;
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                layoutControls(static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
                // StretchDIBits uses the current child-client size on every
                // paint. Force a complete child repaint here so resizing the
                // main window immediately rescales the existing preview.
                if (previewCanvas_) {
                    RedrawWindow(previewCanvas_, nullptr, nullptr,
                                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                }
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            return colorStatic(reinterpret_cast<HWND>(lParam), reinterpret_cast<HDC>(wParam));
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }
        case WM_DRAWITEM:
            drawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_DESTROY:
            KillTimer(window_, kTimer);
            if (source_) source_->stop();
            logger_.info("app", "shutdown");
            logger_.disable();
            if (latestReleaseFont_) DeleteObject(latestReleaseFont_);
            if (headingFont_) DeleteObject(headingFont_);
            if (bodyFont_) DeleteObject(bodyFont_);
            if (backgroundBrush_) DeleteObject(backgroundBrush_);
            if (cardBrush_) DeleteObject(cardBrush_);
            if (statusBrush_) DeleteObject(statusBrush_);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

    HWND addLabel(const wchar_t* text, int x, int y, int width = 180) {
        HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     x, y, width, 22, window_, nullptr, instance_, nullptr);
        if (bodyFont_) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        return label;
    }

    HWND addButton(const wchar_t* text, int id, int x, int y, int width = 110) {
        HWND button = CreateWindowExW(0, L"BUTTON", text,
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                                      x, y, width, 34, window_, menuId(id), instance_, nullptr);
        if (bodyFont_) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        return button;
    }

    HWND addEdit(int id, int x, int y, int width = 70) {
        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                    x, y, width, 26, window_, menuId(id), instance_, nullptr);
        if (bodyFont_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        return edit;
    }

    void createControls() {
        bodyFont_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        headingFont_ = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        title_ = addLabel(L"Universal Scroll Stitcher", 24, 20, 700);
        subtitle_ = addLabel(L"Capture a long page from any visible window - locally, privately, and without injected input.",
                             24, 58, 880);
        if (headingFont_) SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);

        targetCard_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME,
                                      24, 96, 900, 76, window_, nullptr, instance_, nullptr);
        addLabel(L"Capture target window", 42, 116, 150);
        targetCombo_ = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | CBS_DROPDOWNLIST,
                                       200, 112, 600, 260, window_, menuId(kTargetCombo), instance_, nullptr);
        if (bodyFont_) SendMessageW(targetCombo_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        addButton(L"Refresh", kRefresh, 812, 110, 100);

        addButton(L"Capture preview", kPreview, 24, 184, 144);
        addButton(L"Auto-detect scrollbar", kAutoDetect, 180, 184, 190);
        addButton(L"Start capture", kStart, 382, 184, 130);
        addButton(L"Stop", kStop, 524, 184, 100);
        addButton(L"Export PNG / JPEG", kExport, 636, 184, 164);

        calibrationCard_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME,
                                           24, 232, 900, 130, window_, nullptr, instance_, nullptr);
        addLabel(L"Calibration", 42, 246, 170);
        addLabel(L"Content viewport (capture pixels)", 42, 273, 240);
        addLabel(L"X", 42, 300, 18); addLabel(L"Y", 174, 300, 18);
        addLabel(L"Width", 306, 300, 44); addLabel(L"Height", 444, 300, 50);
        viewportEdits_[0] = addEdit(kViewportX, 62, 296, 96);
        viewportEdits_[1] = addEdit(kViewportY, 194, 296, 96);
        viewportEdits_[2] = addEdit(kViewportWidth, 352, 296, 78);
        viewportEdits_[3] = addEdit(kViewportHeight, 500, 296, 96);
        addLabel(L"Scrollbar track - adjust if detection is wrong", 630, 273, 270);
        trackEdits_[0] = addEdit(kTrackX, 630, 296, 64);
        trackEdits_[1] = addEdit(kTrackY, 702, 296, 64);
        trackEdits_[2] = addEdit(kTrackWidth, 774, 296, 64);
        trackEdits_[3] = addEdit(kTrackHeight, 846, 296, 64);
        addButton(L"Save template", kSaveTemplate, 630, 326, 132);
        addButton(L"Load template", kLoadTemplate, 774, 326, 132);

        statusCard_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME,
                                      24, 376, 900, 112, window_, nullptr, instance_, nullptr);
        addLabel(L"Session status", 42, 389, 160);
        addLabel(L"The app only captures frames and never sends input to the target. Scroll down manually in small increments.",
                 42, 414, 820);
        status_ = CreateWindowExW(0, L"STATIC", L"Select a target and capture a preview.",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                                  42, 443, 864, 36, window_, menuId(kStatus), instance_, nullptr);
        if (bodyFont_) SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);

        previewCard_ = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDFRAME,
                                       24, 500, 900, 220, window_, nullptr, instance_, nullptr);
        addLabel(L"Preview & calibration", 42, 514, 220);
        addLabel(L"Drag the green viewport or orange scrollbar rectangle to fine-tune it.", 300, 514, 560);
        previewCanvas_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"UniversalScrollStitcherPreview", nullptr,
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                         36, 542, 876, 170, window_, nullptr, instance_, this);

        loggingCheckbox_ = CreateWindowExW(0, L"BUTTON", L"Enable diagnostic logging",
                                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                           24, 740, 220, 28, window_, menuId(kLoggingEnable), instance_, nullptr);
        if (bodyFont_) SendMessageW(loggingCheckbox_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        openLogsButton_ = addButton(L"Open logs folder", kOpenLogs, 252, 736, 150);
        latestReleaseLink_ = CreateWindowExW(
            0, L"STATIC", L"Get the latest portable release from GitHub",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | SS_NOTIFY | SS_RIGHT,
            430, 740, 480, 28, window_, menuId(kLatestRelease), instance_, nullptr);
        latestReleaseFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, TRUE, FALSE,
                                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (latestReleaseFont_) SendMessageW(latestReleaseLink_, WM_SETFONT,
                                             reinterpret_cast<WPARAM>(latestReleaseFont_), TRUE);

        RECT client{};
        GetClientRect(window_, &client);
        layoutControls(client.right - client.left, client.bottom - client.top);
    }

    void layoutControls(int clientWidth, int clientHeight) {
        const int width = std::max(1, clientWidth);
        const int margin = 24;
        const int contentWidth = std::max(1, width - margin * 2);
        const int footerTop = std::max(0, clientHeight - 52);
        const int previewTop = 500;
        const int previewHeight = std::max(170, footerTop - previewTop - 12);
        const int buttonY = 184;
        if (title_) MoveWindow(title_, margin, 20, contentWidth, 38, TRUE);
        if (subtitle_) MoveWindow(subtitle_, margin, 58, contentWidth, 24, TRUE);
        if (targetCard_) MoveWindow(targetCard_, margin, 96, contentWidth, 76, TRUE);
        if (targetCombo_) MoveWindow(targetCombo_, 200, 112,
                                     std::max(250, width - 340), 260, TRUE);
        if (HWND refresh = GetDlgItem(window_, kRefresh)) MoveWindow(refresh,
            width - margin - 100, 110, 100, 34, TRUE);
        if (HWND preview = GetDlgItem(window_, kPreview)) MoveWindow(preview, margin, buttonY, 144, 34, TRUE);
        if (HWND autoDetect = GetDlgItem(window_, kAutoDetect)) MoveWindow(autoDetect, 180, buttonY, 190, 34, TRUE);
        if (HWND start = GetDlgItem(window_, kStart)) MoveWindow(start, 382, buttonY, 130, 34, TRUE);
        if (HWND stop = GetDlgItem(window_, kStop)) MoveWindow(stop, 524, buttonY, 100, 34, TRUE);
        if (HWND exportButton = GetDlgItem(window_, kExport)) MoveWindow(exportButton, 636, buttonY, 164, 34, TRUE);
        if (calibrationCard_) MoveWindow(calibrationCard_, margin, 232, contentWidth, 130, TRUE);
        if (HWND saveTemplate = GetDlgItem(window_, kSaveTemplate)) {
            MoveWindow(saveTemplate, 630, 326, 132, 30, TRUE);
        }
        if (HWND loadTemplate = GetDlgItem(window_, kLoadTemplate)) {
            MoveWindow(loadTemplate, 774, 326, 132, 30, TRUE);
        }
        if (statusCard_) MoveWindow(statusCard_, margin, 376, contentWidth, 112, TRUE);
        if (status_) MoveWindow(status_, 42, 443, std::max(400, width - 84), 36, TRUE);
        if (previewCard_) MoveWindow(previewCard_, margin, previewTop, contentWidth, previewHeight, TRUE);
        if (previewCanvas_) MoveWindow(previewCanvas_, 36, previewTop + 42,
                                       std::max(200, width - 72), std::max(100, previewHeight - 54), TRUE);
        if (loggingCheckbox_) MoveWindow(loggingCheckbox_, margin, footerTop + 10, 220, 28, TRUE);
        if (openLogsButton_) MoveWindow(openLogsButton_, 252, footerTop + 7, 150, 34, TRUE);
        if (latestReleaseLink_) MoveWindow(latestReleaseLink_, std::max(420, width - 500),
                                           footerTop + 10, std::min(476, width - 444), 28, TRUE);
    }

    LRESULT colorStatic(HWND control, HDC dc) const {
        SetBkMode(dc, TRANSPARENT);
        if (control == latestReleaseLink_) {
            SetTextColor(dc, RGB(24, 96, 190));
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }
        if (control == title_) {
            SetTextColor(dc, RGB(20, 42, 74));
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }
        if (control == subtitle_) {
            SetTextColor(dc, RGB(84, 101, 122));
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }
        if (control == status_) {
            SetTextColor(dc, RGB(28, 62, 103));
            SetBkMode(dc, OPAQUE);
            return reinterpret_cast<LRESULT>(statusBrush_);
        }
        if (control == targetCard_ || control == calibrationCard_ || control == statusCard_ || control == previewCard_) {
            SetBkMode(dc, OPAQUE);
            return reinterpret_cast<LRESULT>(cardBrush_);
        }
        SetTextColor(dc, RGB(40, 54, 72));
        return reinterpret_cast<LRESULT>(cardBrush_);
    }

    void drawButton(const DRAWITEMSTRUCT* item) const {
        if (!item || item->CtlType != ODT_BUTTON) return;
        const int id = static_cast<int>(item->CtlID);
        const bool primary = id == kStart || id == kExport;
        COLORREF fill = primary ? RGB(35, 108, 224) : RGB(235, 241, 249);
        COLORREF text = primary ? RGB(255, 255, 255) : RGB(31, 56, 86);
        if (item->itemState & ODS_DISABLED) {
            fill = RGB(222, 227, 234);
            text = RGB(136, 146, 160);
        } else if (item->itemState & ODS_SELECTED) {
            fill = primary ? RGB(24, 82, 180) : RGB(211, 222, 237);
        }
        RECT rect = item->rcItem;
        InflateRect(&rect, -1, -1);
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, primary ? fill : RGB(199, 211, 228));
        HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
        HGDIOBJ oldPen = SelectObject(item->hDC, pen);
        RoundRect(item->hDC, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
        SelectObject(item->hDC, oldPen);
        SelectObject(item->hDC, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, text);
        RECT textRect = rect;
        wchar_t caption[128]{};
        GetWindowTextW(item->hwndItem, caption, static_cast<int>(std::size(caption)));
        DrawTextW(item->hDC, caption, -1, &textRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        // Owner-drawn controls remain keyboard-accessible; make focus visible
        // without changing their native tab behavior.
        if (item->itemState & ODS_FOCUS) {
            RECT focus = rect;
            InflateRect(&focus, -4, -4);
            DrawFocusRect(item->hDC, &focus);
        }
    }

    static std::string narrow(const std::wstring& value) {
        if (value.empty()) return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                             nullptr, 0, nullptr, nullptr);
        if (size <= 0) {
            std::string fallback;
            fallback.reserve(value.size());
            for (const wchar_t character : value) {
                fallback.push_back(character >= 0 && character <= 0x7f
                    ? static_cast<char>(character) : '?');
            }
            return fallback;
        }
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                            result.data(), size, nullptr, nullptr);
        return result;
    }

    static std::string rectDescription(const Rect& rect) {
        return "x=" + std::to_string(rect.x) + " y=" + std::to_string(rect.y) +
               " w=" + std::to_string(rect.width) + " h=" + std::to_string(rect.height);
    }

    static std::string windowDescription(HWND window) {
        if (!IsWindow(window)) return "handle=0x0 title=<closed>";
        wchar_t title[512]{};
        GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        std::ostringstream result;
        result << "handle=0x" << std::hex << reinterpret_cast<std::uintptr_t>(window)
               << " title=" << narrow(title);
        return result.str();
    }

    void toggleLogging() {
        const bool requested = SendMessageW(loggingCheckbox_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (requested) {
            if (!logger_.enable()) {
                SendMessageW(loggingCheckbox_, BM_SETCHECK, BST_UNCHECKED, 0);
                const std::string error = logger_.lastError();
                setStatus(L"Diagnostic logging could not be enabled: " + widen(error));
                return;
            }
            logger_.info("ui", "logging enable checkbox checked");
            logger_.info("app", "startup diagnostics activated after UI initialization");
            const std::wstring path = logger_.sessionPath().wstring();
            setStatus(L"Diagnostic logging enabled. Session log: " + path);
            return;
        }
        if (logger_.enabled()) logger_.info("ui", "logging enable checkbox unchecked");
        logger_.disable();
        setStatus(L"Diagnostic logging disabled. No capture pixels are stored.");
    }

    void openLogs() {
        logger_.info("ui", "open logs folder button clicked");
        if (!logger_.openLogsFolder()) {
            setStatus(L"Could not open the logs folder: " + widen(logger_.lastError()));
            return;
        }
        const std::wstring path = logger_.logsDirectory().wstring();
        if (logger_.enabled()) setStatus(L"Logs folder opened. Active session log: " + logger_.sessionPath().wstring());
        else setStatus(L"Logs folder opened: " + path);
    }

    void refreshTargets() {
        logger_.info("ui", "refresh targets requested");
        windows_.clear();
        std::pair<MainWindow*, std::vector<WindowInfo>*> context{this, &windows_};
        EnumWindows(&MainWindow::enumerateWindow, reinterpret_cast<LPARAM>(&context));
        SendMessageW(targetCombo_, CB_RESETCONTENT, 0, 0);
        for (const auto& entry : windows_) {
            const LRESULT index = SendMessageW(targetCombo_, CB_ADDSTRING, 0,
                                               reinterpret_cast<LPARAM>(entry.title.c_str()));
            SendMessageW(targetCombo_, CB_SETITEMDATA, index, reinterpret_cast<LPARAM>(entry.handle));
            logger_.info("target", "discovered " + windowDescription(entry.handle));
        }
        if (!windows_.empty()) SendMessageW(targetCombo_, CB_SETCURSEL, 0, 0);
        logger_.info("target", "discovered window count=" + std::to_string(windows_.size()));
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
        const bool previousUpdating = updatingCalibrationEdits_;
        updatingCalibrationEdits_ = true;
        setValue(edits[0], rect.x); setValue(edits[1], rect.y);
        setValue(edits[2], rect.width); setValue(edits[3], rect.height);
        updatingCalibrationEdits_ = previousUpdating;
    }

    bool applyCalibrationProfile(const CalibrationProfile& saved, bool announce) {
        if (preview_.empty()) return false;
        const CalibrationProfile profile = saved.scaledTo(preview_.cols, preview_.rows);
        if (!profile.valid()) {
            setStatus(L"The template could not be scaled to the current preview size.");
            return false;
        }
        scrollbarCandidates_.clear();
        selectedScrollbarCandidate_.reset();
        scrollbarSide_ = profile.scrollbar.side;
        setRect(viewportEdits_, profile.content);
        setRect(trackEdits_, profile.scrollbar.track);
        logger_.info("calibration", "template applied reference=" +
                     std::to_string(saved.frameWidth) + "x" + std::to_string(saved.frameHeight) +
                     " current=" + std::to_string(profile.frameWidth) + "x" +
                     std::to_string(profile.frameHeight) + " content{" +
                     rectDescription(profile.content) + "} scrollbar{" +
                     rectDescription(profile.scrollbar.track) + "}");
        InvalidateRect(previewCanvas_, nullptr, FALSE);
        if (announce) {
            const bool scaled = saved.frameWidth != profile.frameWidth ||
                                saved.frameHeight != profile.frameHeight;
            setStatus(scaled
                ? L"Template loaded and scaled to the current preview. The green content and orange scrollbar areas were restored."
                : L"Template loaded. The green content and orange scrollbar areas were restored.");
        }
        return true;
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

    void selectScrollbarCandidate(std::size_t index, bool announce) {
        if (index >= scrollbarCandidates_.size()) return;
        selectedScrollbarCandidate_ = index;
        const auto& candidate = scrollbarCandidates_[index];
        scrollbarSide_ = candidate.config.side;
        logger_.info("calibration", "selected scrollbar candidate #" + std::to_string(index + 1) +
                     " track{" + rectDescription(candidate.config.track) + "} thumb{" +
                     rectDescription(candidate.observation.thumb) + "} confidence=" +
                     std::to_string(candidate.observation.confidence));
        setRect(trackEdits_, scrollbarCandidates_[index].config.track);
        InvalidateRect(previewCanvas_, nullptr, FALSE);
        if (announce) {
            const int confidence = static_cast<int>(std::lround(
                scrollbarCandidates_[index].observation.confidence * 100.0F));
            setStatus(L"Selected scrollbar (confidence " + std::to_wstring(confidence) +
                      L"%). Drag the orange rectangle to fine-tune it if needed.");
        }
    }

    [[nodiscard]] std::optional<std::size_t> scrollbarCandidateAt(const POINT& point,
                                                                  int radius) const {
        std::optional<std::size_t> best;
        int bestDistance = std::numeric_limits<int>::max();
        for (std::size_t index = 0; index < scrollbarCandidates_.size(); ++index) {
            if (selectedScrollbarCandidate_ && *selectedScrollbarCandidate_ == index) continue;
            int ignoredEdges = EdgeNone;
            const Rect& track = scrollbarCandidates_[index].config.track;
            if (!hitRect(track, point, radius, ignoredEdges)) continue;
            const int center = track.x + track.width / 2;
            const int distance = std::abs(point.x - center);
            if (distance < bestDistance) {
                best = index;
                bestDistance = distance;
            }
        }
        return best;
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
        HPEN candidatePen = CreatePen(PS_DOT, 1, RGB(40, 180, 255));
        HPEN viewportPen = CreatePen(PS_SOLID, 2, RGB(50, 220, 90));
        HPEN trackPen = CreatePen(PS_SOLID, 2, RGB(255, 170, 30));
        HGDIOBJ oldPen = SelectObject(dc, candidatePen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(40, 180, 255));
        for (std::size_t index = 0; index < scrollbarCandidates_.size(); ++index) {
            if (selectedScrollbarCandidate_ && *selectedScrollbarCandidate_ == index) continue;
            const RECT candidateRect = scaledRect(scrollbarCandidates_[index].config.track, *transform);
            Rectangle(dc, candidateRect.left, candidateRect.top,
                      candidateRect.right, candidateRect.bottom);
            const int confidence = static_cast<int>(std::lround(
                scrollbarCandidates_[index].observation.confidence * 100.0F));
            const std::wstring label = L"#" + std::to_wstring(index + 1) + L" " +
                                       std::to_wstring(confidence) + L"%";
            const LONG labelTop = std::min(candidateRect.bottom - 20,
                                           candidateRect.top + 3 +
                                               static_cast<LONG>(index % 12) * 18);
            RECT labelRect{candidateRect.left + 3, labelTop,
                           candidateRect.right + 100, labelTop + 19};
            DrawTextW(dc, label.c_str(), -1, &labelRect,
                      DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        SelectObject(dc, viewportPen);
        Rectangle(dc, viewportRect.left, viewportRect.top, viewportRect.right, viewportRect.bottom);
        SelectObject(dc, trackPen);
        Rectangle(dc, trackRect.left, trackRect.top, trackRect.right, trackRect.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(viewportPen);
        DeleteObject(trackPen);
        DeleteObject(candidatePen);

        SetTextColor(dc, RGB(50, 220, 90));
        RECT viewportLabel{viewportRect.left + 4, viewportRect.top + 3, viewportRect.right - 4, viewportRect.top + 22};
        DrawTextW(dc, L"CONTENT VIEWPORT", -1, &viewportLabel, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(dc, RGB(255, 170, 30));
        RECT trackLabel{trackRect.left + 4, trackRect.top + 3, trackRect.right + 150, trackRect.top + 22};
        const std::wstring trackText = selectedScrollbarCandidate_
            ? L"SCROLLBAR SELECTED"
            : L"SCROLLBAR TRACK (MANUAL)";
        DrawTextW(dc, trackText.c_str(), -1, &trackLabel,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void beginPreviewDrag(HWND canvas, int clientX, int clientY) {
        POINT imagePoint{};
        if (!clientToImage(canvas, clientX, clientY, imagePoint)) return;
        const int radius = std::max(4, static_cast<int>(std::ceil(7.0 / displayTransform(canvas)->scale)));
        const Rect viewport = readRect(viewportEdits_);
        const Rect track = readRect(trackEdits_);
        int trackEdges = EdgeNone;
        int viewportEdges = EdgeNone;
        if (const auto candidate = scrollbarCandidateAt(imagePoint, radius)) {
            selectScrollbarCandidate(*candidate, true);
            return;
        }
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
        if (dragTarget_ == CanvasTarget::Viewport) {
            logger_.info("calibration", "viewport drag finished with{" + rectDescription(readRect(viewportEdits_)) + "}");
        } else if (dragTarget_ == CanvasTarget::Track) {
            logger_.info("calibration", "scrollbar track drag finished with{" + rectDescription(readRect(trackEdits_)) + "}");
        }
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
        case WM_SIZE:
            InvalidateRect(canvas, nullptr, TRUE);
            return 0;
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
            logger_.warning("target", "selected target is no longer a valid window");
            setStatus(L"Choose a valid visible top-level window first.");
            return false;
        }
        logger_.info("target", "using " + windowDescription(target_));
        if (!source_ || source_->width() == 0 || sourceTarget_ != target_) {
            logger_.info("capture", "initializing frame source for " + windowDescription(target_));
            source_ = createFrameSource(target_);
            sourceTarget_ = target_;
        }
        if (!source_) {
            logger_.error("capture", "frame source initialization failed; graphics capture and GDI fallback unavailable");
            setStatus(L"Unable to initialize Windows Graphics Capture or visible-window fallback.");
            return false;
        }
        const std::wstring sourceName = source_->name();
        logger_.info("capture", "frame source ready name=" + narrow(sourceName) +
                     (sourceName.find(L"GDI") != std::wstring::npos ? " (fallback)" : "") +
                     " dimensions=" + std::to_string(source_->width()) + "x" + std::to_string(source_->height()));
        return true;
    }

    void capturePreview() {
        logger_.info("preview", "preview capture requested");
        if (capturing_) {
            logger_.warning("preview", "preview rejected while capture is active");
            setStatus(L"Stop the current capture before recapturing a preview.");
            return;
        }
        target_ = selectedTarget();
        if (!IsWindow(target_)) {
            logger_.warning("preview", "preview rejected because target is invalid");
            setStatus(L"Choose a valid visible top-level window first.");
            return;
        }
        // A preview belongs to exactly one target and calibration. Do not let
        // a failed recapture leave the previous target's image/coordinates in
        // place, and discard any finalized session tied to that image.
        preview_.release();
        scrollbarCandidates_.clear();
        selectedScrollbarCandidate_.reset();
        session_.reset();
        InvalidateRect(previewCanvas_, nullptr, TRUE);
        source_ = createFrameSource(target_);
        sourceTarget_ = target_;
        if (!source_) {
            logger_.error("preview", "unable to initialize frame source for " + windowDescription(target_));
            setStatus(L"Unable to capture the selected window.");
            return;
        }
        const std::wstring previewSourceName = source_->name();
        logger_.info("capture", "preview frame source ready name=" + narrow(previewSourceName) +
                     (previewSourceName.find(L"GDI") != std::wstring::npos ? " (fallback)" : "") +
                     " dimensions=" + std::to_string(source_->width()) + "x" + std::to_string(source_->height()));
        cv::Mat captured;
        for (int attempt = 0; attempt < 8 && captured.empty(); ++attempt) {
            logger_.info("preview", "preview frame attempt=" + std::to_string(attempt + 1));
            const auto frame = source_->capture();
            if (frame) captured = frame->bgra.clone();
            Sleep(20);
        }
        if (captured.empty()) {
            logger_.warning("preview", "preview capture returned no frame after 8 attempts");
            setStatus(L"The capture source did not return a frame yet; try Capture preview again.");
            return;
        }
        preview_ = std::move(captured);
        // Once a preview source exists, keep the canvas live even before and
        // after stitching. Static WGC targets simply yield no queued frame,
        // so this timer is inexpensive while nothing changes.
        SetTimer(window_, kTimer, kTimerPeriodMs, nullptr);
        logger_.info("preview", "preview succeeded source=" + narrow(source_->name()) +
                     " dimensions=" + std::to_string(preview_.cols) + "x" + std::to_string(preview_.rows));
        const auto detectedCandidates = ScrollbarDetector::autoDetectAll(preview_);
        logger_.info("calibration", "auto-detected scrollbar candidates=" +
                     std::to_string(detectedCandidates.size()));
        for (std::size_t index = 0; index < detectedCandidates.size(); ++index) {
            const auto& candidate = detectedCandidates[index];
            logger_.info("calibration", "candidate #" + std::to_string(index + 1) +
                         " track{" + rectDescription(candidate.config.track) + "} thumb{" +
                         rectDescription(candidate.observation.thumb) + "} confidence=" +
                         std::to_string(candidate.observation.confidence) + " selection_score=" +
                         std::to_string(candidate.autoDetectionScore));
        }
        // Keep the preview unambiguous: automatic ranking selects exactly one
        // scrollbar. The full list above is diagnostics-only.
        if (!detectedCandidates.empty()) scrollbarCandidates_.push_back(detectedCandidates.front());
        if (!scrollbarCandidates_.empty()) {
            selectScrollbarCandidate(0, false);
            // The content viewport must exclude the scrollbar strip; baking
            // the thumb into every strip leaves a jumping column through the
            // entire capture.
            const Rect& track = scrollbarCandidates_.front().config.track;
            Rect viewport{0, track.y, preview_.cols, track.height};
            if (track.x + track.width / 2 >= preview_.cols / 2) {
                viewport.width = std::max(16, track.x);
            } else {
                viewport.x = std::min(preview_.cols - 16, track.right());
                viewport.width = std::max(16, preview_.cols - viewport.x);
            }
            setRect(viewportEdits_, viewport);
        } else {
            setRect(viewportEdits_, {0, 0, preview_.cols, preview_.rows});
            setRect(trackEdits_, {std::max(0, preview_.cols - 18), 0, 18, preview_.rows});
        }
        if (pendingCalibrationProfile_) {
            const CalibrationProfile loaded = *pendingCalibrationProfile_;
            pendingCalibrationProfile_.reset();
            if (applyCalibrationProfile(loaded, true)) return;
        }
        std::wstring message = L"Preview captured by " + source_->name() + L" (" +
            std::to_wstring(preview_.cols) + L"x" + std::to_wstring(preview_.rows) + L"). ";
        if (!scrollbarCandidates_.empty()) {
            const int confidence = static_cast<int>(std::lround(
                scrollbarCandidates_.front().observation.confidence * 100.0F));
            message += L"The best scrollbar was selected (confidence " +
                       std::to_wstring(confidence) +
                       L"%). Only the selected scrollbar is shown.";
        } else {
            message += L"No scrollbar candidate found; set the orange track manually.";
        }
        setStatus(message);
        InvalidateRect(previewCanvas_, nullptr, TRUE);
    }

    void autoDetectScrollbar() {
        logger_.info("calibration", "auto-detect scrollbar requested");
        if (preview_.empty()) capturePreview();
        if (preview_.empty()) return;
        const auto detectedCandidates = ScrollbarDetector::autoDetectAll(preview_);
        logger_.info("calibration", "auto-detect completed candidates=" + std::to_string(detectedCandidates.size()));
        scrollbarCandidates_.clear();
        selectedScrollbarCandidate_.reset();
        if (detectedCandidates.empty()) {
            logger_.warning("calibration", "auto-detect found no scrollbar candidate");
            setStatus(L"No scrollbar candidate was found. Set the track rectangle manually.");
            InvalidateRect(previewCanvas_, nullptr, FALSE);
            return;
        }
        for (std::size_t index = 0; index < detectedCandidates.size(); ++index) {
            const auto& candidate = detectedCandidates[index];
            logger_.info("calibration", "candidate #" + std::to_string(index + 1) +
                         " track{" + rectDescription(candidate.config.track) + "} thumb{" +
                         rectDescription(candidate.observation.thumb) + "} confidence=" +
                         std::to_string(candidate.observation.confidence) + " selection_score=" +
                         std::to_string(candidate.autoDetectionScore));
        }
        scrollbarCandidates_.push_back(detectedCandidates.front());
        selectScrollbarCandidate(0, false);
        const int confidence = static_cast<int>(std::lround(
            scrollbarCandidates_.front().observation.confidence * 100.0F));
        setStatus(L"The best scrollbar was selected (confidence " +
                  std::to_wstring(confidence) +
                  L"%). Only the orange selected rectangle is shown; drag it to adjust.");
    }

    void startCapture() {
        logger_.info("capture", "start requested");
        if (capturing_) return;
        if (preview_.empty() || selectedTarget() != sourceTarget_) capturePreview();
        if (!ensureSource()) return;
        const int calibratedWidth = preview_.cols;
        const int calibratedHeight = preview_.rows;
        cv::Mat firstFrame;
        int firstFrameAttempts = 0;
        for (; firstFrameAttempts < 8 && firstFrame.empty(); ++firstFrameAttempts) {
            const auto captured = source_->capture();
            if (captured) {
                firstFrame = captured->bgra.clone();
                break;
            }
            if (firstFrameAttempts + 1 < 8) Sleep(20);
        }
        if (firstFrame.empty()) {
            // An unchanged WGC target may have an empty compositor queue. The
            // calibrated preview is the last known complete frame and is a
            // safe baseline when dimensions still match.
            if (!preview_.empty() && source_->width() == preview_.cols &&
                source_->height() == preview_.rows) {
                firstFrame = preview_.clone();
                logger_.warning("capture", "first frame queue remained empty after retries; "
                                "using the calibrated preview as the baseline");
            } else {
                logger_.error("capture", "first frame unavailable after retries=" +
                              std::to_string(firstFrameAttempts));
                setStatus(L"Could not obtain the first frame; capture a preview again.");
                return;
            }
        }
        if ((calibratedWidth > 0 && firstFrame.cols != calibratedWidth) ||
            (calibratedHeight > 0 && firstFrame.rows != calibratedHeight)) {
            logger_.warning("capture", "target resized before start from=" + std::to_string(calibratedWidth) +
                            "x" + std::to_string(calibratedHeight) + " to=" +
                            std::to_string(firstFrame.cols) + "x" + std::to_string(firstFrame.rows));
            preview_ = firstFrame;
            InvalidateRect(previewCanvas_, nullptr, TRUE);
            session_.reset();
            setStatus(L"The target changed size; capture a new preview and recalibrate.");
            return;
        }
        preview_ = std::move(firstFrame);
        InvalidateRect(previewCanvas_, nullptr, FALSE);
        StitchOptions options;
        options.viewport = readRect(viewportEdits_);
        options.scrollbar.track = readRect(trackEdits_);
        options.scrollbar.side = scrollbarSide_;
        options.scrollbar.enabled = true;
        logger_.info("capture", "start options viewport{" + rectDescription(options.viewport) +
                     "} scrollbar{" + rectDescription(options.scrollbar.track) + "} preview=" +
                     std::to_string(preview_.cols) + "x" + std::to_string(preview_.rows));
        if (!options.viewport.valid() || !options.scrollbar.track.valid()) {
            logger_.error("capture", "start rejected because calibration rectangle is invalid");
            setStatus(L"Viewport and scrollbar track must have positive width and height.");
            return;
        }
        if (!session_.start(preview_, options)) {
            logger_.error("capture", "stitch session start failed: " + session_.lastMessage());
            setStatus(widen(session_.lastMessage()));
            return;
        }
        if (session_.viewport().y != options.viewport.y ||
            session_.viewport().height != options.viewport.height) {
            logger_.info("capture", "static mask adjusted effective viewport{" +
                         rectDescription(session_.viewport()) + "}");
        }
        capturing_ = true;
        captureTickCount_ = 0;
        transientMissCount_ = 0;
        logger_.info("capture", "capture started");
        SetTimer(window_, kTimer, kTimerPeriodMs, nullptr);
        setStatus(L"Capturing. Scroll the target down manually; no input is injected. "
                  L"Prefer small mouse-wheel notches or the down arrow.");
    }

    void stopCapture() {
        logger_.info("capture", "stop requested capturing=" + std::to_string(capturing_ ? 1 : 0));
        if (!capturing_ && session_.state() != SessionState::Paused &&
            session_.state() != SessionState::Capturing) return;
        capturing_ = false;
        if (session_.finish()) {
            logger_.info("capture", "capture finalized accepted_frames=" + std::to_string(session_.acceptedFrames()) +
                         " output_rows=" + std::to_string(session_.outputRows()));
            setStatus(L"Finalized " + std::to_wstring(session_.outputStore().width()) + L"x" +
                      std::to_wstring(session_.outputStore().rows()) +
                      L" in disk-backed storage. Choose Export PNG/JPEG.");
        } else {
            logger_.error("capture", "capture finalize failed: " + session_.lastMessage());
            setStatus(widen(session_.lastMessage()));
        }
    }

    void captureTick() {
        if (!source_) return;
        if (!capturing_) {
            const auto frame = source_->capture();
            if (!frame) return;
            if (!preview_.empty() && frame->width() == preview_.cols &&
                frame->height() == preview_.rows) {
                preview_ = frame->bgra;
                InvalidateRect(previewCanvas_, nullptr, FALSE);
            }
            return;
        }
        ++captureTickCount_;

        bool sawFrame = false;
        StitchUpdate lastUpdate;
        // Windows Graphics Capture exposes a real queue worth draining. The
        // GDI fallback synthesizes a fresh full-window capture on every call,
        // so invoking it repeatedly here only burns CPU on duplicate frames.
        const int maximumFrames = dynamic_cast<GraphicsCaptureSource*>(source_.get())
            ? kMaximumFramesPerTick : 1;
        for (int drained = 0; drained < maximumFrames; ++drained) {
            const auto frame = source_->capture();
            if (!frame) break;
            sawFrame = true;
            if (frame->width() != preview_.cols || frame->height() != preview_.rows) {
                capturing_ = false;
                KillTimer(window_, kTimer);
                setStatus(L"Target size changed during capture; click Stop to finalize the valid prefix or recapture.");
                logger_.warning("capture", "capture frame dimensions changed from=" + std::to_string(preview_.cols) +
                                "x" + std::to_string(preview_.rows) + " to=" + std::to_string(frame->width()) +
                                "x" + std::to_string(frame->height()));
                return;
            }

            // Keep the calibration canvas live during capture. cv::Mat's
            // reference counting keeps these pixels alive after the optional
            // CaptureFrame leaves scope; invalidations coalesce when several
            // queued frames are drained in one tick.
            preview_ = frame->bgra;
            InvalidateRect(previewCanvas_, nullptr, FALSE);
            lastUpdate = session_.process(frame->bgra);
            if (lastUpdate.accepted || lastUpdate.rejected || lastUpdate.paused) {
                logger_.info("stitch", "update accepted=" + std::to_string(lastUpdate.accepted ? 1 : 0) +
                             " rejected=" + std::to_string(lastUpdate.rejected ? 1 : 0) +
                             " paused=" + std::to_string(lastUpdate.paused ? 1 : 0) +
                             " top=" + std::to_string(lastUpdate.atTop ? 1 : 0) +
                             " bottom=" + std::to_string(lastUpdate.atBottom ? 1 : 0) +
                             " scrollbar_visible=" + std::to_string(lastUpdate.scrollbarVisible ? 1 : 0) +
                             " shift=" + std::to_string(lastUpdate.shift) +
                             " expected_shift=" + std::to_string(lastUpdate.expectedShift) +
                             " consecutive_rejections=" + std::to_string(lastUpdate.consecutiveRejections) +
                             " visual_confidence=" + std::to_string(lastUpdate.confidence) +
                             " visual_margin=" + std::to_string(lastUpdate.margin) +
                             " scrollbar_confidence=" + std::to_string(lastUpdate.scrollbarConfidence) +
                             " message=" + lastUpdate.message);
            }
            if (lastUpdate.paused) {
                capturing_ = false;
                setStatus(std::wstring(L"PAUSED: ") + widen(lastUpdate.message));
                return;
            }
        }

        if (!sawFrame) {
            ++transientMissCount_;
            if (transientMissCount_ <= 3 || transientMissCount_ % 25 == 0) {
                logger_.warning("capture", "transient frame miss tick=" + std::to_string(captureTickCount_) +
                                " count=" + std::to_string(transientMissCount_));
            }
            if (!IsWindow(target_)) {
                capturing_ = false;
                KillTimer(window_, kTimer);
                setStatus(L"The target window closed; click Stop to finalize the valid prefix.");
                logger_.error("capture", "target window closed during capture");
            } else if (source_->width() != preview_.cols || source_->height() != preview_.rows) {
                capturing_ = false;
                KillTimer(window_, kTimer);
                setStatus(L"Target size changed during capture; click Stop to finalize the valid prefix or recapture.");
                logger_.warning("capture", "target resized or source dimensions changed during capture");
            }
            // Windows Graphics Capture can have an empty frame queue between
            // compositor updates. Treat that as a transient miss, not a
            // failed capture.
            return;
        }

        transientMissCount_ = 0;
        if (lastUpdate.accepted) {
            std::wstring message = widen(lastUpdate.message) +
                L" (shift " + std::to_wstring(lastUpdate.shift) + L", confidence " +
                std::to_wstring(lastUpdate.confidence).substr(0, 5) + L")";
            setStatus(message);
            return;
        }
        // The session is still live and still holding the last accepted frame,
        // so the capture keeps running while the user recovers the overlap.
        if (lastUpdate.rejected) {
            setStatus(L"RECOVERING: " + widen(lastUpdate.message) +
                      L" (mouse wheel may be jumping too far — try slower notches or the down arrow)");
        }
    }

    void saveTemplate() {
        logger_.info("calibration", "save template requested");
        if (capturing_) {
            setStatus(L"Stop the current capture before saving a calibration template.");
            return;
        }
        if (preview_.empty()) {
            setStatus(L"Capture a preview and calibrate the two areas before saving a template.");
            return;
        }
        CalibrationProfile profile;
        profile.frameWidth = preview_.cols;
        profile.frameHeight = preview_.rows;
        profile.content = readRect(viewportEdits_);
        profile.scrollbar.track = readRect(trackEdits_);
        profile.scrollbar.side = scrollbarSide_;
        profile.scrollbar.enabled = true;
        if (!profile.valid()) {
            setStatus(L"The content or scrollbar area is outside the preview. Adjust both rectangles before saving.");
            return;
        }

        wchar_t filename[MAX_PATH] = L"scroll_stitch_template.ussconfig";
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"Scroll Stitcher template (*.ussconfig)\0*.ussconfig\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
        dialog.lpstrDefExt = L"ussconfig";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) {
            logger_.info("calibration", "save template dialog cancelled");
            return;
        }
        std::filesystem::path path(filename);
        if (path.extension().empty()) path += L".ussconfig";
        std::string error;
        if (!saveCalibrationProfile(path, profile, error)) {
            logger_.error("calibration", "template save failed path=" + narrow(path.wstring()) +
                          " error=" + error);
            setStatus(L"Could not save the template: " + widen(error));
            return;
        }
        logger_.info("calibration", "template saved path=" + narrow(path.wstring()));
        setStatus(L"Template saved: " + path.wstring());
    }

    void loadTemplate() {
        logger_.info("calibration", "load template requested");
        if (capturing_) {
            setStatus(L"Stop the current capture before loading a calibration template.");
            return;
        }
        wchar_t filename[MAX_PATH]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"Scroll Stitcher template (*.ussconfig)\0*.ussconfig\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
        dialog.lpstrDefExt = L"ussconfig";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) {
            logger_.info("calibration", "load template dialog cancelled");
            return;
        }
        std::string error;
        const auto loaded = loadCalibrationProfile(filename, error);
        if (!loaded) {
            logger_.error("calibration", "template load failed path=" + narrow(filename) +
                          " error=" + error);
            setStatus(L"Could not load the template: " + widen(error));
            return;
        }
        logger_.info("calibration", "template loaded path=" + narrow(filename));
        if (preview_.empty()) {
            pendingCalibrationProfile_ = *loaded;
            setStatus(L"Template loaded. Capture a preview and its two areas will be restored automatically.");
            return;
        }
        applyCalibrationProfile(*loaded, true);
    }

    void exportImage() {
        logger_.info("export", "export requested");
        if (capturing_) stopCapture();
        if (!session_.hasOutput()) {
            logger_.warning("export", "export rejected because no finalized output exists");
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
        if (!GetSaveFileNameW(&dialog)) {
            logger_.info("export", "export dialog cancelled");
            return;
        }
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
        logger_.info("export", "export path requested=" + narrow(outputPath.wstring()) +
                     " success=" + std::to_string(result.success ? 1 : 0) +
                     " files=" + std::to_string(result.files.size()));
        if (!result.success) {
            logger_.error("export", "export failed: " + result.message);
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
        case kSaveTemplate: saveTemplate(); break;
        case kLoadTemplate: loadTemplate(); break;
        case kLoggingEnable: toggleLogging(); break;
        case kOpenLogs: openLogs(); break;
        default: break;
        }
    }

    void setStatus(const std::wstring& text) {
        std::wstring visibleText = text;
        const std::string loggingError = logger_.lastError();
        if (!loggingError.empty()) {
            visibleText += L"\nLogging warning: " + widen(loggingError);
        }
        if (status_) SetWindowTextW(status_, visibleText.c_str());
        logger_.info("status", narrow(text));
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
