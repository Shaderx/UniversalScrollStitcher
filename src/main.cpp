#include "universal_stitcher/ImageExporter.h"
#include "universal_stitcher/CalibrationProfile.h"
#include "universal_stitcher/DiagnosticLogger.h"
#include "universal_stitcher/StitchSession.h"
#include "universal_stitcher/WindowCapture.h"

#include <opencv2/core.hpp>

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <dwmapi.h>

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
    kMaximumJump = 140,
    kScrollbarCandidateCombo = 141,
    kSetScrollbarCandidate = 142,
    kStatus = 150,
    kLoggingEnable = 151,
    kOpenLogs = 152,
    kLatestRelease = 153,
    kAdvanced = 160,
    kNewCapture = 161,
    kRetryPreview = 162
};

constexpr wchar_t kLatestReleaseUrl[] =
    L"https://github.com/Shaderx/UniversalScrollStitcher/releases/latest";

struct WindowInfo {
    HWND handle = nullptr;
    std::wstring title;
};

class MainWindow final {
public:
    bool translateUiMessage(MSG& message) {
        if (handlePreviewShortcut(message)) return true;
        if (message.message == WM_KEYDOWN && message.wParam == VK_TAB) {
            const HWND root = GetAncestor(message.hwnd, GA_ROOT);
            if (root == window_ || root == previewCanvas_) {
                const bool handled = IsDialogMessageW(root, &message) != FALSE;
                if (handled && root == window_) {
                    RECT focus{}, client{};
                    GetWindowRect(GetFocus(), &focus);
                    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&focus), 2);
                    GetClientRect(window_, &client);
                    if (focus.top < 0) scrollOffset_ += focus.top - px(12);
                    else if (focus.bottom > client.bottom) scrollOffset_ += focus.bottom - client.bottom + px(12);
                    layoutControls(client.right, client.bottom);
                }
                return handled;
            }
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN) {
            wchar_t className[32]{};
            GetClassNameW(message.hwnd, className, 32);
            if (wcscmp(className, L"Button") == 0) {
                SendMessageW(message.hwnd, BM_CLICK, 0, 0);
                return true;
            }
        }
        if (message.message == WM_KEYDOWN && GetAncestor(message.hwnd, GA_ROOT) == previewCanvas_ &&
            (message.wParam == VK_ESCAPE || (message.wParam == 'Z' && GetKeyState(VK_CONTROL) < 0))) {
            SendMessageW(previewCanvas_, message.message, message.wParam, message.lParam);
            return true;
        }
        return false;
    }

    bool create(HINSTANCE instance) {
        instance_ = instance;
        backgroundBrush_ = CreateSolidBrush(RGB(20, 23, 29));
        cardBrush_ = CreateSolidBrush(RGB(31, 36, 45));
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
                                  WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                  540, 800, nullptr, nullptr, instance, this);
        if (!window_) return false;
        // CreateWindow's dimensions are physical pixels in a per-monitor-DPI
        // process. Size the initial client area using the window's actual DPI
        // before showing it, so scaled controls do not open in a 96-DPI shell.
        RECT initial{0, 0, px(520), px(752)};
        AdjustWindowRectExForDpi(&initial, WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                                FALSE, 0, uiDpi_);
        MONITORINFO monitor{sizeof(monitor)};
        if (GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            RECT bounds{};
            GetWindowRect(window_, &bounds);
            const int width = std::min<int>(initial.right - initial.left,
                                           monitor.rcWork.right - monitor.rcWork.left);
            const int height = std::min<int>(initial.bottom - initial.top,
                                            monitor.rcWork.bottom - monitor.rcWork.top);
            SetWindowPos(window_, nullptr,
                std::clamp<int>(bounds.left, monitor.rcWork.left, monitor.rcWork.right - width),
                std::clamp<int>(bounds.top, monitor.rcWork.top, monitor.rcWork.bottom - height),
                width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
        return true;
    }

private:
    friend struct PreviewInteractionTests;
    friend struct UiWorkflowTests;
    static constexpr UINT_PTR kTimer = 1;
    // Poll the WGC queue at roughly 60 Hz so accelerated wheel bursts retain
    // more intermediate views. The source itself controls the actual frame
    // rate; this timer only establishes the maximum polling cadence.
    static constexpr UINT kTimerPeriodMs = 16;
    // Bound work per UI tick so a burst cannot starve preview painting. The
    // WGC pool retains eight frames and the next tick continues draining it.
    static constexpr int kMaximumFramesPerTick = 4;
    static constexpr int kMinimumJumpPercent = 50;
    static constexpr int kMaximumJumpPercent = 95;
    static constexpr int kDefaultJumpPercent = 90;
    static constexpr int kMinimumClientWidth = 480;
    static constexpr int kMinimumClientHeight = 580;

    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND targetCombo_ = nullptr;
    HWND previewCanvas_ = nullptr;
    HWND status_ = nullptr;
    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND loggingCheckbox_ = nullptr;
    HWND openLogsButton_ = nullptr;
    HWND latestReleaseLink_ = nullptr;
    HWND maximumJumpSlider_ = nullptr;
    HWND maximumJumpValue_ = nullptr;
    HWND scrollbarCandidateCombo_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
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
    bool calibrationReady_ = false;
    bool exported_ = false;
    bool recovering_ = false;
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
            if (HIWORD(wParam) == EN_CHANGE) {
                const int id = LOWORD(wParam);
                if (!updatingCalibrationEdits_ && id >= kTrackX && id <= kTrackHeight) {
                    selectedScrollbarCandidate_.reset();
                    if (scrollbarCandidateCombo_) {
                        SendMessageW(scrollbarCandidateCombo_, CB_SETCURSEL,
                                     static_cast<WPARAM>(-1), 0);
                    }
                    const Rect track = readRect(trackEdits_);
                    if (!preview_.empty() && track.valid()) {
                        scrollbarSide_ = track.x + track.width / 2 < preview_.cols / 2
                            ? ScrollbarSide::Left : ScrollbarSide::Right;
                    }
                    logger_.info("calibration", "manual scrollbar track edit changed to{" +
                                 rectDescription(track) + "}");
                } else if (!updatingCalibrationEdits_ && id >= kViewportX && id <= kViewportHeight) {
                    markViewportCustomized();
                    logger_.info("calibration", "manual viewport edit changed to{" +
                                 rectDescription(readRect(viewportEdits_)) + "}");
                }
                InvalidateRect(previewCanvas_, nullptr, FALSE);
                if (!updatingCalibrationEdits_) refreshWorkflow();
            }
            if (HIWORD(wParam) == EN_SETFOCUS &&
                ((LOWORD(wParam) >= kViewportX && LOWORD(wParam) <= kViewportHeight) ||
                 (LOWORD(wParam) >= kTrackX && LOWORD(wParam) <= kTrackHeight))) rememberCalibration();
            if (LOWORD(wParam) == kTargetCombo && HIWORD(wParam) == CBN_SELCHANGE) {
                logger_.info("target", "selected " + windowDescription(selectedTarget()));
                capturePreview();
            }
            return 0;
        case WM_VSCROLL:
            scrollControls(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_MOUSEWHEEL:
            scrollControls(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? SB_LINEUP : SB_LINEDOWN);
            return 0;
        case WM_DPICHANGED: {
            uiDpi_ = HIWORD(wParam);
            updateFonts();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            RECT minimum{0, 0, px(kMinimumClientWidth), px(kMinimumClientHeight)};
            AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                                    FALSE, 0, uiDpi_);
            RECT bounds = *suggested;
            int width = std::max<int>(bounds.right - bounds.left, minimum.right - minimum.left);
            int height = std::max<int>(bounds.bottom - bounds.top, minimum.bottom - minimum.top);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromRect(&bounds, MONITOR_DEFAULTTONEAREST), &monitor)) {
                width = std::min<int>(width, monitor.rcWork.right - monitor.rcWork.left);
                height = std::min<int>(height, monitor.rcWork.bottom - monitor.rcWork.top);
                bounds.left = std::clamp<int>(bounds.left, monitor.rcWork.left, monitor.rcWork.right - width);
                bounds.top = std::clamp<int>(bounds.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
            }
            SetWindowPos(window_, nullptr, bounds.left, bounds.top, width, height,
                SWP_NOZORDER | SWP_NOACTIVATE);
            RECT client{};
            GetClientRect(window_, &client);
            layoutControls(client.right, client.bottom);
            return 0;
        }
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == maximumJumpSlider_) {
                updateMaximumJumpLabel();
                logger_.info("capture", "maximum single-frame jump changed percent=" +
                             std::to_string(maximumJumpPercent()));
                return 0;
            }
            return DefWindowProcW(window_, message, wParam, lParam);
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            UINT dpi = GetDpiForWindow(window_);
            if (dpi == 0) dpi = USER_DEFAULT_SCREEN_DPI;
            RECT desiredClient{0, 0, MulDiv(kMinimumClientWidth, dpi, 96), MulDiv(kMinimumClientHeight, dpi, 96)};
            RECT desiredWindow = desiredClient;
            if (!AdjustWindowRectExForDpi(&desiredWindow, WS_OVERLAPPEDWINDOW | WS_VSCROLL, FALSE, 0, dpi)) {
                AdjustWindowRectEx(&desiredWindow, WS_OVERLAPPEDWINDOW | WS_VSCROLL, FALSE, 0);
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
            }
            return 0;
        case WM_CTLCOLORSTATIC:
            return colorStatic(reinterpret_cast<HWND>(lParam), reinterpret_cast<HDC>(wParam));
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(235, 240, 248));
            SetBkColor(dc, RGB(31, 36, 45));
            return reinterpret_cast<LRESULT>(cardBrush_);
        }
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
            if (previewCanvas_) DestroyWindow(previewCanvas_);
            logger_.info("app", "shutdown");
            logger_.disable();
            if (headingFont_) DeleteObject(headingFont_);
            if (bodyFont_) DeleteObject(bodyFont_);
            if (backgroundBrush_) DeleteObject(backgroundBrush_);
            if (cardBrush_) DeleteObject(cardBrush_);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window_, message, wParam, lParam);
        }
    }

#include "ui/MainControls.inc"

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

    int maximumJumpPercent() const {
        if (!maximumJumpSlider_) return kDefaultJumpPercent;
        return std::clamp(static_cast<int>(SendMessageW(
            maximumJumpSlider_, TBM_GETPOS, 0, 0)), kMinimumJumpPercent, kMaximumJumpPercent);
    }

    void updateMaximumJumpLabel() const {
        if (!maximumJumpValue_) return;
        const int jump = maximumJumpPercent();
        const std::wstring text = std::to_wstring(jump) + L"% (" +
            std::to_wstring(100 - jump) + L"% overlap)";
        SetWindowTextW(maximumJumpValue_, text.c_str());
    }

    static std::wstring timestampedCaptureFilename() {
        SYSTEMTIME localTime{};
        GetLocalTime(&localTime);
        std::wostringstream filename;
        filename << std::setfill(L'0')
                 << std::setw(4) << localTime.wYear << L'-'
                 << std::setw(2) << localTime.wMonth << L'-'
                 << std::setw(2) << localTime.wDay << L'_'
                 << std::setw(2) << localTime.wHour << L'-'
                 << std::setw(2) << localTime.wMinute << L'-'
                 << std::setw(2) << localTime.wSecond << L".png";
        return filename.str();
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
        const HWND previousTarget = selectedTarget();
        windows_.clear();
        std::pair<MainWindow*, std::vector<WindowInfo>*> context{this, &windows_};
        EnumWindows(&MainWindow::enumerateWindow, reinterpret_cast<LPARAM>(&context));
        SendMessageW(targetCombo_, CB_RESETCONTENT, 0, 0);
        for (const auto& entry : windows_) {
            const LRESULT index = SendMessageW(targetCombo_, CB_ADDSTRING, 0,
                                               reinterpret_cast<LPARAM>(entry.title.c_str()));
            SendMessageW(targetCombo_, CB_SETITEMDATA, index, reinterpret_cast<LPARAM>(entry.handle));
            if (entry.handle == previousTarget) SendMessageW(targetCombo_, CB_SETCURSEL, index, 0);
            logger_.info("target", "discovered " + windowDescription(entry.handle));
        }
        logger_.info("target", "discovered window count=" + std::to_string(windows_.size()));
        if (previousTarget && !selectedTarget()) {
            calibrationReady_ = false;
            preview_.release();
            source_.reset();
            resetPreviewInteraction();
        }
        setStatus(windows_.empty() ? L"Open the app you want to capture, then click Refresh."
            : (selectedTarget() ? L"Window list refreshed. Your current selection is unchanged."
                                : L"Choose a window above. We will find its scrollbar for you."));
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
        rememberCalibration();
        scrollbarCandidates_.clear();
        selectedScrollbarCandidate_.reset();
        refreshScrollbarCandidateCombo();
        scrollbarSide_ = profile.scrollbar.side;
        setRect(viewportEdits_, profile.content);
        setRect(trackEdits_, profile.scrollbar.track);
        markViewportCustomized();
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
                ? L"Template loaded and scaled to this window. Check both areas in the preview."
                : L"Template loaded. Your capture area and scrollbar track are restored.");
        }
        return true;
    }

#include "ui/PreviewControls.inc"

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

    bool sourceGeometryChanged() const {
        if (!source_ || preview_.empty()) return false;
        if (dynamic_cast<GdiCaptureSource*>(source_.get())) {
            RECT client{};
            return GetClientRect(sourceTarget_, &client) &&
                (client.right != preview_.cols || client.bottom != preview_.rows);
        }
        return source_->width() != preview_.cols || source_->height() != preview_.rows;
    }

    void capturePreview() {
        logger_.info("preview", "preview capture requested");
        if (uiStage() == UiStage::Capture || uiStage() == UiStage::Ready) {
            logger_.warning("preview", "preview rejected while capture is active");
            setStatus(L"Stop the current capture before recapturing a preview.");
            return;
        }
        calibrationReady_ = false;
        exported_ = false;
        recovering_ = false;
        resetPreviewInteraction();
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
        refreshScrollbarCandidateCombo();
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
            setStatus(L"No frame received yet. Make the target visible, then click Retry preview.");
            return;
        }
        preview_ = std::move(captured);
        calibrationReady_ = true;
        ShowWindow(previewCanvas_, SW_SHOWNOACTIVATE);
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
        // Keep every distinct result available in the dropdown, while the
        // preview itself draws only the currently selected rectangle.
        scrollbarCandidates_ = detectedCandidates;
        refreshScrollbarCandidateCombo();
        if (!scrollbarCandidates_.empty()) {
            historyRestoreInProgress_ = true;
            selectScrollbarCandidate(0, false);
            historyRestoreInProgress_ = false;
        } else {
            setRect(viewportEdits_, {0, 0, preview_.cols, preview_.rows});
            setRect(trackEdits_, {std::max(0, preview_.cols - 18), 0, 18, preview_.rows});
        }
        if (pendingCalibrationProfile_) {
            const CalibrationProfile loaded = *pendingCalibrationProfile_;
            pendingCalibrationProfile_.reset();
            if (applyCalibrationProfile(loaded, true)) return;
        }
        std::wstring message;
        if (!scrollbarCandidates_.empty()) {
            message = L"Scrollbar found. Check the capture area in the preview, then start.";
        } else {
            message = L"No scrollbar found. In the preview, choose Scrollbar, then Draw new around its full track.";
        }
        setStatus(message);
        ShowWindow(previewCanvas_, SW_SHOW);
        InvalidateRect(previewCanvas_, nullptr, TRUE);
    }

    void autoDetectScrollbar() {
        if (uiStage() == UiStage::Capture || uiStage() == UiStage::Ready) return;
        logger_.info("calibration", "auto-detect scrollbar requested");
        if (preview_.empty()) capturePreview();
        if (preview_.empty()) return;
        rememberCalibration();
        const auto detectedCandidates = ScrollbarDetector::autoDetectAll(preview_);
        logger_.info("calibration", "auto-detect completed candidates=" + std::to_string(detectedCandidates.size()));
        scrollbarCandidates_.clear();
        selectedScrollbarCandidate_.reset();
        if (detectedCandidates.empty()) {
            refreshScrollbarCandidateCombo();
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
        scrollbarCandidates_ = detectedCandidates;
        refreshScrollbarCandidateCombo();
        historyRestoreInProgress_ = true;
        selectScrollbarCandidate(0, false);
        historyRestoreInProgress_ = false;
        setStatus(L"Detection refreshed. Use the preview's scrollbar chooser if the selected track is wrong.");
    }

    void startCapture() {
        logger_.info("capture", "start requested");
        if (uiStage() != UiStage::Calibrate) return;
        if (!validCalibration()) {
            setStatus(L"Keep both areas inside the preview, with the capture area aligned to the scrollbar's travel.");
            return;
        }
        if (preview_.empty() || selectedTarget() != sourceTarget_) capturePreview();
        if (!ensureSource()) return;
        if (sourceGeometryChanged()) {
            calibrationReady_ = false;
            resetPreviewInteraction();
            setStatus(L"The target changed size. Click Retry preview before starting.");
            return;
        }
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
            calibrationReady_ = false;
            resetPreviewInteraction();
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
        const int maximumJump = maximumJumpPercent();
        options.estimator.minimumOverlapRatio =
            static_cast<float>(100 - maximumJump) / 100.0F;
        logger_.info("capture", "start options viewport{" + rectDescription(options.viewport) +
                     "} scrollbar{" + rectDescription(options.scrollbar.track) + "} preview=" +
                     std::to_string(preview_.cols) + "x" + std::to_string(preview_.rows) +
                     " maximum_jump_percent=" + std::to_string(maximumJump) +
                     " minimum_overlap_ratio=" + std::to_string(options.estimator.minimumOverlapRatio));
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
        recovering_ = false;
        exported_ = false;
        scrollOffset_ = 0;
        EnableWindow(maximumJumpSlider_, FALSE);
        captureTickCount_ = 0;
        transientMissCount_ = 0;
        logger_.info("capture", "capture started");
        SetTimer(window_, kTimer, kTimerPeriodMs, nullptr);
        setStatus(L"Scroll down in your target window. Keep its size unchanged until you stop.");
    }

    void stopCapture() {
        logger_.info("capture", "stop requested capturing=" + std::to_string(capturing_ ? 1 : 0));
        if (!capturing_ && session_.state() != SessionState::Paused &&
            session_.state() != SessionState::Capturing) return;
        capturing_ = false;
        recovering_ = false;
        scrollOffset_ = 0;
        EnableWindow(maximumJumpSlider_, TRUE);
        if (session_.finish()) {
            logger_.info("capture", "capture finalized accepted_frames=" + std::to_string(session_.acceptedFrames()) +
                         " output_rows=" + std::to_string(session_.outputRows()));
            setStatus(L"Your capture is ready. Export it as PNG or JPEG.");
        } else {
            logger_.error("capture", "capture finalize failed: " + session_.lastMessage());
            setStatus(widen(session_.lastMessage()));
        }
    }

    void captureTick() {
        if (!source_) return;
        if (!IsWindow(sourceTarget_) || sourceGeometryChanged()) {
            const bool unfinished = uiStage() == UiStage::Capture;
            capturing_ = false;
            calibrationReady_ = false;
            resetPreviewInteraction();
            KillTimer(window_, kTimer);
            setStatus(unfinished
                ? L"The target closed or changed size. Finish and keep the content captured so far."
                : (session_.hasOutput() ? L"The target changed. Your finished capture is still ready to export."
                                        : L"The target closed or changed size. Choose a window or click Retry preview."));
            return;
        }
        if (!capturing_) {
            const auto frame = source_->capture();
            if (!frame) return;
            if (!preview_.empty() && (frame->width() != preview_.cols || frame->height() != preview_.rows)) {
                resetPreviewInteraction();
                calibrationReady_ = false;
                // Finalized output remains exportable even if its source resizes.
                if (!session_.hasOutput() && uiStage() != UiStage::Capture) {
                    preview_ = frame->bgra;
                    setStatus(L"The target changed size. Click Retry preview to set its capture area again.");
                    KillTimer(window_, kTimer);
                }
                return;
            }
            if (previewInteractionActive()) return;
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
                EnableWindow(maximumJumpSlider_, TRUE);
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
                EnableWindow(maximumJumpSlider_, TRUE);
                recovering_ = false;
                setStatus(L"Overlap could not be restored. Finish and export the content captured so far.");
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
                EnableWindow(maximumJumpSlider_, TRUE);
                KillTimer(window_, kTimer);
                setStatus(L"The target window closed; click Stop to finalize the valid prefix.");
                logger_.error("capture", "target window closed during capture");
            } else if (source_->width() != preview_.cols || source_->height() != preview_.rows) {
                capturing_ = false;
                EnableWindow(maximumJumpSlider_, TRUE);
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
            recovering_ = false;
            setStatus(lastUpdate.atBottom ? L"Bottom reached. Stop capture when all the content you want is visible."
                                         : L"Looking good. Keep scrolling down in your target window.");
            return;
        }
        // The session is still live and still holding the last accepted frame,
        // so the capture keeps running while the user recovers the overlap.
        if (lastUpdate.rejected) {
            recovering_ = true;
            setStatus(L"Scroll up slightly to reconnect. Capture will continue when the overlap returns.");
        }
    }

    void saveTemplate() {
        logger_.info("calibration", "save template requested");
        if (uiStage() == UiStage::Capture) {
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
        if (uiStage() == UiStage::Capture || uiStage() == UiStage::Ready) {
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
            setStatus(L"Template loaded. Choose a target window to restore its capture area.");
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
        wchar_t filename[MAX_PATH]{};
        const std::wstring defaultFilename = timestampedCaptureFilename();
        wcsncpy_s(filename, defaultFilename.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"PNG image (*.png)\0*.png\0JPEG image (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
        dialog.lpstrDefExt = L"png";
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
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
            if ((!png && lower == L".png") || (lower != L".png" && lower != L".jpg" && lower != L".jpeg")) {
                outputPath.replace_extension(png ? L".png" : L".jpg");
            }
        }
        // The selected format can change the suggested extension after the
        // native dialog's overwrite check. Confirm that final path as well.
        if (outputPath != std::filesystem::path(filename) && std::filesystem::exists(outputPath) &&
            MessageBoxW(window_, (L"Replace the existing file?\n" + outputPath.wstring()).c_str(),
                L"Export image", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        const ExportResult result = ImageExporter::write(session_.outputStore(), outputPath);
        logger_.info("export", "export path requested=" + narrow(outputPath.wstring()) +
                     " success=" + std::to_string(result.success ? 1 : 0) +
                     " files=" + std::to_string(result.files.size()));
        if (!result.success) {
            logger_.error("export", "export failed: " + result.message);
            setStatus(widen(result.message));
            return;
        }
        exported_ = true;
        setStatus(L"Saved " + std::to_wstring(result.files.size()) + L" file(s). You can export again or start a new capture.");
    }

    void newCapture() {
        if (uiStage() != UiStage::Ready) return;
        if (session_.hasOutput() && !exported_ && MessageBoxW(window_,
            L"This capture has not been exported. Discard it and start a new capture?",
            L"Keep your capture?", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        session_.reset();
        exported_ = false;
        recovering_ = false;
        scrollOffset_ = 0;
        setStatus(L"Your areas are still set. Adjust them if needed, then start another capture.");
    }

    void onButton(int id) {
        switch (id) {
        case kRefresh: refreshTargets(); break;
        case kPreview:
            ShowWindow(previewCanvas_, SW_SHOWNORMAL);
            SetForegroundWindow(previewCanvas_);
            break;
        case kRetryPreview: capturePreview(); break;
        case kAutoDetect: autoDetectScrollbar(); break;
        case kSetScrollbarCandidate: setScrollbarCandidateFromCombo(); break;
        case kStart: startCapture(); break;
        case kStop: stopCapture(); break;
        case kExport: exportImage(); break;
        case kSaveTemplate: saveTemplate(); break;
        case kLoadTemplate: loadTemplate(); break;
        case kLoggingEnable: toggleLogging(); break;
        case kOpenLogs: openLogs(); break;
        case kNewCapture: newCapture(); break;
        case kAdvanced:
            advancedOpen_ = !advancedOpen_;
            refreshWorkflow();
            if (advancedOpen_) scrollControls(SB_LINEDOWN);
            break;
        case kLatestRelease:
            ShellExecuteW(window_, L"open", kLatestReleaseUrl, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        default: break;
        }
    }

    void setStatus(const std::wstring& text) {
        std::wstring visibleText = text;
        const std::string loggingError = logger_.lastError();
        if (!loggingError.empty()) {
            visibleText += L"\nLogging warning: " + widen(loggingError);
        }
        if (visibleText != lastStatus_) {
            lastStatus_ = visibleText;
            if (status_) SetWindowTextW(status_, visibleText.c_str());
            logger_.info("status", narrow(text));
        }
        refreshWorkflow();
    }
};

} // namespace
} // namespace universal_stitcher

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&commonControls);
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
        if (app.translateUiMessage(message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
